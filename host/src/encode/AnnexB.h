#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dd::annexb
{

struct Nal
{
    size_t offset = 0; // start of the start code
    size_t size = 0;   // start code + NAL payload
    uint8_t type = 0;  // H.264 nal_unit_type
};

inline constexpr uint8_t kNalSlice = 1;
inline constexpr uint8_t kNalIdr = 5;
inline constexpr uint8_t kNalSps = 7;
inline constexpr uint8_t kNalPps = 8;

// Splits an H.264 Annex-B buffer into NAL units (3- or 4-byte start codes).
std::vector<Nal> SplitH264(std::span<const uint8_t> data);

struct SplitAccessUnit
{
    std::vector<uint8_t> codecConfig; // SPS + PPS NALs, Annex-B
    std::vector<uint8_t> frame;       // everything else
    bool keyframe = false;
};

// Separates parameter sets from picture data so they can be sent as CODEC_CONFIG.
SplitAccessUnit SeparateParameterSets(std::span<const uint8_t> accessUnit);

} // namespace dd::annexb
