#pragma once

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <string>

namespace dd::capture
{

struct LocatedOutput
{
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    Microsoft::WRL::ComPtr<IDXGIOutput1> output;
    std::wstring deviceName; // e.g. \\.\DISPLAY3
};

// Each virtual monitor's EDID uses vendor "DXD" and product code index + 1, so Windows gives the
// monitor on driver connector `index` the hardware ID MONITOR\DXD0001, DXD0002, ... Match on
// that rather than on output order.
std::wstring MonitorHardwareId(unsigned index);

// Returns false if virtual monitor `index` is not attached as its own (extended) output.
bool FindVirtualOutput(unsigned index, LocatedOutput& result);

// True if virtual monitor `index` is attached but mirroring another display (duplicate mode).
bool IsVirtualMonitorMirrored(unsigned index);

// Finds an output by GDI device name (e.g. \\.\DISPLAY1). For testing the pipeline on a
// physical monitor without the driver.
bool FindOutputByName(const std::wstring& deviceName, LocatedOutput& result);

// For diagnostics: the monitor device ID of an output, e.g. MONITOR\DXD0001\{...}\0003.
std::wstring MonitorDeviceId(const wchar_t* outputDeviceName);

} // namespace dd::capture
