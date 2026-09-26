#include "Protocol.h"

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
    Writer w(38 + msg.deviceName.size());
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
        }
    }
    return msg;
}

std::vector<uint8_t> SerializeConfig(const Config& msg)
{
    Writer w(16);
    w.Put(msg.protocolVersion);
    w.Put(static_cast<uint8_t>(msg.codec));
    w.Put(uint8_t{0});
    w.Put(msg.widthPx);
    w.Put(msg.heightPx);
    w.Put(msg.fpsMilliHz);
    w.Put(msg.bitrateKbps);
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
