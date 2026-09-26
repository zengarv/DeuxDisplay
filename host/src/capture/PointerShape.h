#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace dd::capture
{

// Mirrors DXGI_OUTDUPL_POINTER_SHAPE_TYPE without pulling in DXGI headers (keeps tests portable).
enum class PointerType : uint32_t
{
    Monochrome = 1,
    Color = 2,
    MaskedColor = 4,
};

struct PointerImage
{
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> bgra; // straight alpha, tightly packed
};

// Converts a Desktop Duplication pointer shape to BGRA with alpha, ready for alpha blending.
// XOR ("invert screen") pixels can't be expressed with plain blending; they are drawn as
// opaque black, which reads correctly on the light backgrounds where they're most common.
PointerImage ConvertPointerShape(PointerType type, uint32_t width, uint32_t height, uint32_t pitch,
                                 std::span<const uint8_t> data);

} // namespace dd::capture
