#include "Check.h"

#include "../src/render/Rotation.h"

#include <initializer_list>

using namespace dd::render;

namespace
{

bool Near(PointF a, PointF b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy < 1e-6f;
}

// Rotating the scan-out frame clockwise by `rotation` must undo DesktopToScanout: map a desktop
// point to scan-out, then rotate that clockwise and land back on the same desktop point.
PointF RotateScanoutClockwise(PointF s, float width, float height, uint16_t rotation)
{
    switch (rotation)
    {
    case 90:
        return {height - s.y, s.x};
    case 180:
        return {width - s.x, height - s.y};
    case 270:
        return {s.y, width - s.x};
    default:
        return s;
    }
}

void DxgiMapping()
{
    CHECK(ClientRotationFor(1) == 0);
    CHECK(ClientRotationFor(2) == 90);
    CHECK(ClientRotationFor(3) == 180);
    CHECK(ClientRotationFor(4) == 270);
    CHECK(ClientRotationFor(0) == 0); // unspecified
}

void CornersAndRoundTrips()
{
    const float w = 2408; // scan-out (native landscape)
    const float h = 1720;

    // Portrait desktop is 1720 x 2408; its corners land on scan-out corners.
    CHECK(Near(DesktopToScanout({0, 0}, w, h, 90), {0, h}));
    CHECK(Near(DesktopToScanout({h, 0}, w, h, 90), {0, 0}));
    CHECK(Near(DesktopToScanout({0, w}, w, h, 90), {w, h}));
    CHECK(Near(DesktopToScanout({0, 0}, w, h, 270), {w, 0}));
    CHECK(Near(DesktopToScanout({0, 0}, w, h, 180), {w, h}));
    CHECK(Near(DesktopToScanout({10, 20}, w, h, 0), {10, 20}));

    for (uint16_t r : {uint16_t{0}, uint16_t{90}, uint16_t{180}, uint16_t{270}})
    {
        const PointF p{123, 456};
        CHECK(Near(RotateScanoutClockwise(DesktopToScanout(p, w, h, r), w, h, r), p));
    }
}

} // namespace

void RunRotationTests()
{
    DxgiMapping();
    CornersAndRoundTrips();
}
