#include "PointerShape.h"

namespace dd::capture
{
namespace
{

void Put(std::vector<uint8_t>& out, size_t pixel, uint8_t b, uint8_t g, uint8_t r, uint8_t a)
{
    out[pixel * 4 + 0] = b;
    out[pixel * 4 + 1] = g;
    out[pixel * 4 + 2] = r;
    out[pixel * 4 + 3] = a;
}

} // namespace

PointerImage ConvertPointerShape(PointerType type, uint32_t width, uint32_t height, uint32_t pitch,
                                 std::span<const uint8_t> data)
{
    PointerImage img;
    // Monochrome shapes stack the AND mask on top of the XOR mask, so the real height is half.
    const uint32_t realHeight = type == PointerType::Monochrome ? height / 2 : height;
    const size_t needed = static_cast<size_t>(pitch) * height;
    if (width == 0 || realHeight == 0 || data.size() < needed)
    {
        return img;
    }

    img.width = width;
    img.height = realHeight;
    img.bgra.assign(static_cast<size_t>(width) * realHeight * 4, 0);

    for (uint32_t y = 0; y < realHeight; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            const size_t pixel = static_cast<size_t>(y) * width + x;
            if (type == PointerType::Monochrome)
            {
                const uint8_t bit = static_cast<uint8_t>(0x80 >> (x % 8));
                const bool andBit = (data[y * pitch + x / 8] & bit) != 0;
                const bool xorBit = (data[(y + realHeight) * pitch + x / 8] & bit) != 0;
                if (!andBit)
                {
                    const uint8_t v = xorBit ? 0xFF : 0x00;
                    Put(img.bgra, pixel, v, v, v, 0xFF);
                }
                else if (xorBit)
                {
                    Put(img.bgra, pixel, 0, 0, 0, 0xFF); // invert, approximated
                }
                // AND=1, XOR=0: transparent (already zero)
            }
            else
            {
                const uint8_t* p = &data[y * pitch + x * 4];
                if (type == PointerType::Color)
                {
                    Put(img.bgra, pixel, p[0], p[1], p[2], p[3]);
                }
                else if (p[3] == 0)
                {
                    Put(img.bgra, pixel, p[0], p[1], p[2], 0xFF); // replace screen pixel
                }
                else if (p[0] | p[1] | p[2])
                {
                    Put(img.bgra, pixel, 0, 0, 0, 0xFF); // XOR with non-zero color, approximated
                }
            }
        }
    }
    return img;
}

} // namespace dd::capture
