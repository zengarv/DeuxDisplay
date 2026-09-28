#include "Server.h"

#include "adb/AdbWatcher.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "capture/DesktopDuplicator.h"
#include "capture/OutputLocator.h"
#include "common/Clock.h"
#include "common/Log.h"
#include "display/ModeList.h"
#include "display/Orientation.h"
#include "display/VirtualDisplay.h"
#include "encode/AdaptiveBitrate.h"
#include "encode/AnnexB.h"
#include "encode/MfH264Encoder.h"
#include "input/Shortcuts.h"
#include "input/TouchInjector.h"
#include "media/MediaControls.h"
#include "protocol/Pairing.h"
#include "render/FrameComposer.h"
#include "render/Rotation.h"
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
    // Replaced whole when the bitrate or quality changes (see EncoderSwap).
    std::unique_ptr<encode::MfH264Encoder> encoder = std::make_unique<encode::MfH264Encoder>();
    LUID adapterLuid{};
    RECT desktopRect{}; // where the output sits on the Windows desktop (for touch)
    UINT width = 0; // desktop (virtual monitor) size
    UINT height = 0;
    UINT streamWidth = 0; // encoded size
    UINT streamHeight = 0;
    bool hevc = false;
    uint16_t rotation = 0; // clockwise degrees the client applies (CONFIG)
    UINT fps = 60;

    // `monitorIndex`: which virtual monitor this session streams (VirtualDisplay::Index()).
    HRESULT Initialize(const ServeOptions& options, bool preferHevc, unsigned monitorIndex, unsigned bitrateKbps,
                       unsigned quality)
    {
        encoder->Shutdown();
        device.Reset();
        const bool found = options.outputName.empty() ? capture::FindVirtualOutput(monitorIndex, output)
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
        // Frames stay in scan-out (native landscape) orientation even when Windows rotates the
        // display; the client rotates them for display.
        rotation = render::ClientRotationFor(desc.Rotation);
        // ModeDesc reports the rotated (desktop) size, but the duplicated texture keeps the
        // scan-out shape: size everything from the texture.
        if (rotation == 90 || rotation == 270)
        {
            std::swap(width, height);
        }
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

        hr = composer.Initialize(device.Get(), width, height, streamWidth, streamHeight, rotation);
        if (FAILED(hr))
        {
            return hr;
        }

        DXGI_ADAPTER_DESC1 adapterDesc{};
        output.adapter->GetDesc1(&adapterDesc);
        encode::EncoderSettings settings{streamWidth, streamHeight, fps, bitrateKbps};
        settings.hevc = preferHevc;
        settings.qualityVsSpeed = quality;
        adapterLuid = adapterDesc.AdapterLuid;
        hr = encoder->Initialize(device.Get(), adapterDesc.AdapterLuid, settings);
        if (FAILED(hr) && preferHevc)
        {
            Log(L"pipeline: HEVC encoder unavailable (0x%08lX), using H.264", Hr(hr));
            settings.hevc = false;
            hr = encoder->Initialize(device.Get(), adapterDesc.AdapterLuid, settings);
        }
        if (FAILED(hr))
        {
            return hr;
        }
        hevc = settings.hevc;
        Log(L"pipeline: %s %ux%u@%u -> stream %ux%u %s, rotation %u, encoder \"%s\"", output.deviceName.c_str(),
            width, height, fps, streamWidth, streamHeight, hevc ? L"HEVC" : L"H.264", rotation,
            encoder->Name().c_str());
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
        const uint16_t width = std::max(hello.modeWidthPx, hello.modeHeightPx); // landscape
        const uint16_t height = std::min(hello.modeWidthPx, hello.modeHeightPx);
        if (driver::IsValidMode(width, height, 60))
        {
            mode.width = width;
            mode.height = height;
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
    const uint16_t nativeWidth = static_cast<uint16_t>(width & ~1u);
    const uint16_t nativeHeight = static_cast<uint16_t>(height & ~1u);
    r.width = mode.width ? mode.width : nativeWidth;
    r.height = mode.height ? mode.height : nativeHeight;

    long hz = std::lround(hello.refreshMilliHz / 1000.0);
    if (hz < 24 || hz > 240)
    {
        hz = 60;
    }
    // The picked rate, else the panel's capped at what the client decodes comfortably (maxFps).
    const long preferred = mode.hz ? static_cast<long>(mode.hz) : std::min<long>(hz, static_cast<long>(maxFps));
    r.refreshHz[0] = static_cast<uint16_t>(preferred);
    if (hz != preferred)
    {
        r.refreshHz[1] = static_cast<uint16_t>(hz); // the EDID's second timing
    }

    // Every size x rate the client can show, so the mode can change in Windows or from the app
    // without plugging a new monitor. Without lists: the native size at the preferred rates.
    std::vector<driver::PlugMode> sizes;
    for (const auto& size : hello.sizes)
    {
        sizes.push_back({std::max(size.width, size.height), std::min(size.width, size.height), 0});
    }
    if (sizes.empty())
    {
        sizes.push_back({nativeWidth, nativeHeight, 0});
    }
    std::vector<uint16_t> rates(hello.rates.begin(), hello.rates.end());
    if (rates.empty())
    {
        rates = {r.refreshHz[0], r.refreshHz[1]};
    }
    const auto modes = BuildModeList({r.width, r.height, r.refreshHz[0]}, sizes, rates);
    r.modeCount = static_cast<uint16_t>(modes.size());
    std::copy(modes.begin(), modes.end(), r.modes);
    return r;
}

// Whether `request` offers this mode (after filling 0 fields from its preferred mode).
std::optional<driver::PlugMode> OfferedMode(const driver::PlugRequest& request, protocol::DisplayMode wanted)
{
    const driver::PlugMode mode{wanted.width ? wanted.width : request.width,
                                wanted.height ? wanted.height : request.height,
                                wanted.refreshHz ? wanted.refreshHz : request.refreshHz[0]};
    for (uint16_t i = 0; i < request.modeCount; ++i)
    {
        const auto& m = request.modes[i];
        if (m.width == mode.width && m.height == mode.height && m.refreshHz == mode.refreshHz)
        {
            return mode;
        }
    }
    return std::nullopt;
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
                            static_cast<uint16_t>(p.streamHeight), p.fps * 1000, bitrateKbps, p.rotation};
    const auto payload = protocol::SerializeConfig(config);
    return conn.Send(MessageType::Config, 0, NowMicros(), payload);
}

// Adaptive bitrate range. Over USB the adb tunnel carries far more than this, but the tablet's
// decoder slows down long before; the controller finds where. The Wi-Fi link measured ~100 Mbit/s
// at best (docs/latency-notes.md), so stay well under that.
constexpr unsigned kAdaptiveStartKbps = 30'000;
constexpr encode::AdaptiveBitrate::Limits kAdaptiveUsb{5'000, 150'000};
constexpr encode::AdaptiveBitrate::Limits kAdaptiveWifi{5'000, 60'000};
constexpr unsigned kMinBitrateKbps = 500;
constexpr unsigned kMaxBitrateKbps = 500'000;

// The encoder's bitrate and quality for a session. The reader thread decides (user picks, the
// adaptive controller); the frame loop applies them, since the encoder isn't thread-safe.
struct EncoderControl
{
    std::mutex mutex;
    bool adaptive = false;
    unsigned kbps = 0;
    unsigned quality = 0;
    std::atomic<bool> changed{false};

    protocol::EncoderState State()
    {
        std::lock_guard lock(mutex);
        return {kbps, adaptive, static_cast<uint8_t>(quality)};
    }
};

// Encoders take bitrate and quality only at initialization (Intel's MFT accepts changes through
// ICodecAPI but ignores them), and creating one takes ~200-350 ms. So a change builds a new
// encoder on a worker thread while the current one keeps streaming, and the frame loop swaps it
// in between two frames: the only cost on the stream is the new encoder's first keyframe. The
// old encoder is shut down on a worker thread too.
class EncoderSwap
{
  public:
    ~EncoderSwap()
    {
        Join(m_builder);
        Join(m_retirer);
    }

    bool Busy() const { return m_builder.joinable(); }

    void Build(ComPtr<ID3D11Device> device, LUID luid, encode::EncoderSettings settings)
    {
        m_ready = false;
        m_builder = std::thread([this, device, luid, settings] {
            m_encoder = std::make_unique<encode::MfH264Encoder>();
            m_hr = m_encoder->Initialize(device.Get(), luid, settings);
            m_device = device;
            m_ready = true;
        });
    }

    // The new encoder once built, if it's for `device`; nullptr while building or on failure.
    std::unique_ptr<encode::MfH264Encoder> TakeReady(ID3D11Device* device, HRESULT& hr)
    {
        if (!m_ready)
        {
            return nullptr;
        }
        Join(m_builder);
        m_ready = false;
        hr = m_device.Get() == device ? m_hr : HRESULT_FROM_WIN32(ERROR_DEVICE_REINITIALIZATION_NEEDED);
        m_device.Reset();
        auto encoder = std::move(m_encoder);
        return SUCCEEDED(hr) ? std::move(encoder) : nullptr;
    }

    void Retire(std::unique_ptr<encode::MfH264Encoder> encoder)
    {
        Join(m_retirer);
        m_retirer = std::thread([old = std::move(encoder)]() mutable { old.reset(); });
    }

  private:
    static void Join(std::thread& t)
    {
        if (t.joinable())
        {
            t.join();
        }
    }

    std::thread m_builder;
    std::thread m_retirer;
    std::unique_ptr<encode::MfH264Encoder> m_encoder;
    ComPtr<ID3D11Device> m_device;
    HRESULT m_hr = S_OK;
    std::atomic<bool> m_ready{false};
};

// Frames recently sent, so FRAME_STATS (keyed by capture time) can be matched with their send
// time and size for the adaptive bitrate controller.
class SentFrames
{
  public:
    struct Frame
    {
        uint64_t captureUs = 0;
        uint64_t sentUs = 0;
        uint32_t bytes = 0;
        uint32_t budgetBytes = 0; // what CBR allowed for it
    };

    void Add(const Frame& frame)
    {
        std::lock_guard lock(m_mutex);
        m_frames[m_next] = frame;
        m_next = (m_next + 1) % m_frames.size();
    }

    void Clear()
    {
        std::lock_guard lock(m_mutex);
        m_frames.fill({});
    }

    std::optional<Frame> Find(uint64_t captureUs)
    {
        std::lock_guard lock(m_mutex);
        for (const auto& f : m_frames)
        {
            if (f.captureUs == captureUs && f.sentUs != 0)
            {
                return f;
            }
        }
        return std::nullopt;
    }

  private:
    std::mutex m_mutex;
    std::array<Frame, 256> m_frames{};
    size_t m_next = 0;
};

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

// One client, start to finish. Runs on its own thread; concurrent sessions each plug their own
// virtual monitor and share nothing on the frame path.
void RunSession(transport::Connection& conn, Transport transport, const PairingContext* pairing,
                const ServeOptions& serveOptions)
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
    VirtualDisplay display;
    driver::PlugRequest request; // the monitor's modes, for DISPLAY_MODE

    if (options.outputName.empty())
    {
        request = PlugRequestFor(*hello, options.maxFps, mode);
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
            Log(L"session: plugging the virtual monitor failed 0x%08lX. Is the driver installed and up to "
                L"date (older drivers show one display at a time)? See driver/README.md.",
                Hr(plugged));
            conn.Send(MessageType::Bye, 0, NowMicros(), {});
            return;
        }
        Log(L"session: plugged monitor %u: %ux%u @ %u Hz (%u modes), %ux%u mm", display.Index() + 1, request.width,
            request.height, request.refreshHz[0], request.modeCount, request.widthMm, request.heightMm);
        if (!WaitForExtendedDisplay(display.Index(), 8000))
        {
            Log(L"session: the monitor did not appear as an extended display (looking for %s). "
                L"Check Settings > System > Display.",
                capture::MonitorHardwareId(display.Index()).c_str());
            conn.Send(MessageType::Bye, 0, NowMicros(), {});
            return;
        }
        // Windows restores the mode it last used for this monitor; a mode picked in the app wins.
        // With no pick, whatever Windows (or the user in Settings) chose stands.
        capture::LocatedOutput located;
        if ((mode.width || mode.hz) && capture::FindVirtualOutput(display.Index(), located))
        {
            SetDisplayMode(located.deviceName, request.width, request.height, request.refreshHz[0]);
        }
        // The monitor only offers modes the client can show, so stream at whichever one Windows
        // runs, including one the user switches to later.
        options.maxFps = 240;
    }

    // Bitrate: the user's pick (HELLO, later ENCODER_SETTINGS), else --bitrate, else adaptive.
    // Quality: the user's pick, else the fastest preset.
    const auto abrLimits = transport == Transport::Wifi ? kAdaptiveWifi : kAdaptiveUsb;
    const auto fixedKbpsFor = [&](uint32_t requested) -> unsigned {
        return requested ? std::clamp<unsigned>(requested, kMinBitrateKbps, kMaxBitrateKbps) : options.bitrateKbps;
    };
    const auto qualityFor = [](uint8_t requested) -> unsigned {
        return requested == protocol::kQualityHostDecides ? 0u : std::min<unsigned>(requested, 100);
    };
    EncoderControl control;
    control.adaptive = fixedKbpsFor(hello->bitrateKbps) == 0;
    control.kbps = control.adaptive ? kAdaptiveStartKbps : fixedKbpsFor(hello->bitrateKbps);
    control.quality = qualityFor(hello->encoderQuality);
    encode::AdaptiveBitrate abr(control.kbps, abrLimits);
    SentFrames sentFrames;
    EncoderSwap encoderSwap;
    unsigned appliedKbps = control.kbps;
    unsigned appliedQuality = control.quality;
    Log(L"session: bitrate %s%.1f Mbit/s, quality %u", control.adaptive ? L"adaptive from " : L"",
        appliedKbps / 1000.0, appliedQuality);
    // What the encoder runs at now (the wanted values may still be on their way in).
    const auto sendEncoderState = [&] {
        bool adaptive = false;
        {
            std::lock_guard lock(control.mutex);
            adaptive = control.adaptive;
        }
        const protocol::EncoderState state{appliedKbps, adaptive, static_cast<uint8_t>(appliedQuality)};
        return conn.Send(MessageType::EncoderState, 0, NowMicros(), protocol::SerializeEncoderState(state));
    };

    Pipeline pipeline;
    const bool clientHevc = (hello->codecs & protocol::kCodecMaskHevc) != 0;
    const bool clientH264 = (hello->codecs & protocol::kCodecMaskH264) != 0;
    const bool preferHevc = clientHevc && (options.codec != L"h264" || !clientH264);
    HRESULT hr = pipeline.Initialize(options, preferHevc, display.Index(), appliedKbps, appliedQuality);
    if (FAILED(hr))
    {
        Log(L"session: pipeline init failed 0x%08lX", Hr(hr));
        conn.Send(MessageType::Bye, 0, NowMicros(), {});
        return;
    }
    if (!SendConfig(conn, pipeline, appliedKbps) || !sendEncoderState())
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
    // The dock's shortcuts, volume and play/pause state. Its first MEDIA_STATE also tells the
    // client that this host takes ACTION messages. Off along with touch (--no-touch).
    std::optional<media::MediaControls> media;
    if (options.touchInput)
    {
        media.emplace([&conn](const protocol::MediaState& state) {
            conn.Send(MessageType::MediaState, 0, NowMicros(), protocol::SerializeMediaState(state));
        });
    }

    std::atomic<bool> stop{false};
    std::atomic<bool> keyframeRequested{false};
    std::atomic<int> requestedOrientation{-1}; // degrees from the client, -1 = none pending
    // DISPLAY_MODE from the client, packed width << 32 | height << 16 | Hz; 0 = none pending.
    std::atomic<uint64_t> requestedMode{0};
    // Adaptive bitrate: one delivered frame (reader thread). Delivery = host send -> decoded on
    // the client, the part of the latency the bitrate drives.
    std::atomic<bool> streamRebuilt{false}; // set by the frame loop, consumed by the reader
    const auto onAdaptiveSample = [&](const protocol::FrameStats& s) {
        if (streamRebuilt.exchange(false))
        {
            abr.Reset(abr.TargetKbps()); // a new format: learn its delivery times afresh
        }
        if (s.decodedTs == 0)
        {
            return;
        }
        {
            std::lock_guard lock(control.mutex);
            if (!control.adaptive)
            {
                return;
            }
        }
        const auto sent = sentFrames.Find(s.captureTs);
        if (!sent || s.decodedTs < sent->sentUs)
        {
            return;
        }
        const unsigned before = abr.TargetKbps();
        if (!abr.OnFrame(NowMicros(), s.decodedTs - sent->sentUs, sent->bytes, sent->budgetBytes))
        {
            return;
        }
        const auto& w = abr.LastWindow();
        Log(L"abr: %.1f -> %.1f Mbit/s (delivery p50 %.1f ms, baseline %.1f, budget used %.0f%%, %u frames)",
            before / 1000.0, abr.TargetKbps() / 1000.0, w.deliveryMs, w.baselineMs, w.utilization * 100, w.frames);
        std::lock_guard lock(control.mutex);
        if (control.adaptive)
        {
            control.kbps = abr.TargetKbps();
            control.changed = true;
        }
    };

    std::wstring logTag = t_logTag;
    std::thread reader([&] {
        SetLogTag(logTag.c_str());
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
                    onAdaptiveSample(*s);
                }
                break;
            case MessageType::DisplayMode:
                if (auto wanted = protocol::ParseDisplayMode(p); wanted && options.outputName.empty())
                {
                    if (const auto m = OfferedMode(request, *wanted))
                    {
                        requestedMode = uint64_t{m->width} << 32 | uint64_t{m->height} << 16 | m->refreshHz;
                    }
                    else
                    {
                        Log(L"session: client asked for %ux%u @ %u Hz, which the monitor doesn't offer", wanted->width,
                            wanted->height, wanted->refreshHz);
                    }
                }
                break;
            case MessageType::EncoderSettings:
                if (auto e = protocol::ParseEncoderSettings(p))
                {
                    const unsigned fixedKbps = fixedKbpsFor(e->bitrateKbps);
                    std::lock_guard lock(control.mutex);
                    if (fixedKbps == 0 && !control.adaptive)
                    {
                        abr.Reset(control.kbps); // carry on from where the user left it
                    }
                    control.adaptive = fixedKbps == 0;
                    control.kbps = control.adaptive ? abr.TargetKbps() : fixedKbps;
                    control.quality = qualityFor(e->quality);
                    control.changed = true;
                    Log(L"session: client set bitrate %s%.1f Mbit/s, quality %u",
                        control.adaptive ? L"adaptive from " : L"", control.kbps / 1000.0, control.quality);
                }
                break;
            case MessageType::RequestKeyframe:
                keyframeRequested = true;
                break;
            case MessageType::Orientation:
                if (auto degrees = protocol::ParseOrientation(p))
                {
                    requestedOrientation = *degrees;
                }
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
            case MessageType::Action:
                if (auto action = protocol::ParseAction(p); action && media)
                {
                    if (*action == protocol::Action::VolumeUp || *action == protocol::Action::VolumeDown)
                    {
                        media->StepVolume(*action == protocol::Action::VolumeUp ? 1 : -1);
                    }
                    else
                    {
                        input::SendShortcut(*action);
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
            const uint64_t sentUs = NowMicros();
            stats.Add(split.frame.size(), times, sentUs, pipeline.encoder->LastTiming());
            sentFrames.Add({frame.timestampUs, sentUs, static_cast<uint32_t>(split.frame.size()),
                            appliedKbps * 1000 / 8 / std::max(1u, pipeline.fps)});
        }
        return true;
    };

    PreciseSleeper sleeper;
    uint64_t nextFrameUs = 0;

    while (!stop)
    {
        // Pace to the stream rate. While we wait, Desktop Duplication keeps accumulating updates, so the
        // next acquired frame is the newest desktop state rather than a queued stale one.
        const uint64_t nowUs = NowMicros();
        if (nowUs < nextFrameUs)
        {
            sleeper.SleepMicros(nextFrameUs - nowUs);
        }

        // A new bitrate/quality from the user or the adaptive controller: build an encoder for it
        // off the frame path, then swap it in here (see EncoderSwap).
        if (!encoderSwap.Busy() && control.changed.exchange(false))
        {
            const auto want = control.State();
            if (want.bitrateKbps != appliedKbps || want.quality != appliedQuality)
            {
                auto settings = pipeline.encoder->Settings();
                settings.bitrateKbps = want.bitrateKbps;
                settings.qualityVsSpeed = want.quality;
                encoderSwap.Build(pipeline.device, pipeline.adapterLuid, settings);
            }
            else if (!sendEncoderState())
            {
                break;
            }
        }
        HRESULT swapped = S_OK;
        if (auto encoder = encoderSwap.TakeReady(pipeline.device.Get(), swapped))
        {
            const auto& settings = encoder->Settings();
            Log(L"encoder: now %.1f Mbit/s, quality %u", settings.bitrateKbps / 1000.0, settings.qualityVsSpeed);
            appliedKbps = settings.bitrateKbps;
            appliedQuality = settings.qualityVsSpeed;
            encoderSwap.Retire(std::exchange(pipeline.encoder, std::move(encoder)));
            lastCodecConfig.clear();
            keyframeRequested = true; // on a static desktop, show the new settings right away
            control.changed = true;   // pick up anything that changed while building
            if (!sendEncoderState())
            {
                break;
            }
        }
        else if (FAILED(swapped))
        {
            Log(L"encoder: rebuilding for new settings failed 0x%08lX, keeping the current one", Hr(swapped));
            control.changed = true; // e.g. the pipeline was rebuilt meanwhile: retry on the new device
        }

        // The user picked another mode in the app. Like a change in Windows Settings, capture then
        // reports ACCESS_LOST and the rebuilt pipeline sends CONFIG with the new size and rate.
        if (const uint64_t m = requestedMode.exchange(0))
        {
            SetDisplayMode(pipeline.output.deviceName, static_cast<uint16_t>(m >> 32), static_cast<uint16_t>(m >> 16),
                           static_cast<uint16_t>(m));
        }

        // The tablet turned: rotate the Windows display to match (like a pivoting monitor).
        // Capture then reports ACCESS_LOST, and the rebuilt pipeline sends CONFIG with the rotation.
        if (const int degrees = requestedOrientation.exchange(-1);
            degrees >= 0 && options.outputName.empty())
        {
            SetDisplayOrientation(pipeline.output.deviceName, static_cast<uint16_t>(degrees));
        }

        if (keyframeRequested.exchange(false))
        {
            pipeline.encoder->RequestKeyframe();
            if (haveFrame)
            {
                // Static desktop: nothing new will arrive, so re-encode the last image now.
                ComPtr<ID3D11Texture2D> nv12;
                if (SUCCEEDED(pipeline.composer.ConvertLast(&nv12)) &&
                    SUCCEEDED(pipeline.encoder->Encode(nv12.Get(), NowMicros(), encoded)))
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
            const uint16_t oldRotation = pipeline.rotation;
            while (!stop && FAILED(pipeline.Initialize(options, preferHevc, display.Index(), appliedKbps,
                                                       appliedQuality)))
            {
                Sleep(250);
            }
            haveFrame = false;
            lastCodecConfig.clear();
            sentFrames.Clear();
            streamRebuilt = true;
            touch.SetTarget(pipeline.desktopRect);
            if (pipeline.width != oldWidth || pipeline.height != oldHeight || pipeline.fps != oldFps ||
                pipeline.rotation != oldRotation)
            {
                if (!SendConfig(conn, pipeline, appliedKbps))
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

        if (!haveFrame && frame.desktop)
        {
            D3D11_TEXTURE2D_DESC td{};
            frame.desktop->GetDesc(&td);
            Log(L"capture: first frame %ux%u (mode %ux%u, dxgi rotation %d)", td.Width, td.Height, pipeline.width,
                pipeline.height, static_cast<int>(pipeline.duplicator.Desc().Rotation));
        }
        times.acquired = NowMicros();
        times.present = frame.presentQpc ? QpcToMicros(frame.presentQpc) : times.acquired;
        nextFrameUs = times.acquired + 1'000'000 / std::max(1u, pipeline.fps);
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
        hr = pipeline.encoder->Encode(nv12.Get(), times.present, encoded);
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
            if (SUCCEEDED(pipeline.encoder->Encode(nv12.Get(), times.present + 1, encoded)))
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
}

// A session on its own thread. The main thread keeps accepting, so more clients can join.
struct SessionThread
{
    std::unique_ptr<transport::Connection> conn;
    unsigned number = 0; // 1-based, reused once free; tags the session's log lines
    std::atomic<bool> done{false};
    std::thread thread;
};

class Sessions
{
  public:
    Sessions() = default;
    Sessions(const Sessions&) = delete;
    Sessions& operator=(const Sessions&) = delete;

    ~Sessions()
    {
        for (auto& s : m_sessions)
        {
            s->conn->Shutdown();
        }
        for (auto& s : m_sessions)
        {
            s->thread.join();
        }
    }

    // Joins the threads of sessions that have ended. Returns how many are still running.
    size_t Reap()
    {
        std::erase_if(m_sessions, [](const std::unique_ptr<SessionThread>& s) {
            if (!s->done)
            {
                return false;
            }
            s->thread.join();
            return true;
        });
        return m_sessions.size();
    }

    void Start(std::unique_ptr<transport::Connection> conn, Transport transport, const PairingContext* pairing,
               const ServeOptions& options)
    {
        auto session = std::make_unique<SessionThread>();
        session->conn = std::move(conn);
        for (unsigned n = 1;; ++n)
        {
            if (std::none_of(m_sessions.begin(), m_sessions.end(), [n](const auto& s) { return s->number == n; }))
            {
                session->number = n;
                break;
            }
        }
        SessionThread* s = session.get();
        s->thread = std::thread([s, transport, pairing, &options] {
            wchar_t tag[16];
            swprintf_s(tag, L"[%u] ", s->number);
            SetLogTag(tag);
            RunSession(*s->conn, transport, pairing, options);
            Log(L"session: ended");
            s->done = true;
        });
        m_sessions.push_back(std::move(session));
    }

  private:
    std::vector<std::unique_ptr<SessionThread>> m_sessions;
};

} // namespace

int Serve(const ServeOptions& options)
{
    transport::WinsockScope winsock;
    if (!winsock.Ok())
    {
        Log(L"WSAStartup failed");
        return 1;
    }

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

    // Plug and play over USB: re-creates the adb tunnel whenever a tablet (re)appears.
    std::optional<adb::AdbWatcher> adbWatcher;
    if (options.usb && options.adbWatch)
    {
        adbWatcher.emplace(options.port, adb::AdbWatcher::FindAdb(options.adbPath));
        adbWatcher->Start();
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

    // One session per client, each with its own virtual monitor, up to the driver's limit. A
    // debug --output session streams an existing display, so only one of those at a time.
    const size_t maxSessions = options.outputName.empty() ? driver::kMaxMonitors : 1;
    Sessions sessions;
    bool loggedFull = false;

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
            sessions.Reap();
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
        if (sessions.Reap() >= maxSessions)
        {
            // Clients retry, so say it once per busy spell.
            if (!loggedFull)
            {
                Log(L"session: rejecting clients, all %zu displays are in use", maxSessions);
                loggedFull = true;
            }
            conn->Send(MessageType::Bye, 0, NowMicros(), {});
            continue;
        }
        loggedFull = false;
        sessions.Start(std::move(conn), fromUsb ? Transport::Usb : Transport::Wifi, pairing ? &*pairing : nullptr,
                       options);
    }
}

} // namespace dd
