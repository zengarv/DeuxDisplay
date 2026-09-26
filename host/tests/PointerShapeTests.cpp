#include "Check.h"

#include "../src/capture/PointerShape.h"

using namespace dd::capture;

namespace
{

void Monochrome()
{
    // 8x1 cursor, pitch 1: AND row then XOR row.
    // x0: AND0 XOR0 -> black, x1: AND0 XOR1 -> white, x2: AND1 XOR0 -> transparent, x3: AND1 XOR1 -> invert
    const std::vector<uint8_t> data = {0b0011'1111, 0b0101'0000};
    const auto img = ConvertPointerShape(PointerType::Monochrome, 8, 2, 1, data);
    CHECK(img.width == 8 && img.height == 1);
    CHECK(img.bgra[0] == 0x00 && img.bgra[3] == 0xFF);              // black opaque
    CHECK(img.bgra[4] == 0xFF && img.bgra[7] == 0xFF);              // white opaque
    CHECK(img.bgra[11] == 0x00);                                    // transparent
    CHECK(img.bgra[12] == 0x00 && img.bgra[15] == 0xFF);            // invert -> black
}

void Color()
{
    const std::vector<uint8_t> data = {1, 2, 3, 128, 4, 5, 6, 0};
    const auto img = ConvertPointerShape(PointerType::Color, 2, 1, 8, data);
    CHECK(img.bgra == data);
}

void MaskedColor()
{
    const std::vector<uint8_t> data = {10, 20, 30, 0x00, 0, 0, 0, 0xFF, 9, 9, 9, 0xFF};
    const auto img = ConvertPointerShape(PointerType::MaskedColor, 3, 1, 12, data);
    CHECK(img.bgra[0] == 10 && img.bgra[3] == 0xFF); // replaced, opaque
    CHECK(img.bgra[7] == 0x00);                      // XOR with black: no-op, transparent
    CHECK(img.bgra[8] == 0 && img.bgra[11] == 0xFF); // XOR approximated
}

void RejectsShortBuffer()
{
    const std::vector<uint8_t> data(3);
    CHECK(ConvertPointerShape(PointerType::Color, 2, 1, 8, data).width == 0);
}

} // namespace

void RunPointerShapeTests()
{
    Monochrome();
    Color();
    MaskedColor();
    RejectsShortBuffer();
}
