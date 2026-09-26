#include "Server.h"

#include <atomic>
#include <thread>

#include "capture/DesktopDuplicator.h"
#include "capture/OutputLocator.h"
#include "common/Clock.h"
#include "common/Log.h"
#include "display/VirtualDisplay.h"
#include "encode/AnnexB.h"
#include "encode/MfH264Encoder.h"
#include "render/FrameComposer.h"
#include "transport/Tcp.h"

using Microsoft::WRL::ComPtr;

namespace dd
{
namespace
{

using protocol::MessageType;

// Everything that has to be rebuilt when the display mode changes or the GPU is reset.
struct Pipeline
{
    capture::LocatedOutput output;
    ComPtr<ID3D11Device> device;
    capture::DesktopDuplicator duplicator;
    render::FrameComposer composer;
    encode::MfH264Encoder encoder;
    UINT width = 0;
    UINT height = 0;
    UINT fps = 60;

    HRESULT Initialize(unsigned bitrateKbps)
    {
        encoder.Shutdown();
        device.Reset();
        if (!capture::FindVirtualOutput(output))
        {
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }

        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        HRESULT hr = D3D11CreateDevice(output.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, nullptr, 0,
                                       D3D11_SDK_VERSION, &device, nullptr, nullptr);
        if (FAILED(hr))
        {
            return hr;
        }

        hr = duplicator.Initialize(device.Get(), output.output.Get());
        if (FAILED(hr))
        {
            return hr;
        }
        const DXGI_OUTDUPL_DESC desc = duplicator.Desc();
        width = desc.ModeDesc.Width;
        height = desc.ModeDesc.Height;
        const auto& rate = desc.ModeDesc.RefreshRate;
        fps = rate.Denominator ? (rate.Numerator + rate.Denominator / 2) / rate.Denominator : 60;
        if (fps == 0)
        {
            fps = 60;
        }

        hr = composer.Initialize(device.Get(), width, height);
        if (FAILED(hr))
        {
            return hr;
        }

        DXGI_ADAPTER_DESC1 adapterDesc{};
        output.adapter->GetDesc1(&adapterDesc);
        hr = encoder.Initialize(device.Get(), adapterDesc.AdapterLuid, {width, height, fps, bitrateKbps});
        if (FAILED(hr))
        {
            return hr;
        }
        Log(L"pipeline: %s %ux%u@%u, encoder \"%s\"", output.deviceName.c_str(), width, height, fps,
            encoder.Name().c_str());
        return S_OK;
    }
};

bool SendConfig(transport::Connection& conn, const Pipeline& p, unsigned bitrateKbps)
{
    protocol::Config config{protocol::kVersion, protocol::Codec::H264, static_cast<uint16_t>(p.width),
                            static_cast<uint16_t>(p.height), p.fps * 1000, bitrateKbps};
    const auto payload = protocol::SerializeConfig(config);
    return conn.Send(MessageType::Config, 0, NowMicros(), payload);
}

struct SessionStats
{
    uint64_t windowStartUs = 0;
    unsigned frames = 0;
    uint64_t bytes = 0;
    uint64_t pipelineUs = 0;

    void Add(size_t size, uint64_t processingUs)
    {
        ++frames;
        bytes += size;
        pipelineUs += processingUs;
    }

    void MaybeLog()
    {
        const uint64_t now = NowMicros();
        if (windowStartUs == 0)
        {
            windowStartUs = now;
        }
        const uint64_t elapsed = now - windowStartUs;
        if (elapsed < 2'000'000)
        {
            return;
        }
        if (frames > 0)
        {
            Log(L"stream: %.1f fps, %.1f Mbit/s, capture->sent %.2f ms avg", frames * 1e6 / elapsed,
                bytes * 8.0 / elapsed, pipelineUs / 1000.0 / frames);
        }
        *this = {};
        windowStartUs = now;
    }
};

void RunSession(transport::Connection& conn, const ServeOptions& options)
{
    protocol::Header header;
    std::vector<uint8_t> payload;
    if (!conn.Receive(header, payload) || header.type != MessageType::Hello)
    {
        Log(L"session: expected HELLO");
        return;
    }
    const auto hello = protocol::ParseHello(payload);
    if (!hello || !(hello->codecs & protocol::kCodecMaskH264))
    {
        Log(L"session: unsupported client (bad HELLO or no H.264)");
        conn.Send(MessageType::Bye, 0, NowMicros(), {});
        return;
    }
    Log(L"session: client \"%S\" %ux%u @ %.1f Hz, %u dpi", hello->deviceName.c_str(), hello->widthPx,
        hello->heightPx, hello->refreshMilliHz / 1000.0, hello->densityDpi);

    Pipeline pipeline;
    HRESULT hr = pipeline.Initialize(options.bitrateKbps);
    if (FAILED(hr))
    {
        Log(L"session: pipeline init failed 0x%08lX", Hr(hr));
        conn.Send(MessageType::Bye, 0, NowMicros(), {});
        return;
    }
    if (!SendConfig(conn, pipeline, options.bitrateKbps))
    {
        return;
    }

    std::atomic<bool> stop{false};
    std::atomic<bool> keyframeRequested{false};
    std::thread reader([&] {
        protocol::Header h;
        std::vector<uint8_t> p;
        while (!stop && conn.Receive(h, p))
        {
            switch (h.type)
            {
            case MessageType::RequestKeyframe:
                keyframeRequested = true;
                break;
            case MessageType::Ping:
                if (auto id = protocol::ParsePing(p))
                {
                    const auto pong = protocol::SerializePong({*id, h.timestamp});
                    conn.Send(MessageType::Pong, 0, NowMicros(), pong);
                }
                break;
            case MessageType::Bye:
                stop = true;
                break;
            default:
                break; // FRAME_STATS etc. are consumed by M4 tooling
            }
        }
        stop = true;
    });

    SessionStats stats;
    std::vector<uint8_t> lastCodecConfig;
    std::vector<encode::EncodedFrame> encoded;
    bool haveFrame = false;

    auto sendEncoded = [&](const encode::EncodedFrame& frame) {
        auto split = annexb::SeparateParameterSets(frame.data);
        if (!split.codecConfig.empty() && split.codecConfig != lastCodecConfig)
        {
            if (!conn.Send(MessageType::VideoFrame, protocol::video_flags::kCodecConfig, frame.timestampUs,
                           split.codecConfig))
            {
                return false;
            }
            lastCodecConfig = std::move(split.codecConfig);
        }
        const uint8_t flags = (split.keyframe || frame.keyframe) ? protocol::video_flags::kKeyframe : 0;
        if (!conn.Send(MessageType::VideoFrame, flags, frame.timestampUs, split.frame))
        {
            return false;
        }
        stats.Add(split.frame.size(), NowMicros() - frame.timestampUs);
        return true;
    };

    while (!stop)
    {
        if (keyframeRequested.exchange(false))
        {
            pipeline.encoder.RequestKeyframe();
            if (haveFrame)
            {
                // Static desktop: nothing new will arrive, so re-encode the last image now.
                ComPtr<ID3D11Texture2D> nv12;
                if (SUCCEEDED(pipeline.composer.ConvertLast(&nv12)) &&
                    SUCCEEDED(pipeline.encoder.Encode(nv12.Get(), NowMicros(), encoded)))
                {
                    for (const auto& f : encoded)
                    {
                        if (!sendEncoded(f))
                        {
                            stop = true;
                        }
                    }
                }
            }
        }

        capture::Frame frame;
        hr = pipeline.duplicator.AcquireFrame(100, frame);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT)
        {
            stats.MaybeLog();
            continue;
        }
        if (FAILED(hr))
        {
            // ACCESS_LOST: mode change, lock screen, UAC, GPU reset. Rebuild everything.
            Log(L"capture: 0x%08lX, reinitializing", Hr(hr));
            const UINT oldWidth = pipeline.width;
            const UINT oldHeight = pipeline.height;
            const UINT oldFps = pipeline.fps;
            while (!stop && FAILED(pipeline.Initialize(options.bitrateKbps)))
            {
                Sleep(250);
            }
            haveFrame = false;
            lastCodecConfig.clear();
            if (pipeline.width != oldWidth || pipeline.height != oldHeight || pipeline.fps != oldFps)
            {
                if (!SendConfig(conn, pipeline, options.bitrateKbps))
                {
                    break;
                }
            }
            continue;
        }
        if (!frame.desktopUpdated && !frame.pointerUpdated)
        {
            continue;
        }

        const uint64_t captureUs = frame.presentQpc ? QpcToMicros(frame.presentQpc) : NowMicros();
        ComPtr<ID3D11Texture2D> nv12;
        hr = pipeline.composer.Compose(frame.desktop, pipeline.duplicator.Pointer(), &nv12);
        pipeline.duplicator.ReleaseFrame();
        if (FAILED(hr))
        {
            Log(L"compose failed 0x%08lX", Hr(hr));
            continue;
        }
        haveFrame = true;

        hr = pipeline.encoder.Encode(nv12.Get(), captureUs, encoded);
        if (FAILED(hr))
        {
            Log(L"encode failed 0x%08lX", Hr(hr));
            continue;
        }
        for (const auto& f : encoded)
        {
            if (!sendEncoded(f))
            {
                stop = true;
                break;
            }
        }
        stats.MaybeLog();
    }

    stop = true;
    conn.Shutdown();
    reader.join();
    Log(L"session: ended");
}

} // namespace

int Serve(const ServeOptions& options)
{
    transport::WinsockScope winsock;
    if (!winsock.Ok())
    {
        Log(L"WSAStartup failed");
        return 1;
    }

    VirtualDisplay display;
    if (options.createDisplay)
    {
        HRESULT hr = display.Create();
        if (FAILED(hr))
        {
            Log(L"Creating the virtual display failed 0x%08lX. Is the driver installed? See driver/README.md.",
                Hr(hr));
            return 1;
        }
    }

    capture::LocatedOutput output;
    for (int i = 0; i < 50 && !capture::FindVirtualOutput(output); ++i)
    {
        Sleep(200);
    }
    if (!output.output)
    {
        Log(L"The DeuxDisplay monitor did not appear (looking for %s). Is it enabled in Display Settings?",
            capture::kMonitorHardwareId);
        return 1;
    }
    Log(L"virtual monitor: %s", output.deviceName.c_str());

    transport::Listener listener;
    if (!listener.Listen(options.port))
    {
        Log(L"listen on 127.0.0.1:%u failed (%d)", options.port, WSAGetLastError());
        return 1;
    }

    for (;;)
    {
        Log(L"waiting for client on 127.0.0.1:%u", options.port);
        auto conn = listener.Accept();
        if (!conn)
        {
            Log(L"accept failed (%d)", WSAGetLastError());
            return 1;
        }
        RunSession(*conn, options);
    }
}

} // namespace dd
