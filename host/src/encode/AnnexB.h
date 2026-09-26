#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace dd::annexb
{

enum class Codec
{
    H264,
    Hevc,
};

struct Nal
{
    size_t offset = 0; // start of the start code
    size_t size = 0;   // start code + NAL payload
    uint8_t type = 0;  // nal_unit_type (H.264: 5 bits, HEVC: 6 bits)
};

inline constexpr uint8_t kNalSlice = 1;
inline constexpr uint8_t kNalIdr = 5;
inline constexpr uint8_t kNalSps = 7;
inline constexpr uint8_t kNalPps = 8;

inline constexpr uint8_t kHevcNalVps = 32;
inline constexpr uint8_t kHevcNalSps = 33;
inline constexpr uint8_t kHevcNalPps = 34;

// Splits an Annex-B buffer into NAL units (3- or 4-byte start codes).
std::vector<Nal> Split(std::span<const uint8_t> data, Codec codec = Codec::H264);

inline std::vector<Nal> SplitH264(std::span<const uint8_t> data)
{
    return Split(data, Codec::H264);
}

struct SplitAccessUnit
{
    std::vector<uint8_t> codecConfig; // parameter sets (H.264 SPS+PPS, HEVC VPS+SPS+PPS), Annex-B
    std::vector<uint8_t> frame;       // everything else
    bool keyframe = false;
};

// Separates parameter sets from picture data so they can be sent as CODEC_CONFIG.
SplitAccessUnit SeparateParameterSets(std::span<const uint8_t> accessUnit, Codec codec = Codec::H264);

} // namespace dd::annexb
