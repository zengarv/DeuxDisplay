#include "Check.h"

#include "../src/encode/Vp9Header.h"

#include <vector>

namespace
{

// Profile 0 keyframe, BT.709 (color_space 2), studio range: 10 0 0 0 0 1 0 | sync | 010 0 ...
std::vector<uint8_t> Keyframe()
{
    // bits: 10 00 0 0 1 0 | 0100 1001 1000 0011 0100 0010 | 010 0 (color_range) ...
    return {0b1000'0010, 0b0100'1001, 0b1000'0011, 0b0100'0010, 0b0100'0000, 0xAB};
}

void SetsTheRangeBitOfAKeyframe()
{
    auto frame = Keyframe();
    CHECK(dd::vp9::MarkFullRange(frame));
    CHECK(frame[4] == 0b0101'0000); // color_range set, color_space untouched
    CHECK(frame[5] == 0xAB);        // nothing else changes
}

void LeavesOtherFramesAlone()
{
    auto inter = Keyframe();
    inter[0] = 0b1000'0110; // frame_type 1: inter frame
    const auto before = inter;
    CHECK(!dd::vp9::MarkFullRange(inter) && inter == before);

    auto rgb = Keyframe();
    rgb[4] = 0b1110'0000; // color_space 7 (RGB) has no range bit
    CHECK(!dd::vp9::MarkFullRange(rgb));

    auto badSync = Keyframe();
    badSync[1] = 0;
    CHECK(!dd::vp9::MarkFullRange(badSync));

    std::vector<uint8_t> tiny{0b1000'0010};
    CHECK(!dd::vp9::MarkFullRange(tiny));
}

} // namespace

void RunVp9HeaderTests()
{
    SetsTheRangeBitOfAKeyframe();
    LeavesOtherFramesAlone();
}
