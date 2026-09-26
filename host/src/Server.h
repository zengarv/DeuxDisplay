#pragma once

#include <cstdint>
#include <string>

namespace dd
{

struct ServeOptions
{
    uint16_t port = 27183;
    unsigned bitrateKbps = 30000;
    // Never send faster than this. Frames the client can't decode in time queue up in its decoder
    // and add latency (the OnePlus Pad Go's decoder tops out around 60 fps at 2408x1720).
    unsigned maxFps = 60;
    // Largest video size to encode (0 = native). The desktop is scaled down to fit, keeping the
    // aspect ratio; the client scales it back up to its panel.
    unsigned maxStreamWidth = 0;
    unsigned maxStreamHeight = 0;
    // "auto" prefers HEVC when the client can decode it (faster decode and sharper text on
    // most tablets), falling back to H.264 if the PC can't encode it.
    std::wstring codec = L"auto";
    // After each frame, also send a re-encode of the same image so "one frame behind" decoders
    // (MediaTek) output the real frame immediately instead of waiting for the next screen change.
    // Measured on the OnePlus Pad Go: no gain (its decoder holds ~2 frames regardless), so off.
    bool repeatFrames = false;
    // Inject the client's touches on the streamed display (Windows touch injection).
    bool touchInput = true;
    std::wstring outputName; // debug: stream this existing output (e.g. \\.\DISPLAY1) instead
};

// Runs until the process is terminated. For each client: plugs a virtual monitor matching the
// client's HELLO, streams it, and unplugs it when the client disconnects.
int Serve(const ServeOptions& options);

} // namespace dd
