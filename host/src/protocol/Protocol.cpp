#include "Protocol.h"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace dd::protocol
{
namespace
{

class Writer
{
  public:
    explicit Writer(size_t reserve) { m_buf.reserve(reserve); }

    template <typename T> void Put(T value)
    {
        static_assert(std::is_unsigned_v<T>);
        for (size_t i = 0; i < sizeof(T); ++i)
        {
            m_buf.push_back(static_cast<uint8_t>(value >> (8 * i)));
        }
    }

    template <size_t N> void PutBytes(const std::array<uint8_t, N>& bytes)
    {
        m_buf.insert(m_buf.end(), bytes.begin(), bytes.end());
    }

    void PutString(const std::string& s)
    {
        const auto len = static_cast<uint16_t>(s.size() > 0xFFFF ? 0xFFFF : s.size());
        Put(len);
        m_buf.insert(m_buf.end(), s.begin(), s.begin() + len);
    }

    std::vector<uint8_t> Take() { return std::move(m_buf); }

  private:
    std::vector<uint8_t> m_buf;
};

class Reader
{
  public:
    explicit Reader(std::span<const uint8_t> data) : m_data(data) {}

    template <typename T> bool Get(T& out)
    {
        static_assert(std::is_unsigned_v<T>);
        if (m_data.size() - m_pos < sizeof(T))
        {
            return false;
        }
        T value = 0;
        for (size_t i = 0; i < sizeof(T); ++i)
        {
            value |= static_cast<T>(static_cast<T>(m_data[m_pos + i]) << (8 * i));
        }
        m_pos += sizeof(T);
        out = value;
        return true;
    }

    template <size_t N> bool GetBytes(std::array<uint8_t, N>& out)
    {
        if (m_data.size() - m_pos < N)
        {
            return false;
        }
        std::copy_n(m_data.begin() + m_pos, N, out.begin());
        m_pos += N;
        return true;
    }

    bool GetString(std::string& out)
    {
        uint16_t len = 0;
        if (!Get(len) || m_data.size() - m_pos < len)
        {
            return false;
        }
        out.assign(reinterpret_cast<const char*>(m_data.data() + m_pos), len);
        m_pos += len;
        return true;
    }

  private:
    std::span<const uint8_t> m_data;
    size_t m_pos = 0;
};

} // namespace

void EncodeHeader(const Header& header, std::span<uint8_t, kHeaderSize> out)
{
    out[0] = static_cast<uint8_t>(header.type);
    out[1] = header.flags;
    out[2] = 0;
    out[3] = 0;
    for (size_t i = 0; i < 4; ++i)
    {
        out[4 + i] = static_cast<uint8_t>(header.length >> (8 * i));
    }
    for (size_t i = 0; i < 8; ++i)
    {
        out[8 + i] = static_cast<uint8_t>(header.timestamp >> (8 * i));
    }
}

std::optional<Header> DecodeHeader(std::span<const uint8_t, kHeaderSize> in)
{
    Reader r(in);
    uint8_t type = 0;
    uint8_t flags = 0;
    uint16_t reserved = 0;
    Header h;
    r.Get(type);
    r.Get(flags);
    r.Get(reserved);
    r.Get(h.length);
    r.Get(h.timestamp);
    if (reserved != 0 || h.length > kMaxPayload)
    {
        return std::nullopt;
    }
    h.type = static_cast<MessageType>(type);
    h.flags = flags;
    return h;
}

std::vector<uint8_t> SerializeHello(const Hello& msg)
{
    Writer w(46 + msg.deviceName.size() + 4 * msg.sizes.size() + 2 * msg.rates.size());
    w.Put(kMagic);
    w.Put(msg.protocolVersion);
    w.Put(msg.widthPx);
    w.Put(msg.heightPx);
    w.Put(msg.densityDpi);
    w.Put(msg.refreshMilliHz);
    w.Put(msg.codecs);
    w.PutString(msg.deviceName);
    w.Put(msg.xdpiMilli);
    w.Put(msg.ydpiMilli);
    w.Put(msg.modeWidthPx);
    w.Put(msg.modeHeightPx);
    w.Put(msg.modeRefreshMilliHz);
    w.Put(msg.bitrateKbps);
    w.Put(msg.encoderQuality);
    w.Put(msg.encoderFlags);
    w.Put(static_cast<uint8_t>(msg.sizes.size()));
    for (const auto& size : msg.sizes)
    {
        w.Put(size.width);
        w.Put(size.height);
    }
    w.Put(static_cast<uint8_t>(msg.rates.size()));
    for (uint16_t rate : msg.rates)
    {
        w.Put(rate);
    }
    return w.Take();
}

std::optional<Hello> ParseHello(std::span<const uint8_t> payload)
{
    Reader r(payload);
    uint32_t magic = 0;
    Hello msg;
    if (!r.Get(magic) || magic != kMagic || !r.Get(msg.protocolVersion) || !r.Get(msg.widthPx) ||
        !r.Get(msg.heightPx) || !r.Get(msg.densityDpi) || !r.Get(msg.refreshMilliHz) || !r.Get(msg.codecs) ||
        !r.GetString(msg.deviceName))
    {
        return std::nullopt;
    }
    // Optional trailing fields: older clients don't send them.
    uint32_t xdpi = 0;
    uint32_t ydpi = 0;
    if (r.Get(xdpi) && r.Get(ydpi))
    {
        msg.xdpiMilli = xdpi;
        msg.ydpiMilli = ydpi;

        uint16_t modeWidth = 0;
        uint16_t modeHeight = 0;
        uint32_t modeRefresh = 0;
        if (r.Get(modeWidth) && r.Get(modeHeight) && r.Get(modeRefresh))
        {
            msg.modeWidthPx = modeWidth;
            msg.modeHeightPx = modeHeight;
            msg.modeRefreshMilliHz = modeRefresh;

            uint32_t bitrate = 0;
            uint8_t quality = 0;
            uint8_t flags = 0;
            if (r.Get(bitrate) && r.Get(quality) && r.Get(flags))
            {
                msg.bitrateKbps = bitrate;
                msg.encoderQuality = quality;
                msg.encoderFlags = flags;

                uint8_t sizeCount = 0;
                std::vector<Size> sizes;
                bool ok = r.Get(sizeCount);
                for (uint8_t i = 0; ok && i < sizeCount; ++i)
                {
                    Size size;
                    ok = r.Get(size.width) && r.Get(size.height);
                    sizes.push_back(size);
                }
                uint8_t rateCount = 0;
                std::vector<uint16_t> rates;
                ok = ok && r.Get(rateCount);
                for (uint8_t i = 0; ok && i < rateCount; ++i)
                {
                    uint16_t rate = 0;
                    ok = r.Get(rate);
                    rates.push_back(rate);
                }
                if (ok)
                {
                    msg.sizes = std::move(sizes);
                    msg.rates = std::move(rates);
                }
            }
        }
    }
    return msg;
}

std::vector<uint8_t> SerializeConfig(const Config& msg)
{
    Writer w(20);
    w.Put(msg.protocolVersion);
    w.Put(static_cast<uint8_t>(msg.codec));
    w.Put(uint8_t{0});
    w.Put(msg.widthPx);
    w.Put(msg.heightPx);
    w.Put(msg.fpsMilliHz);
    w.Put(msg.bitrateKbps);
    w.Put(msg.rotationDegrees);
    w.Put(static_cast<uint8_t>(msg.fullRange ? 1 : 0));
    w.Put(uint8_t{0});
    return w.Take();
}

std::optional<Config> ParseConfig(std::span<const uint8_t> payload)
{
    Reader r(payload);
    Config msg;
    uint8_t codec = 0;
    uint8_t reserved = 0;
    if (!r.Get(msg.protocolVersion) || !r.Get(codec) || !r.Get(reserved) || !r.Get(msg.widthPx) ||
        !r.Get(msg.heightPx) || !r.Get(msg.fpsMilliHz) || !r.Get(msg.bitrateKbps))
    {
        return std::nullopt;
    }
    msg.codec = static_cast<Codec>(codec);
    uint16_t rotation = 0;
    if (r.Get(rotation) && rotation % 90 == 0 && rotation < 360) // optional; older hosts omit it
    {
        msg.rotationDegrees = rotation;
    }
    uint8_t flags = 0;
    if (r.Get(flags))
    {
        msg.fullRange = (flags & 1) != 0;
    }
    return msg;
}

std::vector<uint8_t> SerializeOrientation(uint16_t degrees)
{
    Writer w(2);
    w.Put(degrees);
    return w.Take();
}

std::optional<uint16_t> ParseOrientation(std::span<const uint8_t> payload)
{
    Reader r(payload);
    uint16_t degrees = 0;
    if (!r.Get(degrees) || degrees % 90 != 0 || degrees >= 360)
    {
        return std::nullopt;
    }
    return degrees;
}

std::vector<uint8_t> SerializeAction(Action action)
{
    Writer w(1);
    w.Put(static_cast<uint8_t>(action));
    return w.Take();
}

std::optional<Action> ParseAction(std::span<const uint8_t> payload)
{
    Reader r(payload);
    uint8_t action = 0;
    if (!r.Get(action) || action < static_cast<uint8_t>(Action::Undo) ||
        action > static_cast<uint8_t>(Action::VolumeDown))
    {
        return std::nullopt;
    }
    return static_cast<Action>(action);
}

std::vector<uint8_t> SerializeMediaState(const MediaState& msg)
{
    Writer w(4);
    w.Put(static_cast<uint8_t>(msg.playback));
    w.Put(static_cast<uint8_t>(msg.muted ? 1 : 0));
    w.Put(msg.volumePercent);
    w.Put(uint8_t{0});
    return w.Take();
}

std::optional<MediaState> ParseMediaState(std::span<const uint8_t> payload)
{
    Reader r(payload);
    uint8_t playback = 0;
    uint8_t flags = 0;
    uint8_t volume = 0;
    uint8_t reserved = 0;
    if (!r.Get(playback) || !r.Get(flags) || !r.Get(volume) || !r.Get(reserved) ||
        playback > static_cast<uint8_t>(Playback::Playing) || volume > 100)
    {
        return std::nullopt;
    }
    return MediaState{static_cast<Playback>(playback), (flags & 1) != 0, volume};
}

std::vector<uint8_t> SerializeEncoderSettings(const EncoderSettings& msg)
{
    Writer w(6);
    w.Put(msg.bitrateKbps);
    w.Put(msg.quality);
    w.Put(msg.flags);
    return w.Take();
}

std::optional<EncoderSettings> ParseEncoderSettings(std::span<const uint8_t> payload)
{
    Reader r(payload);
    EncoderSettings msg;
    if (!r.Get(msg.bitrateKbps) || !r.Get(msg.quality) || !r.Get(msg.flags))
    {
        return std::nullopt;
    }
    return msg;
}

std::vector<uint8_t> SerializeDisplayMode(const DisplayMode& msg)
{
    Writer w(8);
    w.Put(msg.width);
    w.Put(msg.height);
    w.Put(msg.refreshHz);
    w.Put(uint16_t{0});
    return w.Take();
}

std::optional<DisplayMode> ParseDisplayMode(std::span<const uint8_t> payload)
{
    Reader r(payload);
    DisplayMode msg;
    uint16_t reserved = 0;
    if (!r.Get(msg.width) || !r.Get(msg.height) || !r.Get(msg.refreshHz) || !r.Get(reserved))
    {
        return std::nullopt;
    }
    return msg;
}

std::vector<uint8_t> SerializeEncoderState(const EncoderState& msg)
{
    Writer w(8);
    w.Put(msg.bitrateKbps);
    // flags: bit0 adaptive, then encoder_flags shifted up by one.
    w.Put(static_cast<uint8_t>((msg.adaptive ? 1 : 0) | msg.flags << 1));
    w.Put(msg.quality);
    w.Put(uint16_t{0});
    return w.Take();
}

std::optional<EncoderState> ParseEncoderState(std::span<const uint8_t> payload)
{
    Reader r(payload);
    EncoderState msg;
    uint8_t flags = 0;
    uint16_t reserved = 0;
    if (!r.Get(msg.bitrateKbps) || !r.Get(flags) || !r.Get(msg.quality) || !r.Get(reserved))
    {
        return std::nullopt;
    }
    msg.adaptive = (flags & 1) != 0;
    msg.flags = static_cast<uint8_t>(flags >> 1);
    return msg;
}

std::vector<uint8_t> SerializePing(uint64_t pingId)
{
    Writer w(8);
    w.Put(pingId);
    return w.Take();
}

std::optional<uint64_t> ParsePing(std::span<const uint8_t> payload)
{
    Reader r(payload);
    uint64_t id = 0;
    if (!r.Get(id))
    {
        return std::nullopt;
    }
    return id;
}

std::vector<uint8_t> SerializePong(const Pong& msg)
{
    Writer w(16);
    w.Put(msg.pingId);
    w.Put(msg.pingTimestamp);
    return w.Take();
}

std::optional<Pong> ParsePong(std::span<const uint8_t> payload)
{
    Reader r(payload);
    Pong msg;
    if (!r.Get(msg.pingId) || !r.Get(msg.pingTimestamp))
    {
        return std::nullopt;
    }
    return msg;
}

std::vector<uint8_t> SerializeFrameStats(const FrameStats& msg)
{
    Writer w(32);
    w.Put(msg.captureTs);
    w.Put(msg.receivedTs);
    w.Put(msg.decodedTs);
    w.Put(msg.renderedTs);
    return w.Take();
}

std::optional<FrameStats> ParseFrameStats(std::span<const uint8_t> payload)
{
    Reader r(payload);
    FrameStats msg;
    if (!r.Get(msg.captureTs) || !r.Get(msg.receivedTs) || !r.Get(msg.decodedTs) || !r.Get(msg.renderedTs))
    {
        return std::nullopt;
    }
    return msg;
}

std::vector<uint8_t> SerializeNonce(const Nonce& nonce)
{
    Writer w(kNonceSize);
    w.PutBytes(nonce);
    return w.Take();
}

std::optional<Nonce> ParseNonce(std::span<const uint8_t> payload)
{
    Reader r(payload);
    Nonce nonce{};
    if (!r.GetBytes(nonce))
    {
        return std::nullopt;
    }
    return nonce;
}

std::vector<uint8_t> SerializeMac(const Mac& mac)
{
    Writer w(kMacSize);
    w.PutBytes(mac);
    return w.Take();
}

std::optional<Mac> ParseMac(std::span<const uint8_t> payload)
{
    Reader r(payload);
    Mac mac{};
    if (!r.GetBytes(mac))
    {
        return std::nullopt;
    }
    return mac;
}

std::vector<uint8_t> SerializeAuthResponse(const AuthResponse& msg)
{
    Writer w(kNonceSize + kMacSize);
    w.PutBytes(msg.clientNonce);
    w.PutBytes(msg.mac);
    return w.Take();
}

std::optional<AuthResponse> ParseAuthResponse(std::span<const uint8_t> payload)
{
    Reader r(payload);
    AuthResponse msg;
    if (!r.GetBytes(msg.clientNonce) || !r.GetBytes(msg.mac))
    {
        return std::nullopt;
    }
    return msg;
}

std::vector<uint8_t> SerializePairing(const PairingInfo& msg)
{
    Writer w(4 + msg.code.size() + msg.hostName.size());
    w.PutString(msg.code);
    w.PutString(msg.hostName);
    return w.Take();
}

std::optional<PairingInfo> ParsePairing(std::span<const uint8_t> payload)
{
    Reader r(payload);
    PairingInfo msg;
    if (!r.GetString(msg.code) || !r.GetString(msg.hostName))
    {
        return std::nullopt;
    }
    return msg;
}

std::vector<uint8_t> SerializeTouchFrame(const TouchFrame& msg)
{
    Writer w(4 + 8 * msg.contacts.size());
    w.Put(kInputKindTouch);
    w.Put(static_cast<uint8_t>(msg.contacts.size()));
    w.Put(uint16_t{0});
    for (const auto& c : msg.contacts)
    {
        w.Put(c.id);
        w.Put(static_cast<uint8_t>(c.action));
        w.Put(c.x);
        w.Put(c.y);
        w.Put(c.pressure);
    }
    return w.Take();
}

std::optional<TouchFrame> ParseTouchFrame(std::span<const uint8_t> payload)
{
    Reader r(payload);
    uint8_t kind = 0;
    uint8_t count = 0;
    uint16_t reserved = 0;
    if (!r.Get(kind) || kind != kInputKindTouch || !r.Get(count) || count == 0 || count > kMaxTouchContacts ||
        !r.Get(reserved))
    {
        return std::nullopt;
    }
    TouchFrame msg;
    msg.contacts.resize(count);
    for (auto& c : msg.contacts)
    {
        uint8_t action = 0;
        if (!r.Get(c.id) || !r.Get(action) || !r.Get(c.x) || !r.Get(c.y) || !r.Get(c.pressure) ||
            c.id >= kMaxTouchContacts || action > static_cast<uint8_t>(TouchAction::Cancel))
        {
            return std::nullopt;
        }
        c.action = static_cast<TouchAction>(action);
    }
    return msg;
}

} // namespace dd::protocol
