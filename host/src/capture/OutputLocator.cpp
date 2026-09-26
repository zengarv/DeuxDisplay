#include "OutputLocator.h"

using Microsoft::WRL::ComPtr;

namespace dd::capture
{

std::wstring MonitorDeviceId(const wchar_t* outputDeviceName)
{
    DISPLAY_DEVICEW monitor{};
    monitor.cb = sizeof(monitor);
    for (DWORD i = 0; EnumDisplayDevicesW(outputDeviceName, i, &monitor, 0); ++i)
    {
        if (monitor.StateFlags & DISPLAY_DEVICE_ACTIVE)
        {
            return monitor.DeviceID;
        }
    }
    return {};
}

namespace
{

template <typename Predicate> bool FindOutput(Predicate matches, LocatedOutput& result)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        return false;
    }

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a)
    {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o)
        {
            DXGI_OUTPUT_DESC desc{};
            output->GetDesc(&desc);
            if (desc.AttachedToDesktop && matches(desc))
            {
                result.adapter = adapter;
                result.deviceName = desc.DeviceName;
                return SUCCEEDED(output.As(&result.output));
            }
            output.Reset();
        }
        adapter.Reset();
    }
    return false;
}

} // namespace

bool FindVirtualOutput(LocatedOutput& result)
{
    return FindOutput(
        [](const DXGI_OUTPUT_DESC& desc) {
            return MonitorDeviceId(desc.DeviceName).find(kMonitorHardwareId) != std::wstring::npos;
        },
        result);
}

bool FindOutputByName(const std::wstring& deviceName, LocatedOutput& result)
{
    return FindOutput([&](const DXGI_OUTPUT_DESC& desc) { return _wcsicmp(desc.DeviceName, deviceName.c_str()) == 0; },
                      result);
}

} // namespace dd::capture
