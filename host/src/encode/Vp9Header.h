#pragma once

// VP9 uncompressed frame header edits. Header-only so the unit tests can cover it.

#include <cstddef>
#include <cstdint>
#include <span>

namespace dd::vp9
{

// Marks a VP9 keyframe as full range (color_range = 1) in place. Hardware encoders may ignore the
// range they were configured with (Intel's VP9 MFT always writes studio range), but the bit sits
// in the uncompressed header, outside the entropy-coded data, so flipping it is safe. Returns
// false (and changes nothing) for anything but a keyframe with a YUV color space.
inline bool MarkFullRange(std::span<uint8_t> frame)
{
    size_t bit = 0;
    const auto read = [&](unsigned count) -> int {
        unsigned value = 0;
        for (unsigned i = 0; i < count; ++i, ++bit)
        {
            if (bit / 8 >= frame.size())
            {
                return -1;
            }
            value = value << 1 | ((frame[bit / 8] >> (7 - bit % 8)) & 1);
        }
        return static_cast<int>(value);
    };
    if (read(2) != 2) // frame_marker
    {
        return false;
    }
    const int profileLow = read(1);
    const int profile = profileLow | read(1) << 1;
    if (profile < 0 || (profile == 3 && read(1) != 0) || read(1) != 0) // reserved_zero, show_existing_frame
    {
        return false;
    }
    if (read(1) != 0) // frame_type: 0 = keyframe
    {
        return false;
    }
    read(2);                        // show_frame, error_resilient_mode
    if (read(24) != 0x498342)       // frame sync code
    {
        return false;
    }
    if (profile >= 2 && read(1) < 0) // ten_or_twelve_bit
    {
        return false;
    }
    const int colorSpace = read(3);
    if (colorSpace < 0 || colorSpace == 7 || bit / 8 >= frame.size()) // 7 = RGB: no range bit
    {
        return false;
    }
    frame[bit / 8] |= static_cast<uint8_t>(0x80 >> (bit % 8));
    return true;
}

} // namespace dd::vp9
