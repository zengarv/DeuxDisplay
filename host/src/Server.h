#pragma once

#include <cstdint>
#include <string>

namespace dd
{

struct ServeOptions
{
    uint16_t port = 27183;
    unsigned bitrateKbps = 30000;
    std::wstring outputName; // debug: stream this existing output (e.g. \\.\DISPLAY1) instead
};

// Runs until the process is terminated. For each client: plugs a virtual monitor matching the
// client's HELLO, streams it, and unplugs it when the client disconnects.
int Serve(const ServeOptions& options);

} // namespace dd
