#include "VirtualDisplay.h"

#include <cfgmgr32.h>
#include <swdevice.h>

#include <vector>

#include "../capture/OutputLocator.h"
#include "../common/Log.h"

namespace dd
{
namespace
{

struct CreationResult
{
    HANDLE event;
    HRESULT hr;
};

VOID WINAPI OnCreated(HSWDEVICE, HRESULT hr, PVOID context, PCWSTR)
{
    auto* result = static_cast<CreationResult*>(context);
    result->hr = hr;
    SetEvent(result->event);
}

// Creates the software device, or opens it if it already exists (persistent lifetime).
HRESULT OpenSoftwareDevice(HSWDEVICE& device)
{
    SW_DEVICE_CREATE_INFO info{};
    info.cbSize = sizeof(info);
    info.pszInstanceId = driver::kSoftwareDeviceInstanceId;
    info.pszzHardwareIds = driver::kHardwareIds;
    info.pszzCompatibleIds = driver::kHardwareIds;
    info.pszDeviceDescription = L"DeuxDisplay Virtual Monitor";
    info.CapabilityFlags = SWDeviceCapabilitiesSilentInstall | SWDeviceCapabilitiesDriverRequired;

    CreationResult result{CreateEventW(nullptr, TRUE, FALSE, nullptr), E_PENDING};
    if (!result.event)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    HRESULT hr = SwDeviceCreate(driver::kSoftwareDeviceEnumerator, L"HTREE\\ROOT\\0", &info, 0, nullptr, OnCreated,
                                &result, &device);
    if (SUCCEEDED(hr))
    {
        hr = WaitForSingleObject(result.event, 15000) == WAIT_OBJECT_0 ? result.hr : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    // Close the device before the event: the creation callback must not fire into freed state.
    if (FAILED(hr) && device)
    {
        SwDeviceClose(device);
        device = nullptr;
    }
    CloseHandle(result.event);
    return hr;
}

} // namespace

HRESULT InstallDevice()
{
    HSWDEVICE device = nullptr;
    HRESULT hr = OpenSoftwareDevice(device);
    if (FAILED(hr))
    {
        return hr;
    }
    // Keep the device after this process exits (and across reboots) until RemoveDevice().
    hr = SwDeviceSetLifetime(device, SWDeviceLifetimeParentPresent);
    SwDeviceClose(device);
    return hr;
}

HRESULT RemoveDevice()
{
    HSWDEVICE device = nullptr;
    HRESULT hr = OpenSoftwareDevice(device);
    if (FAILED(hr))
    {
        return hr;
    }
    // Back to handle lifetime: closing the handle removes the device.
    hr = SwDeviceSetLifetime(device, SWDeviceLifetimeHandle);
    SwDeviceClose(device);
    return hr;
}

VirtualDisplay::~VirtualDisplay()
{
    Unplug();
    if (m_device != INVALID_HANDLE_VALUE)
    {
        CloseHandle(m_device);
    }
}

HRESULT VirtualDisplay::Open()
{
    if (m_device != INVALID_HANDLE_VALUE)
    {
        return S_OK;
    }

    GUID guid = driver::kDeviceInterfaceGuid;
    ULONG length = 0;
    CONFIGRET cr = CM_Get_Device_Interface_List_SizeW(&length, &guid, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (cr != CR_SUCCESS || length <= 1)
    {
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND); // driver or device not installed
    }
    std::vector<wchar_t> list(length);
    cr = CM_Get_Device_Interface_ListW(&guid, nullptr, list.data(), length, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    if (cr != CR_SUCCESS || list[0] == L'\0')
    {
        return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    }

    // No access rights requested: the IOCTLs are FILE_ANY_ACCESS, so standard users can use them.
    m_device = CreateFileW(list.data(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (m_device == INVALID_HANDLE_VALUE)
    {
        const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        Log(L"display: opening %s failed 0x%08lX", list.data(), Hr(hr));
        return hr;
    }
    return S_OK;
}

HRESULT VirtualDisplay::Plug(const driver::PlugRequest& request)
{
    if (!driver::IsValidPlugRequest(request))
    {
        return E_INVALIDARG;
    }
    HRESULT hr = Open();
    if (FAILED(hr))
    {
        return hr;
    }
    DWORD returned = 0;
    driver::PlugRequest copy = request;
    if (!DeviceIoControl(m_device, driver::kIoctlPlug, &copy, sizeof(copy), nullptr, 0, &returned, nullptr))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        Log(L"display: PLUG ioctl failed 0x%08lX", Hr(hr));
        return hr;
    }
    m_plugged = true;
    return S_OK;
}

void VirtualDisplay::Unplug()
{
    if (m_plugged && m_device != INVALID_HANDLE_VALUE)
    {
        DWORD returned = 0;
        DeviceIoControl(m_device, driver::kIoctlUnplug, nullptr, 0, nullptr, 0, &returned, nullptr);
    }
    m_plugged = false;
}

bool WaitForExtendedDisplay(DWORD timeoutMs)
{
    bool switchedTopology = false;
    capture::LocatedOutput output;
    for (DWORD waited = 0; waited <= timeoutMs; waited += 100)
    {
        if (capture::FindVirtualOutput(output))
        {
            Log(L"display: virtual monitor is %s", output.deviceName.c_str());
            return true;
        }
        if (!switchedTopology && capture::IsVirtualMonitorMirrored())
        {
            Log(L"display: monitor attached in duplicate mode, switching to extend");
            const LONG result = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND);
            if (result != ERROR_SUCCESS)
            {
                Log(L"display: SetDisplayConfig(extend) failed %ld", result);
            }
            switchedTopology = true;
        }
        Sleep(100);
    }
    return false;
}

driver::PlugRequest DefaultPlugRequest()
{
    driver::PlugRequest r;
    r.width = 2408;
    r.height = 1720;
    r.widthMm = 235;
    r.heightMm = 168;
    r.refreshHz[0] = 60;
    r.refreshHz[1] = 90;
    return r;
}

} // namespace dd
