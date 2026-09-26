#include "Check.h"

#include "../src/adb/AdbProtocol.h"

using namespace dd::adb;

namespace
{

void Requests()
{
    CHECK(EncodeRequest("host:track-devices") == "0012host:track-devices");
    CHECK(EncodeRequest("") == "0000");
    CHECK(ReverseRequest(27183) == "reverse:forward:tcp:27183;tcp:27183");
    CHECK(EncodeRequest(ReverseRequest(27183)).substr(0, 4) == "0023");
}

void HexLengths()
{
    CHECK(ParseHexLength("0000") == 0u);
    CHECK(ParseHexLength("001a") == 26u);
    CHECK(ParseHexLength("FFFF") == 65535u);
    CHECK(!ParseHexLength("00g0").has_value());
    CHECK(!ParseHexLength("123").has_value());
}

void DeviceLists()
{
    auto devices = ParseDeviceList("0123456789ABCDEF\tdevice\nemulator-5554\tunauthorized\n");
    CHECK(devices.size() == 2);
    CHECK(devices[0].serial == "0123456789ABCDEF" && devices[0].state == "device");
    CHECK(devices[1].serial == "emulator-5554" && devices[1].state == "unauthorized");

    CHECK(ParseDeviceList("").empty()); // nothing attached
    auto crlf = ParseDeviceList("abc\toffline\r\n\ngarbage\n");
    CHECK(crlf.size() == 1 && crlf[0].serial == "abc" && crlf[0].state == "offline");
}

} // namespace

void RunAdbTests()
{
    Requests();
    HexLengths();
    DeviceLists();
}
