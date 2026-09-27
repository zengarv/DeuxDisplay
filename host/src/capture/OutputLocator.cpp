#include "OutputLocator.h"

#include <cstdio>

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

std::wstring MonitorHardwareId(unsigned index)
{
    wchar_t id[16] = {};
    swprintf_s(id, L"DXD%04X", index + 1);
    return id;
}

bool IsVirtualMonitorMirrored(unsigned index)
{
    // In duplicate mode our monitor shares a source (e.g. \\.\DISPLAY1) with another monitor.
    const std::wstring hardwareId = MonitorHardwareId(index);
    DISPLAY_DEVICEW adapter{};
    adapter.cb = sizeof(adapter);
    for (DWORD a = 0; EnumDisplayDevicesW(nullptr, a, &adapter, 0); ++a)
    {
        int active = 0;
        bool ours = false;
        DISPLAY_DEVICEW monitor{};
        monitor.cb = sizeof(monitor);
        for (DWORD m = 0; EnumDisplayDevicesW(adapter.DeviceName, m, &monitor, 0); ++m)
        {
            if (monitor.StateFlags & DISPLAY_DEVICE_ACTIVE)
            {
                ++active;
                ours = ours || std::wstring(monitor.DeviceID).find(hardwareId) != std::wstring::npos;
            }
        }
        if (ours && active > 1)
        {
            return true;
        }
    }
    return false;
}

bool FindVirtualOutput(unsigned index, LocatedOutput& result)
{
    // Only an output driven by our monitor alone counts; a mirrored source is someone else's image.
    const std::wstring hardwareId = MonitorHardwareId(index);
    return FindOutput(
        [&](const DXGI_OUTPUT_DESC& desc) {
            int active = 0;
            bool ours = false;
            DISPLAY_DEVICEW monitor{};
            monitor.cb = sizeof(monitor);
            for (DWORD m = 0; EnumDisplayDevicesW(desc.DeviceName, m, &monitor, 0); ++m)
            {
                if (monitor.StateFlags & DISPLAY_DEVICE_ACTIVE)
                {
                    ++active;
                    ours = ours || std::wstring(monitor.DeviceID).find(hardwareId) != std::wstring::npos;
                }
            }
            return ours && active == 1;
        },
        result);
}

bool FindOutputByName(const std::wstring& deviceName, LocatedOutput& result)
{
    return FindOutput([&](const DXGI_OUTPUT_DESC& desc) { return _wcsicmp(desc.DeviceName, deviceName.c_str()) == 0; },
                      result);
}

} // namespace dd::capture
