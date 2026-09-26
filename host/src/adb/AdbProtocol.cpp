#include "AdbProtocol.h"

#include <cstdio>

namespace dd::adb
{

std::string EncodeRequest(std::string_view payload)
{
    char length[5] = {};
    std::snprintf(length, sizeof(length), "%04zx", payload.size() & 0xFFFF);
    return std::string(length, 4) + std::string(payload);
}

std::optional<size_t> ParseHexLength(std::string_view digits)
{
    if (digits.size() != 4)
    {
        return std::nullopt;
    }
    size_t value = 0;
    for (char c : digits)
    {
        value <<= 4;
        if (c >= '0' && c <= '9')
        {
            value |= static_cast<size_t>(c - '0');
        }
        else if (c >= 'a' && c <= 'f')
        {
            value |= static_cast<size_t>(c - 'a' + 10);
        }
        else if (c >= 'A' && c <= 'F')
        {
            value |= static_cast<size_t>(c - 'A' + 10);
        }
        else
        {
            return std::nullopt;
        }
    }
    return value;
}

std::vector<Device> ParseDeviceList(std::string_view payload)
{
    std::vector<Device> devices;
    while (!payload.empty())
    {
        const size_t end = payload.find('\n');
        std::string_view line = payload.substr(0, end);
        payload = end == std::string_view::npos ? std::string_view{} : payload.substr(end + 1);
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }
        const size_t tab = line.find('\t');
        if (tab == std::string_view::npos || tab == 0)
        {
            continue;
        }
        devices.push_back({std::string(line.substr(0, tab)), std::string(line.substr(tab + 1))});
    }
    return devices;
}

std::string ReverseRequest(unsigned short port)
{
    const std::string spec = "tcp:" + std::to_string(port);
    return "reverse:forward:" + spec + ";" + spec;
}

} // namespace dd::adb
