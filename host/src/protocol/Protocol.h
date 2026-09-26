#pragma once

// Implements docs/wire-protocol.md. Keep the two in sync.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace dd::protocol
{

inline constexpr uint16_t kVersion = 1;
inline constexpr uint32_t kMagic = 0x50445844; // "DXDP" little-endian
inline constexpr size_t kHeaderSize = 16;
inline constexpr uint32_t kMaxPayload = 16u * 1024 * 1024;

enum class MessageType : uint8_t
{
    Hello = 0x01,
    Config = 0x02,
    Bye = 0x03,
    VideoFrame = 0x10,
    RequestKeyframe = 0x11,
    Ping = 0x20,
    Pong = 0x21,
    FrameStats = 0x22,
    Cursor = 0x30,
    Input = 0x40,
};

namespace video_flags
{
inline constexpr uint8_t kKeyframe = 1u << 0;
inline constexpr uint8_t kCodecConfig = 1u << 1;
} // namespace video_flags

enum class Codec : uint8_t
{
    H264 = 0,
    Hevc = 1,
};

inline constexpr uint32_t kCodecMaskH264 = 1u << 0;
inline constexpr uint32_t kCodecMaskHevc = 1u << 1;

struct Header
{
    MessageType type{};
    uint8_t flags = 0;
    uint32_t length = 0;
    uint64_t timestamp = 0;
};

struct Hello
{
    uint16_t protocolVersion = kVersion;
    uint16_t widthPx = 0;
    uint16_t heightPx = 0;
    uint16_t densityDpi = 0;
    uint32_t refreshMilliHz = 0;
    uint32_t codecs = 0;
    std::string deviceName;
    uint32_t xdpiMilli = 0; // optional, 0 = not sent
    uint32_t ydpiMilli = 0;
};

struct Config
{
    uint16_t protocolVersion = kVersion;
    Codec codec = Codec::H264;
    uint16_t widthPx = 0;
    uint16_t heightPx = 0;
    uint32_t fpsMilliHz = 0;
    uint32_t bitrateKbps = 0;
};

struct Pong
{
    uint64_t pingId = 0;
    uint64_t pingTimestamp = 0;
};

struct FrameStats
{
    uint64_t captureTs = 0;
    uint64_t receivedTs = 0;
    uint64_t decodedTs = 0;
    uint64_t renderedTs = 0;
};

void EncodeHeader(const Header& header, std::span<uint8_t, kHeaderSize> out);

// Returns nullopt for a malformed header (reserved bits set, oversized payload).
std::optional<Header> DecodeHeader(std::span<const uint8_t, kHeaderSize> in);

// Payload (de)serialisers. Parsers return nullopt on truncated/invalid input and
// ignore trailing bytes so peers can append fields without a version bump.
std::vector<uint8_t> SerializeHello(const Hello& msg);
std::optional<Hello> ParseHello(std::span<const uint8_t> payload);

std::vector<uint8_t> SerializeConfig(const Config& msg);
std::optional<Config> ParseConfig(std::span<const uint8_t> payload);

std::vector<uint8_t> SerializePing(uint64_t pingId);
std::optional<uint64_t> ParsePing(std::span<const uint8_t> payload);

std::vector<uint8_t> SerializePong(const Pong& msg);
std::optional<Pong> ParsePong(std::span<const uint8_t> payload);

std::vector<uint8_t> SerializeFrameStats(const FrameStats& msg);
std::optional<FrameStats> ParseFrameStats(std::span<const uint8_t> payload);

} // namespace dd::protocol
