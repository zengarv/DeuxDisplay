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

// The virtual monitor's EDID uses vendor "DXD", product 0x0001, so Windows gives it the
// monitor hardware ID MONITOR\DXD0001. Match on that rather than on output order.
inline constexpr wchar_t kMonitorHardwareId[] = L"DXD0001";

// Returns false if the virtual monitor is not currently attached.
bool FindVirtualOutput(LocatedOutput& result);

// For diagnostics: the monitor device ID of an output, e.g. MONITOR\DXD0001\{...}\0003.
std::wstring MonitorDeviceId(const wchar_t* outputDeviceName);

} // namespace dd::capture
