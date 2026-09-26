#include "Check.h"

#include "../src/encode/AnnexB.h"

using namespace dd::annexb;

namespace
{

// SPS (4-byte start code), PPS (3-byte), IDR slice (4-byte).
const std::vector<uint8_t> kKeyframeAu = {
    0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xC0, 0x1F, // SPS
    0x00, 0x00, 0x01, 0x68, 0xCE, 0x3C, 0x80,       // PPS
    0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x00, // IDR
};

void SplitsMixedStartCodes()
{
    const auto nals = SplitH264(kKeyframeAu);
    CHECK(nals.size() == 3);
    if (nals.size() != 3)
    {
        return;
    }
    CHECK(nals[0].offset == 0 && nals[0].size == 8 && nals[0].type == kNalSps);
    CHECK(nals[1].offset == 8 && nals[1].size == 7 && nals[1].type == kNalPps);
    CHECK(nals[2].offset == 15 && nals[2].size == 8 && nals[2].type == kNalIdr);
}

void SeparatesParameterSets()
{
    const auto split = SeparateParameterSets(kKeyframeAu);
    CHECK(split.keyframe);
    CHECK(split.codecConfig.size() == 15);
    CHECK(split.frame.size() == 8);
    CHECK(split.frame[4] == 0x65);
}

void NonKeyframe()
{
    const std::vector<uint8_t> au = {0x00, 0x00, 0x00, 0x01, 0x41, 0x9A, 0x02};
    const auto split = SeparateParameterSets(au);
    CHECK(!split.keyframe);
    CHECK(split.codecConfig.empty());
    CHECK(split.frame == au);
}

void EmptyAndGarbage()
{
    CHECK(SplitH264({}).empty());
    const std::vector<uint8_t> noStartCode = {0x12, 0x34, 0x56};
    CHECK(SplitH264(noStartCode).empty());
}

} // namespace

void RunAnnexBTests()
{
    SplitsMixedStartCodes();
    SeparatesParameterSets();
    NonKeyframe();
    EmptyAndGarbage();
}
