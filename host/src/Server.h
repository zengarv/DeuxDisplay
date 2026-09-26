#pragma once

#include <cstdint>
#include <string>

namespace dd
{

struct ServeOptions
{
    uint16_t port = 27183;
    unsigned bitrateKbps = 30000;
    bool createDisplay = true; // false: use an already-attached DeuxDisplay monitor
    std::wstring outputName;   // debug: stream this existing output (e.g. \\.\DISPLAY1) instead
};

// Runs until the process is terminated: waits for a client, streams the virtual display,
// and goes back to waiting when the client disconnects.
int Serve(const ServeOptions& options);

} // namespace dd
