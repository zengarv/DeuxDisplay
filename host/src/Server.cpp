#include "Server.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>

#include "capture/DesktopDuplicator.h"
#include "capture/OutputLocator.h"
#include "common/Clock.h"
#include "common/Log.h"
#include "display/VirtualDisplay.h"
#include "encode/AnnexB.h"
#include "encode/MfH264Encoder.h"
#include "input/TouchInjector.h"
#include "protocol/Pairing.h"
#include "render/FrameComposer.h"
#include "transport/Tcp.h"
#include "wireless/PairingStore.h"
#include "wireless/WifiDirectAp.h"
#include "wireless/WlanTuning.h"

#include <optional>
#include <string>

using Microsoft::WRL::ComPtr;

namespace dd
{
namespace
{

using protocol::MessageType;

enum class Transport
{
    Usb,
    Wifi,
};

const wchar_t* TransportName(Transport transport)
{
    return transport == Transport::Wifi ? L"Wi-Fi" : L"USB";
}

// The host's pairing code and what it derives (docs/wire-protocol.md, "Pairing").
struct PairingContext
{
    std::string code; // normalized
    protocol::PairingSecrets secrets;
    std::string hostName;
};

// Everything that has to be rebuilt when the display mode changes or the GPU is reset.
struct Pipeline
{
    capture::LocatedOutput output;
    ComPtr<ID3D11Device> device;
    capture::DesktopDuplicator duplicator;
    render::FrameComposer composer;
    encode::MfH264Encoder encoder;
    RECT desktopRect{}; // where the output sits on the Windows desktop (for touch)
    UINT width = 0; // desktop (virtual monitor) size
    UINT height = 0;
    UINT streamWidth = 0; // encoded size
    UINT streamHeight = 0;
    bool hevc = false;
    UINT fps = 60;

    HRESULT Initialize(const ServeOptions& options, bool preferHevc)
    {
        const unsigned bitrateKbps = options.bitrateKbps;
        encoder.Shutdown();
        device.Reset();
        const bool found = options.outputName.empty() ? capture::FindVirtualOutput(output)
                                                      : capture::FindOutputByName(options.outputName, output);
        if (!found)
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
        DXGI_OUTPUT_DESC outputDesc{};
        hr = output.output->GetDesc(&outputDesc);
        if (FAILED(hr))
        {
            return hr;
        }
        desktopRect = outputDesc.DesktopCoordinates;

        const DXGI_OUTDUPL_DESC desc = duplicator.Desc();
        width = desc.ModeDesc.Width;
        height = desc.ModeDesc.Height;
        const auto& rate = desc.ModeDesc.RefreshRate;
        fps = rate.Denominator ? (rate.Numerator + rate.Denominator / 2) / rate.Denominator : 60;
        if (fps == 0)
        {
            fps = 60;
        }
        fps = std::min(fps, options.maxFps); // the encoder's rate control budgets per frame

        double scale = 1.0;
        if (options.maxStreamWidth && options.maxStreamHeight)
        {
            scale = std::min({1.0, static_cast<double>(options.maxStreamWidth) / width,
                              static_cast<double>(options.maxStreamHeight) / height});
        }
        streamWidth = static_cast<UINT>(width * scale) & ~1u;
        streamHeight = static_cast<UINT>(height * scale) & ~1u;

        hr = composer.Initialize(device.Get(), width, height, streamWidth, streamHeight);
        if (FAILED(hr))
        {
            return hr;
        }

        DXGI_ADAPTER_DESC1 adapterDesc{};
        output.adapter->GetDesc1(&adapterDesc);
        encode::EncoderSettings settings{streamWidth, streamHeight, fps, bitrateKbps};
        settings.hevc = preferHevc;
        hr = encoder.Initialize(device.Get(), adapterDesc.AdapterLuid, settings);
        if (FAILED(hr) && preferHevc)
        {
            Log(L"pipeline: HEVC encoder unavailable (0x%08lX), using H.264", Hr(hr));
            settings.hevc = false;
            hr = encoder.Initialize(device.Get(), adapterDesc.AdapterLuid, settings);
        }
        if (FAILED(hr))
        {
            return hr;
        }
        hevc = settings.hevc;
        Log(L"pipeline: %s %ux%u@%u -> stream %ux%u %s, encoder \"%s\"", output.deviceName.c_str(), width, height,
            fps, streamWidth, streamHeight, hevc ? L"HEVC" : L"H.264", encoder.Name().c_str());
        return S_OK;
    }
};

// The stream format the user picked on the client (HELLO mode_* fields); 0 = host decides.
struct RequestedMode
{
    uint16_t width = 0;
    uint16_t height = 0;
    unsigned hz = 0;
};

RequestedMode RequestedModeFor(const protocol::Hello& hello)
{
    RequestedMode mode;
    if (hello.modeWidthPx && hello.modeHeightPx)
    {
        driver::PlugRequest probe;
        probe.width = std::max(hello.modeWidthPx, hello.modeHeightPx); // landscape only in v1
        probe.height = std::min(hello.modeWidthPx, hello.modeHeightPx);
        probe.refreshHz[0] = 60;
        if (driver::IsValidPlugRequest(probe))
        {
            mode.width = probe.width;
            mode.height = probe.height;
        }
        else
        {
            Log(L"session: ignoring requested size %ux%u", hello.modeWidthPx, hello.modeHeightPx);
        }
    }
    if (hello.modeRefreshMilliHz)
    {
        const long hz = std::lround(hello.modeRefreshMilliHz / 1000.0);
        if (hz >= 24 && hz <= 240)
        {
            mode.hz = static_cast<unsigned>(hz);
        }
        else
        {
            Log(L"session: ignoring requested refresh rate %.3f Hz", hello.modeRefreshMilliHz / 1000.0);
        }
    }
    return mode;
}

// Turns what the client reported into a monitor description for the driver.
driver::PlugRequest PlugRequestFor(const protocol::Hello& hello, unsigned maxFps, const RequestedMode& mode)
{
    uint16_t width = hello.widthPx;
    uint16_t height = hello.heightPx;
    double fallbackDpi = hello.densityDpi ? hello.densityDpi : 160.0;
    double xdpi = hello.xdpiMilli ? hello.xdpiMilli / 1000.0 : fallbackDpi;
    double ydpi = hello.ydpiMilli ? hello.ydpiMilli / 1000.0 : fallbackDpi;
    if (width < height)
    {
        // v1 streams landscape only (the client locks orientation).
        std::swap(width, height);
        std::swap(xdpi, ydpi);
    }

    driver::PlugRequest r;
    // Physical size always comes from the native panel, so Windows scaling stays right for the
    // real screen even when the user picked a lower stream resolution.
    r.widthMm = static_cast<uint16_t>(std::lround(width / xdpi * 25.4));
    r.heightMm = static_cast<uint16_t>(std::lround(height / ydpi * 25.4));
    r.width = mode.width ? mode.width : static_cast<uint16_t>(width & ~1u);
    r.height = mode.height ? mode.height : static_cast<uint16_t>(height & ~1u);

    if (mode.hz)
    {
        // Only the picked rate, so Windows can't choose another one.
        r.refreshHz[0] = static_cast<uint16_t>(mode.hz);
        return r;
    }

    long hz = std::lround(hello.refreshMilliHz / 1000.0);
    if (hz < 24 || hz > 240)
    {
        hz = 60;
    }
    // Prefer a mode no faster than we'll stream; keep the client's rate available as an option.
    const long preferred = std::min<long>(hz, static_cast<long>(maxFps));
    r.refreshHz[0] = static_cast<uint16_t>(preferred);
    if (hz != preferred)
    {
        r.refreshHz[1] = static_cast<uint16_t>(hz);
    }
    return r;
}

// Sleeps with ~0.5 ms precision (plain Sleep() rounds up to the 15.6 ms timer tick).
class PreciseSleeper
{
  public:
    PreciseSleeper()
        : m_timer(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS))
    {
    }
    ~PreciseSleeper()
    {
        if (m_timer)
        {
            CloseHandle(m_timer);
        }
    }
    PreciseSleeper(const PreciseSleeper&) = delete;
    PreciseSleeper& operator=(const PreciseSleeper&) = delete;

    void SleepMicros(uint64_t us)
    {
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(us * 10); // relative, 100 ns units
        if (m_timer && SetWaitableTimerEx(m_timer, &due, 0, nullptr, nullptr, nullptr, 0))
        {
            WaitForSingleObject(m_timer, INFINITE);
        }
        else
        {
            Sleep(static_cast<DWORD>((us + 999) / 1000));
        }
    }

  private:
    HANDLE m_timer;
};

bool SendConfig(transport::Connection& conn, const Pipeline& p, unsigned bitrateKbps)
{
    protocol::Config config{protocol::kVersion, p.hevc ? protocol::Codec::Hevc : protocol::Codec::H264,
                            static_cast<uint16_t>(p.streamWidth),
                            static_cast<uint16_t>(p.streamHeight), p.fps * 1000, bitrateKbps};
    const auto payload = protocol::SerializeConfig(config);
    return conn.Send(MessageType::Config, 0, NowMicros(), payload);
}

// Per-frame timestamps (host clock, µs) used to break latency down by stage.
struct FrameTimes
{
    uint64_t present = 0;  // DWM presented the frame (DDA LastPresentTime)
    uint64_t acquired = 0; // AcquireNextFrame returned
    uint64_t encodeStart = 0;
    uint64_t encoded = 0; // encoder output available
};

struct SessionStats
{
    uint64_t windowStartUs = 0;
    unsigned frames = 0;
    uint64_t bytes = 0;
    uint64_t pipelineUs = 0;
    uint64_t waitAcquireUs = 0;
    uint64_t composeUs = 0;
    uint64_t encodeUs = 0;
    uint64_t sendUs = 0;
    uint64_t encoderWaitUs = 0;
    uint64_t encoderProcessUs = 0;

    void Add(size_t size, const FrameTimes& t, uint64_t sent, const encode::EncoderTiming& et)
    {
        encoderWaitUs += et.waitInputUs;
        encoderProcessUs += et.processingUs;
        ++frames;
        bytes += size;
        pipelineUs += sent - t.present;
        waitAcquireUs += t.acquired - t.present;
        composeUs += t.encodeStart - t.acquired;
        encodeUs += t.encoded - t.encodeStart;
        sendUs += sent - t.encoded;
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
            const double n = frames * 1000.0;
            Log(L"stream: %.1f fps, %.1f Mbit/s | present->sent %.2f ms = acquire %.2f + compose %.2f + "
                L"encode %.2f (wait-input %.2f, process %.2f) + send %.2f",
                frames * 1e6 / elapsed, bytes * 8.0 / elapsed, pipelineUs / n, waitAcquireUs / n, composeUs / n,
                encodeUs / n, encoderWaitUs / n, encoderProcessUs / n, sendUs / n);
        }
        *this = {};
        windowStartUs = now;
    }
};

// End-to-end timings reported by the client (FRAME_STATS, host clock), logged every 2 s.
struct ClientStats
{
    uint64_t windowStartUs = 0;
    std::vector<double> received, decoded, rendered; // ms after present

    static double Percentile(std::vector<double>& v, double p)
    {
        if (v.empty())
        {
            return 0;
        }
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, static_cast<size_t>(p * v.size()))];
    }

    void Add(const protocol::FrameStats& s)
    {
        if (s.captureTs == 0 || s.receivedTs < s.captureTs)
        {
            return;
        }
        received.push_back((s.receivedTs - s.captureTs) / 1000.0);
        if (s.decodedTs >= s.captureTs)
        {
            decoded.push_back((s.decodedTs - s.captureTs) / 1000.0);
        }
        if (s.renderedTs >= s.captureTs)
        {
            rendered.push_back((s.renderedTs - s.captureTs) / 1000.0);
        }

        const uint64_t now = NowMicros();
        if (windowStartUs == 0)
        {
            windowStartUs = now;
        }
        if (now - windowStartUs >= 2'000'000)
        {
            Log(L"client: present->received p50 %.1f p95 %.1f | ->decoded p50 %.1f p95 %.1f | "
                L"->on screen p50 %.1f p95 %.1f ms (%zu frames)",
                Percentile(received, 0.5), Percentile(received, 0.95), Percentile(decoded, 0.5),
                Percentile(decoded, 0.95), Percentile(rendered, 0.5), Percentile(rendered, 0.95), received.size());
            received.clear();
            decoded.clear();
            rendered.clear();
            windowStartUs = now;
        }
    }
};

// Wi-Fi handshake: the client proves it knows the pairing code, then the host does.
bool Authenticate(transport::Connection& conn, const protocol::AuthKey& key)
{
    protocol::Nonce hostNonce{};
    if (!protocol::RandomNonce(hostNonce) ||
        !conn.Send(MessageType::AuthChallenge, 0, NowMicros(), protocol::SerializeNonce(hostNonce)))
    {
        return false;
    }

    protocol::Header header;
    std::vector<uint8_t> payload;
    conn.SetReceiveTimeout(5000);
    const bool received = conn.Receive(header, payload);
    conn.SetReceiveTimeout(0);
    const auto response = received && header.type == MessageType::AuthResponse
                              ? protocol::ParseAuthResponse(payload)
                              : std::nullopt;
    if (!response ||
        !protocol::MacEquals(response->mac, protocol::ClientAuthMac(key, hostNonce, response->clientNonce)))
    {
        Log(L"session: Wi-Fi client failed authentication (wrong pairing code, or no response in 5 s)");
        conn.Send(MessageType::Bye, 0, NowMicros(), {});
        return false;
    }
    const auto proof = protocol::HostAuthMac(key, response->clientNonce, hostNonce);
    return conn.Send(MessageType::AuthOk, 0, NowMicros(), protocol::SerializeMac(proof));
}

void RunSession(transport::Connection& conn, Transport transport, const PairingContext* pairing,
                const ServeOptions& serveOptions, VirtualDisplay& display)
{
    // Held for the whole Wi-Fi session: keeps the PC's radio awake and on-channel.
    std::optional<wireless::WlanTuning> wlanTuning;
    if (transport == Transport::Wifi)
    {
        if (!pairing || !Authenticate(conn, pairing->secrets.authKey))
        {
            return;
        }
        wlanTuning.emplace();
        conn.EnableWifiQos();
    }

    protocol::Header header;
    std::vector<uint8_t> payload;
    if (!conn.Receive(header, payload) || header.type != MessageType::Hello)
    {
        Log(L"session: expected HELLO");
        return;
    }
    const auto hello = protocol::ParseHello(payload);
    if (!hello || !(hello->codecs & (protocol::kCodecMaskH264 | protocol::kCodecMaskHevc)))
    {
        Log(L"session: unsupported client (bad HELLO or no H.264)");
        conn.Send(MessageType::Bye, 0, NowMicros(), {});
        return;
    }
    Log(L"session: client \"%S\" over %s, %ux%u @ %.1f Hz, %u dpi", hello->deviceName.c_str(),
        TransportName(transport), hello->widthPx, hello->heightPx, hello->refreshMilliHz / 1000.0,
        hello->densityDpi);

    // A mode picked on the client wins over the command-line limits: stream exactly that.
    ServeOptions options = serveOptions;
    const RequestedMode mode = RequestedModeFor(*hello);
    if (mode.width)
    {
        options.maxStreamWidth = 0;
        options.maxStreamHeight = 0;
    }
    if (mode.hz)
    {
        options.maxFps = mode.hz;
    }
    if (mode.width || mode.hz)
    {
        Log(L"session: client requested %ux%u @ %u Hz (0 = host decides)", mode.width, mode.height, mode.hz);
    }

    // Declared before the pipeline so the monitor is unplugged after capture/encode shut down.
    struct UnplugOnExit
    {
        VirtualDisplay& display;
        ~UnplugOnExit() { display.Unplug(); }
    } unplugOnExit{display};

    if (options.outputName.empty())
    {
        auto request = PlugRequestFor(*hello, options.maxFps, mode);
        HRESULT plugged = display.Plug(request);
        if (FAILED(plugged) && (mode.width || mode.hz))
        {
            // E.g. the pixel clock doesn't fit an EDID timing. Streaming something beats a client
            // stuck reconnecting; CONFIG tells it what it actually gets.
            Log(L"session: plugging the requested mode failed 0x%08lX, falling back to the default", Hr(plugged));
            options = serveOptions;
            request = PlugRequestFor(*hello, options.maxFps, {});
            plugged = display.Plug(request);
        }
        if (FAILED(plugged))
        {
            Log(L"session: plugging the virtual monitor failed 0x%08lX. Is the driver installed? "
                L"See driver/README.md.",
                Hr(plugged));
            conn.Send(MessageType::Bye, 0, NowMicros(), {});
            return;
        }
        Log(L"session: plugged %ux%u @ %u Hz, %ux%u mm", request.width, request.height, request.refreshHz[0],
            request.widthMm, request.heightMm);
        if (!WaitForExtendedDisplay(8000))
        {
            Log(L"session: the monitor did not appear as an extended display (looking for %s). "
                L"Check Settings > System > Display.",
                capture::kMonitorHardwareId);
            conn.Send(MessageType::Bye, 0, NowMicros(), {});
            return;
        }
    }

    Pipeline pipeline;
    const bool clientHevc = (hello->codecs & protocol::kCodecMaskHevc) != 0;
    const bool clientH264 = (hello->codecs & protocol::kCodecMaskH264) != 0;
    const bool preferHevc = clientHevc && (options.codec != L"h264" || !clientH264);
    HRESULT hr = pipeline.Initialize(options, preferHevc);
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
    if (transport == Transport::Usb && pairing)
    {
        // USB is the trusted channel: hand over the code the client needs for Wi-Fi.
        const auto message =
            protocol::SerializePairing({protocol::FormatPairingCode(pairing->code), pairing->hostName});
        if (!conn.Send(MessageType::Pairing, 0, NowMicros(), message))
        {
            return;
        }
    }

    // Declared before the reader thread, which injects into it; lifts all contacts on exit.
    input::TouchInjector touch;
    touch.SetTarget(pipeline.desktopRect);

    std::atomic<bool> stop{false};
    std::atomic<bool> keyframeRequested{false};
    std::thread reader([&] {
        protocol::Header h;
        std::vector<uint8_t> p;
        ClientStats clientStats;
        while (!stop && conn.Receive(h, p))
        {
            switch (h.type)
            {
            case MessageType::FrameStats:
                if (auto s = protocol::ParseFrameStats(p))
                {
                    clientStats.Add(*s);
                }
                break;
            case MessageType::RequestKeyframe:
                keyframeRequested = true;
                break;
            case MessageType::Input:
                if (options.touchInput)
                {
                    if (auto frame = protocol::ParseTouchFrame(p))
                    {
                        touch.Inject(*frame);
                    }
                }
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
                break;
            }
        }
        stop = true;
    });

    SessionStats stats;
    std::vector<uint8_t> lastCodecConfig;
    std::vector<encode::EncodedFrame> encoded;
    bool haveFrame = false;

    FrameTimes times;
    auto sendEncoded = [&](const encode::EncodedFrame& frame, uint8_t extraFlags = 0) {
        auto split = annexb::SeparateParameterSets(frame.data,
                                                   pipeline.hevc ? annexb::Codec::Hevc : annexb::Codec::H264);
        if (!split.codecConfig.empty() && split.codecConfig != lastCodecConfig)
        {
            if (!conn.Send(MessageType::VideoFrame, protocol::video_flags::kCodecConfig, frame.timestampUs,
                           split.codecConfig))
            {
                return false;
            }
            lastCodecConfig = std::move(split.codecConfig);
        }
        const uint8_t flags =
            static_cast<uint8_t>(((split.keyframe || frame.keyframe) ? protocol::video_flags::kKeyframe : 0) | extraFlags);
        if (!conn.Send(MessageType::VideoFrame, flags, frame.timestampUs, split.frame))
        {
            return false;
        }
        if (frame.timestampUs == times.present)
        {
            stats.Add(split.frame.size(), times, NowMicros(), pipeline.encoder.LastTiming());
        }
        return true;
    };

    PreciseSleeper sleeper;
    const uint64_t frameIntervalUs = 1'000'000 / std::max(1u, options.maxFps);
    uint64_t nextFrameUs = 0;

    while (!stop)
    {
        // Pace to maxFps. While we wait, Desktop Duplication keeps accumulating updates, so the
        // next acquired frame is the newest desktop state rather than a queued stale one.
        const uint64_t nowUs = NowMicros();
        if (nowUs < nextFrameUs)
        {
            sleeper.SleepMicros(nextFrameUs - nowUs);
        }

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
            while (!stop && FAILED(pipeline.Initialize(options, preferHevc)))
            {
                Sleep(250);
            }
            haveFrame = false;
            lastCodecConfig.clear();
            touch.SetTarget(pipeline.desktopRect);
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

        times.acquired = NowMicros();
        times.present = frame.presentQpc ? QpcToMicros(frame.presentQpc) : times.acquired;
        nextFrameUs = times.acquired + frameIntervalUs;
        ComPtr<ID3D11Texture2D> nv12;
        hr = pipeline.composer.Compose(frame.desktop, pipeline.duplicator.Pointer(), &nv12);
        pipeline.duplicator.ReleaseFrame();
        if (FAILED(hr))
        {
            Log(L"compose failed 0x%08lX", Hr(hr));
            continue;
        }
        haveFrame = true;

        times.encodeStart = NowMicros();
        hr = pipeline.encoder.Encode(nv12.Get(), times.present, encoded);
        times.encoded = NowMicros();
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

        if (options.repeatFrames && !stop && !encoded.empty())
        {
            // Same image again: encodes to a tiny all-skip frame that pushes the real one out of
            // "one frame behind" decoders right away (see REPEAT in docs/wire-protocol.md).
            if (SUCCEEDED(pipeline.encoder.Encode(nv12.Get(), times.present + 1, encoded)))
            {
                for (const auto& f : encoded)
                {
                    if (!sendEncoded(f, protocol::video_flags::kRepeat))
                    {
                        stop = true;
                        break;
                    }
                }
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
    if (!options.outputName.empty())
    {
        capture::LocatedOutput output;
        if (!capture::FindOutputByName(options.outputName, output))
        {
            Log(L"output %s not found (see --list-outputs)", options.outputName.c_str());
            return 1;
        }
        Log(L"debug: streaming existing output %s", output.deviceName.c_str());
    }

    std::optional<PairingContext> pairing;
    if (auto code = wireless::LoadOrCreatePairingCode())
    {
        if (auto secrets = protocol::DerivePairingSecrets(*code))
        {
            pairing = PairingContext{*code, *secrets, wireless::ComputerNameUtf8()};
        }
    }
    if (!pairing)
    {
        Log(L"pairing: no pairing code available, Wi-Fi disabled");
        if (!options.usb)
        {
            return 1;
        }
    }

    transport::Listener usbListener;
    if (options.usb && !usbListener.Listen(options.port))
    {
        Log(L"listen on 127.0.0.1:%u failed (%d)", options.port, WSAGetLastError());
        return 1;
    }

    wireless::WifiDirectAp accessPoint;
    transport::Listener wifiListener;
    std::wstring wifiEndpoint;
    auto startWifi = [&] {
        wifiListener.Close();
        const std::wstring ssid(pairing->secrets.ssid.begin(), pairing->secrets.ssid.end());
        const std::wstring passphrase(pairing->secrets.passphrase.begin(), pairing->secrets.passphrase.end());
        HRESULT hr = accessPoint.Start(ssid, passphrase);
        if (FAILED(hr))
        {
            Log(L"wifi: can't start the access point (0x%08lX). Is Wi-Fi on and Mobile Hotspot off?", Hr(hr));
            return false;
        }
        in_addr address{};
        if (!wireless::WifiDirectAp::WaitForAddress(10000, address))
        {
            Log(L"wifi: the access point got no IPv4 address");
            accessPoint.Stop();
            return false;
        }
        if (!wifiListener.Listen(options.port, address))
        {
            Log(L"wifi: listen failed (%d)", WSAGetLastError());
            accessPoint.Stop();
            return false;
        }
        wchar_t text[INET_ADDRSTRLEN] = {};
        InetNtopW(AF_INET, &address, text, INET_ADDRSTRLEN);
        wifiEndpoint = std::wstring(text) + L":" + std::to_wstring(options.port);
        Log(L"wifi: network \"%s\" is up. Pair a tablet once over USB, or type the code from --pair.",
            ssid.c_str());
        return true;
    };
    if (options.wifi && pairing && !startWifi() && !options.usb)
    {
        return 1;
    }

    bool announce = true;
    for (;;)
    {
        if (announce)
        {
            std::wstring where;
            if (usbListener.Handle() != INVALID_SOCKET)
            {
                where = L"USB (127.0.0.1:" + std::to_wstring(options.port) + L")";
            }
            if (wifiListener.Handle() != INVALID_SOCKET)
            {
                where += (where.empty() ? L"Wi-Fi (" : L" or Wi-Fi (") + wifiEndpoint + L")";
            }
            Log(L"waiting for a client on %s", where.c_str());
            announce = false;
        }

        fd_set readable;
        FD_ZERO(&readable);
        if (usbListener.Handle() != INVALID_SOCKET)
        {
            FD_SET(usbListener.Handle(), &readable);
        }
        if (wifiListener.Handle() != INVALID_SOCKET)
        {
            FD_SET(wifiListener.Handle(), &readable);
        }

        int ready = 0;
        if (readable.fd_count > 0)
        {
            timeval timeout{2, 0};
            ready = select(0, &readable, nullptr, nullptr, &timeout);
            if (ready == SOCKET_ERROR)
            {
                Log(L"select failed (%d)", WSAGetLastError());
                return 1;
            }
        }
        else
        {
            Sleep(2000); // Wi-Fi only, and the network is down: retried below
        }

        if (ready == 0)
        {
            if (options.wifi && pairing && !accessPoint.Running())
            {
                Log(L"wifi: access point down, restarting it");
                announce = startWifi();
            }
            continue;
        }

        const bool fromUsb = usbListener.Handle() != INVALID_SOCKET && FD_ISSET(usbListener.Handle(), &readable);
        auto conn = fromUsb ? usbListener.Accept() : wifiListener.Accept();
        if (!conn)
        {
            Log(L"accept failed (%d)", WSAGetLastError());
            continue;
        }
        RunSession(*conn, fromUsb ? Transport::Usb : Transport::Wifi, pairing ? &*pairing : nullptr, options,
                   display);
        announce = true;
    }
}

} // namespace dd
