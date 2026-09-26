// Copyright (c) 2026 DeuxDisplay contributors. MIT License (see LICENSE at the repo root).
// Original code, not derived from the Microsoft sample.
//
// Builds a 128-byte EDID 1.4 base block describing the virtual monitor.
// Header-only with no Windows/WDK dependencies so host unit tests can cover it.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dd::edid
{

inline constexpr size_t kEdidSize = 128;

struct Timing
{
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t refreshHz = 0;
};

struct MonitorDescription
{
    Timing preferred;
    Timing secondary; // width == 0 means "none"
    uint16_t widthMm = 0;
    uint16_t heightMm = 0;
    uint32_t serial = 1;
    const char* name = "DeuxDisplay"; // up to 13 chars are used
};

// Reduced blanking: the link is virtual, so blanking only needs to be plausible.
inline constexpr uint16_t kHBlank = 160;
inline constexpr uint16_t kHFrontPorch = 48;
inline constexpr uint16_t kHSync = 32;
inline constexpr uint16_t kVBlank = 40;
inline constexpr uint16_t kVFrontPorch = 3;
inline constexpr uint16_t kVSync = 10;

// Pixel clock in units of 10 kHz, as stored in a detailed timing descriptor.
constexpr uint16_t PixelClock10kHz(const Timing& t)
{
    const uint64_t hz = static_cast<uint64_t>(t.width + kHBlank) * static_cast<uint64_t>(t.height + kVBlank) *
                        t.refreshHz;
    return static_cast<uint16_t>((hz + 5000) / 10000);
}

constexpr void WriteDetailedTiming(uint8_t* d, const Timing& t, uint16_t widthMm, uint16_t heightMm)
{
    const uint16_t clock = PixelClock10kHz(t);
    d[0] = static_cast<uint8_t>(clock & 0xFF);
    d[1] = static_cast<uint8_t>(clock >> 8);
    d[2] = static_cast<uint8_t>(t.width & 0xFF);
    d[3] = static_cast<uint8_t>(kHBlank & 0xFF);
    d[4] = static_cast<uint8_t>(((t.width >> 8) & 0x0F) << 4 | ((kHBlank >> 8) & 0x0F));
    d[5] = static_cast<uint8_t>(t.height & 0xFF);
    d[6] = static_cast<uint8_t>(kVBlank & 0xFF);
    d[7] = static_cast<uint8_t>(((t.height >> 8) & 0x0F) << 4 | ((kVBlank >> 8) & 0x0F));
    d[8] = static_cast<uint8_t>(kHFrontPorch & 0xFF);
    d[9] = static_cast<uint8_t>(kHSync & 0xFF);
    d[10] = static_cast<uint8_t>((kVFrontPorch & 0x0F) << 4 | (kVSync & 0x0F));
    d[11] = static_cast<uint8_t>(((kHFrontPorch >> 8) & 0x03) << 6 | ((kHSync >> 8) & 0x03) << 4 |
                                 ((kVFrontPorch >> 4) & 0x03) << 2 | ((kVSync >> 4) & 0x03));
    d[12] = static_cast<uint8_t>(widthMm & 0xFF);
    d[13] = static_cast<uint8_t>(heightMm & 0xFF);
    d[14] = static_cast<uint8_t>(((widthMm >> 8) & 0x0F) << 4 | ((heightMm >> 8) & 0x0F));
    d[15] = 0;
    d[16] = 0;
    d[17] = 0x1E; // digital separate sync, +hsync, +vsync
}

constexpr void WriteTextDescriptor(uint8_t* d, uint8_t tag, const char* text)
{
    d[0] = d[1] = d[2] = 0;
    d[3] = tag;
    d[4] = 0;
    size_t i = 0;
    for (; i < 13 && text[i] != '\0'; ++i)
    {
        d[5 + i] = static_cast<uint8_t>(text[i]);
    }
    if (i < 13)
    {
        d[5 + i++] = 0x0A;
    }
    for (; i < 13; ++i)
    {
        d[5 + i] = 0x20;
    }
}

constexpr std::array<uint8_t, kEdidSize> BuildEdid(const MonitorDescription& m)
{
    std::array<uint8_t, kEdidSize> e{};

    // Header
    e[0] = 0x00;
    for (size_t i = 1; i < 7; ++i)
    {
        e[i] = 0xFF;
    }
    e[7] = 0x00;

    // Manufacturer "DXD" (5-bit letters, A=1), product code, serial.
    constexpr uint16_t kVendor = ((('D' - 'A' + 1) << 10) | (('X' - 'A' + 1) << 5) | ('D' - 'A' + 1));
    e[8] = static_cast<uint8_t>(kVendor >> 8);
    e[9] = static_cast<uint8_t>(kVendor & 0xFF);
    e[10] = 0x01;
    e[11] = 0x00;
    for (size_t i = 0; i < 4; ++i)
    {
        e[12 + i] = static_cast<uint8_t>(m.serial >> (8 * i));
    }
    e[16] = 1;               // week
    e[17] = 2026 - 1990;     // year
    e[18] = 1;               // EDID 1.4
    e[19] = 4;

    e[20] = 0xA5; // digital, 8 bpc, DisplayPort
    e[21] = static_cast<uint8_t>((m.widthMm + 5) / 10);
    e[22] = static_cast<uint8_t>((m.heightMm + 5) / 10);
    e[23] = 120;  // gamma 2.2
    e[24] = 0x06; // RGB 4:4:4, sRGB default color space, preferred timing is native

    // sRGB chromaticity: R(0.640,0.330) G(0.300,0.600) B(0.150,0.060) W(0.3127,0.3290), 10-bit fixed point.
    constexpr uint16_t rx = 655, ry = 338, gx = 307, gy = 614, bx = 154, by = 61, wx = 320, wy = 337;
    e[25] = static_cast<uint8_t>((rx & 3) << 6 | (ry & 3) << 4 | (gx & 3) << 2 | (gy & 3));
    e[26] = static_cast<uint8_t>((bx & 3) << 6 | (by & 3) << 4 | (wx & 3) << 2 | (wy & 3));
    e[27] = static_cast<uint8_t>(rx >> 2);
    e[28] = static_cast<uint8_t>(ry >> 2);
    e[29] = static_cast<uint8_t>(gx >> 2);
    e[30] = static_cast<uint8_t>(gy >> 2);
    e[31] = static_cast<uint8_t>(bx >> 2);
    e[32] = static_cast<uint8_t>(by >> 2);
    e[33] = static_cast<uint8_t>(wx >> 2);
    e[34] = static_cast<uint8_t>(wy >> 2);

    // No established timings; standard timings unused (0x01 0x01).
    for (size_t i = 38; i < 54; ++i)
    {
        e[i] = 0x01;
    }

    WriteDetailedTiming(&e[54], m.preferred, m.widthMm, m.heightMm);
    if (m.secondary.width != 0)
    {
        WriteDetailedTiming(&e[72], m.secondary, m.widthMm, m.heightMm);
    }
    else
    {
        e[75] = 0x10; // dummy descriptor
    }
    WriteTextDescriptor(&e[90], 0xFC, m.name); // monitor name
    e[111] = 0x10;                              // dummy descriptor at 108

    e[126] = 0; // no extension blocks
    uint8_t sum = 0;
    for (size_t i = 0; i < kEdidSize - 1; ++i)
    {
        sum = static_cast<uint8_t>(sum + e[i]);
    }
    e[127] = static_cast<uint8_t>(0x100 - sum);
    return e;
}

// Decodes the detailed timing descriptors of a base block back into modes (refresh rounded to
// whole Hz). Used where IddCx hands the driver only the EDID, not the monitor it belongs to.
inline std::vector<Timing> DetailedTimings(const uint8_t* e, size_t size)
{
    std::vector<Timing> timings;
    if (size < kEdidSize)
    {
        return timings;
    }
    for (size_t offset : {size_t{54}, size_t{72}, size_t{90}, size_t{108}})
    {
        const uint8_t* d = e + offset;
        const unsigned clock10kHz = d[0] | d[1] << 8;
        if (clock10kHz == 0)
        {
            continue; // display descriptor, not a timing
        }
        const unsigned width = d[2] | (d[4] >> 4) << 8;
        const unsigned hblank = d[3] | (d[4] & 0x0F) << 8;
        const unsigned height = d[5] | (d[7] >> 4) << 8;
        const unsigned vblank = d[6] | (d[7] & 0x0F) << 8;
        const uint64_t total = uint64_t{width + hblank} * (height + vblank);
        if (total == 0)
        {
            continue;
        }
        const uint64_t refresh = (uint64_t{clock10kHz} * 10000 + total / 2) / total;
        timings.push_back({static_cast<uint16_t>(width), static_cast<uint16_t>(height), static_cast<uint16_t>(refresh)});
    }
    return timings;
}

} // namespace dd::edid
