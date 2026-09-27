#pragma once

#include <cstdint>

namespace dd::render
{

// When Windows rotates a display, Desktop Duplication still delivers frames in the display's
// native scan-out orientation (landscape for DeuxDisplay), with the desktop drawn rotated inside.
// DeuxDisplay encodes those frames unchanged and tells the client to rotate them for display
// (CONFIG rotation), which its compositor does for free.
//
// `clientRotation` is that angle: degrees clockwise that turn a scan-out frame into the upright
// desktop. The pointer arrives in desktop coordinates, so drawing it into the scan-out frame
// needs the inverse mapping below.

// DXGI_MODE_ROTATION (1 = identity, 2 = 90, 3 = 180, 4 = 270) -> clockwise degrees the client
// applies. Verified on hardware; flip 90/270 here if a device reports the opposite convention.
constexpr uint16_t ClientRotationFor(int dxgiModeRotation)
{
    switch (dxgiModeRotation)
    {
    case 2:
        return 90;
    case 3:
        return 180;
    case 4:
        return 270;
    default:
        return 0;
    }
}

struct PointF
{
    float x = 0;
    float y = 0;
};

// Maps a desktop point to the scan-out frame (width x height) for a given clientRotation.
// Derived from the client's clockwise rotation of the scan-out frame (u, v):
//   90: desktop = (H - v, u)   180: (W - u, H - v)   270: (v, W - u)
constexpr PointF DesktopToScanout(PointF p, float width, float height, uint16_t clientRotation)
{
    switch (clientRotation)
    {
    case 90:
        return {p.y, height - p.x};
    case 180:
        return {width - p.x, height - p.y};
    case 270:
        return {width - p.y, p.x};
    default:
        return p;
    }
}

} // namespace dd::render
