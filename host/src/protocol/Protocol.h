#pragma once

// Implements docs/wire-protocol.md. Keep the two in sync.

#include <array>
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
    AuthChallenge = 0x04,
    AuthResponse = 0x05,
    AuthOk = 0x06,
    Pairing = 0x07,
    VideoFrame = 0x10,
    RequestKeyframe = 0x11,
    Ping = 0x20,
    Pong = 0x21,
    FrameStats = 0x22,
    Cursor = 0x30,
    MediaState = 0x31,
    EncoderState = 0x32,
    Input = 0x40,
    Orientation = 0x41,
    Action = 0x42,
    EncoderSettings = 0x43,
};

namespace video_flags
{
inline constexpr uint8_t kKeyframe = 1u << 0;
inline constexpr uint8_t kCodecConfig = 1u << 1;
inline constexpr uint8_t kRepeat = 1u << 2; // identical to the previous frame; flushes decoder pipelines
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

inline constexpr uint8_t kQualityHostDecides = 0xFF;

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
    // Optional stream mode picked by the user; 0 = host decides.
    uint16_t modeWidthPx = 0;
    uint16_t modeHeightPx = 0;
    uint32_t modeRefreshMilliHz = 0;
    // Optional encoder settings picked by the user (see EncoderSettings).
    uint32_t bitrateKbps = 0;                  // 0 = host decides
    uint8_t encoderQuality = kQualityHostDecides; // 0 fastest .. 100 best
};

struct Config
{
    uint16_t protocolVersion = kVersion;
    Codec codec = Codec::H264;
    uint16_t widthPx = 0;
    uint16_t heightPx = 0;
    uint32_t fpsMilliHz = 0;
    uint32_t bitrateKbps = 0;
    // Optional: degrees clockwise (0/90/180/270) the client rotates each frame to show it upright.
    // Frames are always encoded in the display's native (landscape) scan-out orientation.
    uint16_t rotationDegrees = 0;
};

// ORIENTATION payload: the desktop orientation the client wants, in degrees clockwise from the
// display's native landscape (0 = landscape, 90 = portrait; 180/270 = flipped variants).
std::vector<uint8_t> SerializeOrientation(uint16_t degrees);
std::optional<uint16_t> ParseOrientation(std::span<const uint8_t> payload);

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

inline constexpr uint8_t kInputKindTouch = 1;
inline constexpr size_t kMaxTouchContacts = 10;

enum class TouchAction : uint8_t
{
    Down = 0,
    Move = 1,
    Up = 2,
    Cancel = 3,
};

struct TouchContact
{
    uint8_t id = 0; // slot, < kMaxTouchContacts
    TouchAction action = TouchAction::Move;
    uint16_t x = 0; // 0..65535 across the video frame
    uint16_t y = 0;
    uint16_t pressure = 0; // 0..1024, 0 = unknown
};

// INPUT payload of kind kInputKindTouch: every contact currently on the client's screen.
struct TouchFrame
{
    std::vector<TouchContact> contacts;
};

// ACTION payload: a shortcut from the client's dock. The host picks the keys, so clients don't
// depend on Windows key codes.
// Values 1-3 (cut, copy, paste in 0.2.1) are retired: Windows touch already offers them.
enum class Action : uint8_t
{
    Undo = 4,
    Redo = 5,
    TaskView = 6,
    PlayPause = 7,
    VolumeUp = 8,
    VolumeDown = 9,
};

std::vector<uint8_t> SerializeAction(Action action);
// nullopt for an empty payload or an action this host doesn't know.
std::optional<Action> ParseAction(std::span<const uint8_t> payload);

enum class Playback : uint8_t
{
    None = 0, // no media session
    Paused = 1,
    Playing = 2,
};

// MEDIA_STATE payload: what the dock shows. Sent after CONFIG and whenever it changes.
struct MediaState
{
    Playback playback = Playback::None;
    bool muted = false;
    uint8_t volumePercent = 0; // 0..100

    bool operator==(const MediaState&) const = default;
};

std::vector<uint8_t> SerializeMediaState(const MediaState& msg);
std::optional<MediaState> ParseMediaState(std::span<const uint8_t> payload);

// ENCODER_SETTINGS (client -> host): the user's bitrate and quality picks, applied live.
struct EncoderSettings
{
    uint32_t bitrateKbps = 0;                     // 0 = host decides (adaptive by default)
    uint8_t quality = kQualityHostDecides;        // 0 fastest .. 100 best, 0xFF = host decides

    bool operator==(const EncoderSettings&) const = default;
};

std::vector<uint8_t> SerializeEncoderSettings(const EncoderSettings& msg);
std::optional<EncoderSettings> ParseEncoderSettings(std::span<const uint8_t> payload);

// ENCODER_STATE (host -> client): what the encoder is running at now.
struct EncoderState
{
    uint32_t bitrateKbps = 0;
    bool adaptive = false; // the host adjusts the bitrate itself
    uint8_t quality = 0;   // 0 fastest .. 100 best

    bool operator==(const EncoderState&) const = default;
};

std::vector<uint8_t> SerializeEncoderState(const EncoderState& msg);
std::optional<EncoderState> ParseEncoderState(std::span<const uint8_t> payload);

inline constexpr size_t kNonceSize = 16;
inline constexpr size_t kMacSize = 32;
using Nonce = std::array<uint8_t, kNonceSize>;
using Mac = std::array<uint8_t, kMacSize>;

// AUTH_RESPONSE (Wi-Fi): the client's nonce and its proof that it knows the pairing code.
struct AuthResponse
{
    Nonce clientNonce{};
    Mac mac{};
};

// PAIRING (USB): hands the client the code it needs for Wi-Fi sessions.
struct PairingInfo
{
    std::string code;
    std::string hostName;
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

// AUTH_CHALLENGE carries a Nonce, AUTH_OK a Mac.
std::vector<uint8_t> SerializeNonce(const Nonce& nonce);
std::optional<Nonce> ParseNonce(std::span<const uint8_t> payload);
std::vector<uint8_t> SerializeMac(const Mac& mac);
std::optional<Mac> ParseMac(std::span<const uint8_t> payload);

std::vector<uint8_t> SerializeAuthResponse(const AuthResponse& msg);
std::optional<AuthResponse> ParseAuthResponse(std::span<const uint8_t> payload);

std::vector<uint8_t> SerializePairing(const PairingInfo& msg);
std::optional<PairingInfo> ParsePairing(std::span<const uint8_t> payload);

std::vector<uint8_t> SerializeTouchFrame(const TouchFrame& msg);
// nullopt for other input kinds, bad slots/actions, or a count outside 1..kMaxTouchContacts.
std::optional<TouchFrame> ParseTouchFrame(std::span<const uint8_t> payload);

} // namespace dd::protocol
