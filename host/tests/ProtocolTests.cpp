#include "Check.h"

#include "../src/protocol/Pairing.h"
#include "../src/protocol/Protocol.h"

#include <array>

using namespace dd::protocol;

namespace
{

void HeaderRoundTrip()
{
    Header h{MessageType::VideoFrame, video_flags::kKeyframe | video_flags::kCodecConfig, 123456,
             0x0102030405060708ull};
    std::array<uint8_t, kHeaderSize> buf{};
    EncodeHeader(h, buf);

    // Byte layout is part of the spec: type, flags, reserved(2), length LE, timestamp LE.
    CHECK(buf[0] == 0x10);
    CHECK(buf[1] == 0x03);
    CHECK(buf[2] == 0 && buf[3] == 0);
    CHECK(buf[4] == 0x40 && buf[5] == 0xE2 && buf[6] == 0x01 && buf[7] == 0x00);
    CHECK(buf[8] == 0x08 && buf[15] == 0x01);

    auto d = DecodeHeader(buf);
    CHECK(d.has_value());
    CHECK(d->type == MessageType::VideoFrame);
    CHECK(d->flags == h.flags);
    CHECK(d->length == h.length);
    CHECK(d->timestamp == h.timestamp);
}

void HeaderRejectsBadInput()
{
    std::array<uint8_t, kHeaderSize> buf{};
    EncodeHeader({MessageType::Ping, 0, kMaxPayload + 1, 0}, buf);
    CHECK(!DecodeHeader(buf).has_value());

    EncodeHeader({MessageType::Ping, 0, 8, 0}, buf);
    buf[2] = 1;
    CHECK(!DecodeHeader(buf).has_value());
}

void HelloRoundTrip()
{
    Hello h;
    h.widthPx = 2408;
    h.heightPx = 1720;
    h.densityDpi = 360;
    h.refreshMilliHz = 90000;
    h.codecs = kCodecMaskH264 | kCodecMaskHevc;
    h.deviceName = "OnePlus OPD2305";
    h.xdpiMilli = 260047;
    h.ydpiMilli = 260268;
    h.modeWidthPx = 1920;
    h.modeHeightPx = 1370;
    h.modeRefreshMilliHz = 60000;
    h.bitrateKbps = 45000;
    h.encoderQuality = 30;
    h.sizes = {{2408, 1720}, {1806, 1290}};
    h.rates = {90, 60, 30};

    auto bytes = SerializeHello(h);
    CHECK(bytes.size() == 20 + 2 + h.deviceName.size() + 8 + 8 + 6 + 1 + 8 + 1 + 6);
    CHECK(bytes[0] == 'D' && bytes[1] == 'X' && bytes[2] == 'D' && bytes[3] == 'P');

    auto p = ParseHello(bytes);
    CHECK(p.has_value());
    CHECK(p->protocolVersion == kVersion);
    CHECK(p->widthPx == 2408 && p->heightPx == 1720 && p->densityDpi == 360);
    CHECK(p->refreshMilliHz == 90000);
    CHECK(p->codecs == (kCodecMaskH264 | kCodecMaskHevc));
    CHECK(p->deviceName == h.deviceName);
    CHECK(p->xdpiMilli == 260047 && p->ydpiMilli == 260268);
    CHECK(p->modeWidthPx == 1920 && p->modeHeightPx == 1370 && p->modeRefreshMilliHz == 60000);
    CHECK(p->bitrateKbps == 45000 && p->encoderQuality == 30);
    CHECK(p->sizes == h.sizes && p->rates == h.rates);

    bytes.push_back(0xAA); // appended future field must be ignored
    CHECK(ParseHello(bytes).has_value());

    bytes.resize(bytes.size() - 1 - 16); // no mode lists
    auto noLists = ParseHello(bytes);
    CHECK(noLists.has_value() && noLists->bitrateKbps == 45000 && noLists->sizes.empty() && noLists->rates.empty());

    bytes.resize(bytes.size() - 6); // requested mode but no encoder settings
    auto noEncoder = ParseHello(bytes);
    CHECK(noEncoder.has_value() && noEncoder->modeWidthPx == 1920 && noEncoder->bitrateKbps == 0 &&
          noEncoder->encoderQuality == kQualityHostDecides);

    bytes.resize(bytes.size() - 8); // older client: DPI but no requested mode
    auto noMode = ParseHello(bytes);
    CHECK(noMode.has_value() && noMode->xdpiMilli == 260047 && noMode->modeWidthPx == 0 &&
          noMode->modeRefreshMilliHz == 0);

    bytes.resize(bytes.size() - 8); // older client: no optional DPI fields
    auto old = ParseHello(bytes);
    CHECK(old.has_value() && old->xdpiMilli == 0 && old->deviceName == h.deviceName);

    bytes.resize(bytes.size() - 3); // truncated string
    CHECK(!ParseHello(bytes).has_value());

    auto bad = SerializeHello(h);
    bad[0] = 'X';
    CHECK(!ParseHello(bad).has_value());
}

void ConfigRoundTrip()
{
    Config c{kVersion, Codec::H264, 2408, 1720, 60000, 20000, 90};
    auto bytes = SerializeConfig(c);
    CHECK(bytes.size() == 18);
    auto p = ParseConfig(bytes);
    CHECK(p.has_value());
    CHECK(p->codec == Codec::H264 && p->widthPx == 2408 && p->heightPx == 1720);
    CHECK(p->fpsMilliHz == 60000 && p->bitrateKbps == 20000);
    CHECK(p->rotationDegrees == 90);

    auto old = ParseConfig(std::span(bytes).first(16)); // older host: no rotation field
    CHECK(old.has_value() && old->rotationDegrees == 0);
    CHECK(!ParseConfig(std::span(bytes).first(15)).has_value());
}

void OrientationRoundTrip()
{
    CHECK(ParseOrientation(SerializeOrientation(0)) == uint16_t{0});
    CHECK(ParseOrientation(SerializeOrientation(90)) == uint16_t{90});
    CHECK(!ParseOrientation(SerializeOrientation(45)).has_value());
    CHECK(!ParseOrientation(SerializeOrientation(360)).has_value());
    CHECK(!ParseOrientation({}).has_value());
}

void DisplayModeRoundTrip()
{
    // Byte layout is shared with the Android tests (ProtocolTest.kt).
    const DisplayMode mode{1806, 1290, 90};
    const auto bytes = SerializeDisplayMode(mode);
    CHECK((bytes == std::vector<uint8_t>{0x0E, 0x07, 0x0A, 0x05, 90, 0, 0, 0}));
    CHECK(ParseDisplayMode(bytes) == mode);
    CHECK(!ParseDisplayMode(std::span(bytes).first(7)).has_value());
}

void EncoderMessagesRoundTrip()
{
    // Byte layout is shared with the Android tests (ProtocolTest.kt).
    const EncoderSettings settings{45000, 30};
    const auto settingsBytes = SerializeEncoderSettings(settings);
    CHECK((settingsBytes == std::vector<uint8_t>{0xC8, 0xAF, 0x00, 0x00, 30, 0}));
    CHECK(ParseEncoderSettings(settingsBytes) == settings);
    CHECK(!ParseEncoderSettings(std::span(settingsBytes).first(5)).has_value());

    const EncoderState state{62000, true, 0};
    const auto stateBytes = SerializeEncoderState(state);
    CHECK((stateBytes == std::vector<uint8_t>{0x30, 0xF2, 0x00, 0x00, 1, 0, 0, 0}));
    CHECK(ParseEncoderState(stateBytes) == state);
    CHECK(!ParseEncoderState(std::span(stateBytes).first(7)).has_value());
}

void ActionAndMediaStateRoundTrip()
{
    CHECK(ParseAction(SerializeAction(Action::Undo)) == Action::Undo);
    CHECK(ParseAction(SerializeAction(Action::VolumeDown)) == Action::VolumeDown);
    const std::array<uint8_t, 1> unknown{0x7F};
    CHECK(!ParseAction(unknown).has_value());
    const std::array<uint8_t, 1> retiredCopy{2}; // 0.2.1 clients may still send it
    CHECK(!ParseAction(retiredCopy).has_value());
    CHECK(!ParseAction({}).has_value());

    const MediaState state{Playback::Playing, true, 45};
    const auto bytes = SerializeMediaState(state);
    CHECK(bytes.size() == 4 && bytes[0] == 2 && bytes[1] == 1 && bytes[2] == 45 && bytes[3] == 0);
    CHECK(ParseMediaState(bytes) == state);
    const std::array<uint8_t, 4> loud{1, 0, 101, 0};
    CHECK(!ParseMediaState(loud).has_value());
    CHECK(!ParseMediaState(std::span(bytes).first(3)).has_value());
}

void PingPongStatsRoundTrip()
{
    CHECK(ParsePing(SerializePing(42)) == 42u);
    CHECK(!ParsePing({}).has_value());

    auto pong = ParsePong(SerializePong({7, 99}));
    CHECK(pong.has_value() && pong->pingId == 7 && pong->pingTimestamp == 99);

    auto stats = ParseFrameStats(SerializeFrameStats({1, 2, 3, 4}));
    CHECK(stats.has_value() && stats->captureTs == 1 && stats->receivedTs == 2 && stats->decodedTs == 3 &&
          stats->renderedTs == 4);
}

void TouchFrameRoundTrip()
{
    TouchFrame t;
    t.contacts.push_back({1, TouchAction::Down, 0x1234, 0xABCD, 512});
    auto bytes = SerializeTouchFrame(t);
    // Same vector as android/.../ProtocolTest.kt.
    const std::vector<uint8_t> expected = {0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x34, 0x12, 0xCD, 0xAB, 0x00, 0x02};
    CHECK(bytes == expected);

    auto p = ParseTouchFrame(bytes);
    CHECK(p.has_value() && p->contacts.size() == 1);
    CHECK(p->contacts[0].id == 1 && p->contacts[0].action == TouchAction::Down && p->contacts[0].x == 0x1234 &&
          p->contacts[0].y == 0xABCD && p->contacts[0].pressure == 512);

    CHECK(!ParseTouchFrame(std::span(bytes).first(11)).has_value()); // truncated contact
    auto bad = bytes;
    bad[0] = 2; // unknown input kind
    CHECK(!ParseTouchFrame(bad).has_value());
    bad = bytes;
    bad[4] = 10; // slot out of range
    CHECK(!ParseTouchFrame(bad).has_value());
    bad = bytes;
    bad[5] = 4; // unknown action
    CHECK(!ParseTouchFrame(bad).has_value());
    bad = bytes;
    bad[1] = 0; // no contacts
    CHECK(!ParseTouchFrame(bad).has_value());
}

template <size_t N> std::array<uint8_t, N> FromHex(const char* hex)
{
    std::array<uint8_t, N> out{};
    for (size_t i = 0; i < N; ++i)
    {
        auto nibble = [](char c) { return static_cast<uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10); };
        out[i] = static_cast<uint8_t>(nibble(hex[2 * i]) << 4 | nibble(hex[2 * i + 1]));
    }
    return out;
}

void AuthMessagesRoundTrip()
{
    Nonce nonce{};
    for (size_t i = 0; i < nonce.size(); ++i)
    {
        nonce[i] = static_cast<uint8_t>(i);
    }
    CHECK(ParseNonce(SerializeNonce(nonce)) == nonce);
    const auto nonceBytes = SerializeNonce(nonce);
    CHECK(!ParseNonce(std::span(nonceBytes).first(15)).has_value());

    Mac mac{};
    mac[31] = 0xEE;
    CHECK(ParseMac(SerializeMac(mac)) == mac);

    const auto response = SerializeAuthResponse({nonce, mac});
    CHECK(response.size() == 48 && response[0] == 0 && response[16 + 31] == 0xEE);
    auto parsed = ParseAuthResponse(response);
    CHECK(parsed.has_value() && parsed->clientNonce == nonce && parsed->mac == mac);
    CHECK(!ParseAuthResponse(std::span(response).first(47)).has_value());

    const auto pairing = SerializePairing({"ABCDE-FGHIJ-KLMNO-PQRST", "DESKTOP"});
    auto p = ParsePairing(pairing);
    CHECK(p.has_value() && p->code == "ABCDE-FGHIJ-KLMNO-PQRST" && p->hostName == "DESKTOP");
    CHECK(!ParsePairing(std::span(pairing).first(pairing.size() - 1)).has_value());
}

void PairingCodes()
{
    CHECK(NormalizePairingCode("abcde-fghij klmno-pqrst") == std::string("ABCDEFGHIJKLMNOPQRST"));
    CHECK(!NormalizePairingCode("ABCDE-FGHIJ-KLMNO-PQRS").has_value()); // too short
    CHECK(!NormalizePairingCode("ABCDE-FGHIJ-KLMNO-PQRS1").has_value()); // '1' isn't Base32
    CHECK(FormatPairingCode("ABCDEFGHIJKLMNOPQRST") == "ABCDE-FGHIJ-KLMNO-PQRST");

    const std::string generated = GeneratePairingCode();
    CHECK(NormalizePairingCode(generated) == generated);
    CHECK(GeneratePairingCode() != generated);
}

void PairingVectors()
{
    // Same vectors as docs/wire-protocol.md and android/.../PairingTest.kt.
    auto secrets = DerivePairingSecrets("ABCDEFGHIJKLMNOPQRST");
    CHECK(secrets.has_value());
    CHECK(secrets->ssid == "DeuxDisplay-9968");
    CHECK(secrets->passphrase == "3252SPVCLUVNTF5LBJDW4VOV");
    CHECK(secrets->authKey == FromHex<32>("29a36685e1e1d7687dbc70c81ffd8a402317f3a748ca2e00b9cbcae5b996a64e"));

    Nonce hostNonce{};
    Nonce clientNonce{};
    for (size_t i = 0; i < kNonceSize; ++i)
    {
        hostNonce[i] = static_cast<uint8_t>(i);
        clientNonce[i] = static_cast<uint8_t>(16 + i);
    }
    const Mac client = ClientAuthMac(secrets->authKey, hostNonce, clientNonce);
    const Mac host = HostAuthMac(secrets->authKey, clientNonce, hostNonce);
    CHECK(client == FromHex<32>("ee895399e449a1562e0db0c6656f9a09e9a0f9655a0a09e6c7717a36935dd2cb"));
    CHECK(host == FromHex<32>("cfa68d242928b528cccf900c86d75c21e6434ff4595955445482ac9ef4ec50c8"));
    CHECK(MacEquals(client, client) && !MacEquals(client, host));

    Nonce a{};
    Nonce b{};
    CHECK(RandomNonce(a) && RandomNonce(b) && a != b);
}

} // namespace

void RunProtocolTests()
{
    HeaderRoundTrip();
    HeaderRejectsBadInput();
    HelloRoundTrip();
    ConfigRoundTrip();
    OrientationRoundTrip();
    ActionAndMediaStateRoundTrip();
    EncoderMessagesRoundTrip();
    DisplayModeRoundTrip();
    PingPongStatsRoundTrip();
    TouchFrameRoundTrip();
    AuthMessagesRoundTrip();
    PairingCodes();
    PairingVectors();
}
