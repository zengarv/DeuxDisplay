#pragma once

// The adb server's "smart socket" protocol (127.0.0.1:5037), the part the host needs to keep
// `adb reverse` in place. See platform/packages/modules/adb/SERVICES.TXT and protocol.txt.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace dd::adb
{

inline constexpr unsigned short kServerPort = 5037;

// A request is its length as 4 hex digits, then the payload: "000chost:version".
std::string EncodeRequest(std::string_view payload);

// Parses 4 hex digits (either case). nullopt if malformed.
std::optional<size_t> ParseHexLength(std::string_view digits);

struct Device
{
    std::string serial;
    std::string state; // "device", "unauthorized", "offline", "recovery", ...
};

// A host:track-devices / host:devices payload: one "serial\tstate" per line.
std::vector<Device> ParseDeviceList(std::string_view payload);

// "reverse:forward:tcp:27183;tcp:27183": the tablet's 127.0.0.1:port reaches the PC's port.
std::string ReverseRequest(unsigned short port);

} // namespace dd::adb
