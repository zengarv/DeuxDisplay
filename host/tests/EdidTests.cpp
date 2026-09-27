#include "Check.h"

#include "../../driver/DeuxDisplayIdd/Edid.h"

#include <cstring>

using namespace dd::edid;

namespace
{

constexpr MonitorDescription kPadGo = {{2408, 1720, 60}, {2408, 1720, 90}, 235, 168, 1, "DeuxDisplay"};

struct DecodedTiming
{
    unsigned clock10kHz, width, hblank, height, vblank, widthMm, heightMm;
};

DecodedTiming DecodeDtd(const uint8_t* d)
{
    return {
        static_cast<unsigned>(d[0] | d[1] << 8),
        static_cast<unsigned>(d[2] | (d[4] >> 4) << 8),
        static_cast<unsigned>(d[3] | (d[4] & 0x0F) << 8),
        static_cast<unsigned>(d[5] | (d[7] >> 4) << 8),
        static_cast<unsigned>(d[6] | (d[7] & 0x0F) << 8),
        static_cast<unsigned>(d[12] | (d[14] >> 4) << 8),
        static_cast<unsigned>(d[13] | (d[14] & 0x0F) << 8),
    };
}

void HeaderAndChecksum()
{
    const auto e = BuildEdid(kPadGo);
    const uint8_t header[] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    CHECK(std::memcmp(e.data(), header, sizeof(header)) == 0);

    unsigned sum = 0;
    for (uint8_t b : e)
    {
        sum += b;
    }
    CHECK(sum % 256 == 0);
    CHECK(e[18] == 1 && e[19] == 4);
    CHECK(e[126] == 0);
}

void Manufacturer()
{
    const auto e = BuildEdid(kPadGo);
    const unsigned id = e[8] << 8 | e[9];
    CHECK((id >> 15) == 0);
    CHECK(static_cast<char>('A' - 1 + ((id >> 10) & 0x1F)) == 'D');
    CHECK(static_cast<char>('A' - 1 + ((id >> 5) & 0x1F)) == 'X');
    CHECK(static_cast<char>('A' - 1 + (id & 0x1F)) == 'D');
}

void ProductCode()
{
    // Product 1 keeps the original single-monitor identity (MONITOR\DXD0001); the host finds
    // further monitors by their product code (little-endian).
    CHECK(BuildEdid(kPadGo)[10] == 0x01 && BuildEdid(kPadGo)[11] == 0x00);
    MonitorDescription second = kPadGo;
    second.product = 2;
    second.name = "DeuxDisplay 2";
    const auto e = BuildEdid(second);
    CHECK(e[10] == 0x02 && e[11] == 0x00);
    CHECK(std::memcmp(&e[95], "DeuxDisplay 2", 13) == 0);
    CHECK(e != BuildEdid(kPadGo));
}

void PhysicalSize()
{
    const auto e = BuildEdid(kPadGo);
    CHECK(e[21] == 24); // cm, rounded from 235 mm
    CHECK(e[22] == 17); // cm, rounded from 168 mm
}

void TimingDescriptors()
{
    const auto e = BuildEdid(kPadGo);

    const auto preferred = DecodeDtd(&e[54]);
    CHECK(preferred.width == 2408 && preferred.height == 1720);
    CHECK(preferred.hblank == kHBlank && preferred.vblank == kVBlank);
    CHECK(preferred.widthMm == 235 && preferred.heightMm == 168);
    CHECK(preferred.clock10kHz == 27118); // 2568 * 1760 * 60 Hz

    const auto secondary = DecodeDtd(&e[72]);
    CHECK(secondary.width == 2408 && secondary.height == 1720);
    CHECK(secondary.clock10kHz == 40677); // 2568 * 1760 * 90 Hz

    const double refresh = secondary.clock10kHz * 10000.0 / ((secondary.width + secondary.hblank) *
                                                             double(secondary.height + secondary.vblank));
    CHECK(refresh > 89.99 && refresh < 90.01);
}

void NameDescriptor()
{
    const auto e = BuildEdid(kPadGo);
    CHECK(e[90] == 0 && e[91] == 0 && e[92] == 0 && e[93] == 0xFC);
    CHECK(std::memcmp(&e[95], "DeuxDisplay\n ", 13) == 0);
}

void NoSecondaryTiming()
{
    MonitorDescription m = kPadGo;
    m.secondary = {};
    const auto e = BuildEdid(m);
    CHECK(e[72] == 0 && e[73] == 0 && e[75] == 0x10);
}

void DecodesOwnTimings()
{
    const auto e = BuildEdid(kPadGo);
    const auto timings = DetailedTimings(e.data(), e.size());
    CHECK(timings.size() == 2);
    if (timings.size() == 2)
    {
        CHECK(timings[0].width == 2408 && timings[0].height == 1720 && timings[0].refreshHz == 60);
        CHECK(timings[1].width == 2408 && timings[1].height == 1720 && timings[1].refreshHz == 90);
    }
    CHECK(DetailedTimings(e.data(), 64).empty());
}

} // namespace

void RunEdidTests()
{
    DecodesOwnTimings();
    HeaderAndChecksum();
    Manufacturer();
    ProductCode();
    PhysicalSize();
    TimingDescriptors();
    NameDescriptor();
    NoSecondaryTiming();
}
