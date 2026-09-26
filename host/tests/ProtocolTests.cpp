#include "Check.h"

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

    auto bytes = SerializeHello(h);
    CHECK(bytes.size() == 20 + 2 + h.deviceName.size() + 8 + 8);
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

    bytes.push_back(0xAA); // appended future field must be ignored
    CHECK(ParseHello(bytes).has_value());

    bytes.resize(bytes.size() - 9); // older client: DPI but no requested mode
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
    Config c{kVersion, Codec::H264, 2408, 1720, 60000, 20000};
    auto bytes = SerializeConfig(c);
    CHECK(bytes.size() == 16);
    auto p = ParseConfig(bytes);
    CHECK(p.has_value());
    CHECK(p->codec == Codec::H264 && p->widthPx == 2408 && p->heightPx == 1720);
    CHECK(p->fpsMilliHz == 60000 && p->bitrateKbps == 20000);
    CHECK(!ParseConfig(std::span(bytes).first(15)).has_value());
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

} // namespace

void RunProtocolTests()
{
    HeaderRoundTrip();
    HeaderRejectsBadInput();
    HelloRoundTrip();
    ConfigRoundTrip();
    PingPongStatsRoundTrip();
}
