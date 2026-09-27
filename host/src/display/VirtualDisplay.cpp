#include "VirtualDisplay.h"

#include <cfgmgr32.h>
#include <devguid.h>
#include <newdev.h>
#include <setupapi.h>

#include <string>
#include <vector>

#include "../capture/OutputLocator.h"
#include "../common/Log.h"

namespace dd
{
namespace
{

bool EqualsIgnoreCase(const wchar_t* a, const wchar_t* b)
{
    return CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL;
}

// Whether a display-class device carries one of our hardware IDs (either kind of device).
bool IsOurDevice(HDEVINFO set, SP_DEVINFO_DATA& data)
{
    wchar_t ids[512]{}; // REG_MULTI_SZ; the size limit keeps the final double NUL in place
    if (!SetupDiGetDeviceRegistryPropertyW(set, &data, SPDRP_HARDWAREID, nullptr, reinterpret_cast<BYTE*>(ids),
                                           sizeof(ids) - 2 * sizeof(wchar_t), nullptr))
    {
        return false;
    }
    for (const wchar_t* id = ids; *id; id += wcslen(id) + 1)
    {
        if (EqualsIgnoreCase(id, driver::kRootHardwareIds) || EqualsIgnoreCase(id, driver::kSoftwareHardwareId))
        {
            return true;
        }
    }
    return false;
}

// Removes our devices, present or not (a software device left over from a reboot is only a
// registry entry). With `keepRoot`, root-enumerated ones stay and are reported in `haveRoot`.
HRESULT RemoveDevices(bool keepRoot, bool& haveRoot)
{
    haveRoot = false;
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, 0); // 0: not only present ones
    if (set == INVALID_HANDLE_VALUE)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    // Collect first: removing while enumerating would shift the indices.
    std::vector<std::wstring> doomed;
    SP_DEVINFO_DATA data{sizeof(data)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &data); ++i)
    {
        wchar_t instanceId[MAX_DEVICE_ID_LEN]{};
        if (!IsOurDevice(set, data) ||
            !SetupDiGetDeviceInstanceIdW(set, &data, instanceId, MAX_DEVICE_ID_LEN, nullptr))
        {
            continue;
        }
        if (keepRoot && _wcsnicmp(instanceId, L"ROOT\\", 5) == 0)
        {
            haveRoot = true;
            continue;
        }
        doomed.emplace_back(instanceId);
    }
    HRESULT hr = S_OK;
    for (const auto& instanceId : doomed)
    {
        SP_DEVINFO_DATA device{sizeof(device)};
        if (!SetupDiOpenDeviceInfoW(set, instanceId.c_str(), nullptr, 0, &device) ||
            !SetupDiCallClassInstaller(DIF_REMOVE, set, &device))
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
            Log(L"display: removing %s failed 0x%08lX", instanceId.c_str(), Hr(hr));
            continue;
        }
        Log(L"display: removed %s", instanceId.c_str());
    }
    SetupDiDestroyDeviceInfoList(set);
    return hr;
}

// Creates ROOT\DEUXDISPLAYIDD\nnnn and installs the driver package already in the driver store.
HRESULT CreateRootDevice()
{
    HDEVINFO set = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_DISPLAY, nullptr);
    if (set == INVALID_HANDLE_VALUE)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    SP_DEVINFO_DATA data{sizeof(data)};
    HRESULT hr = S_OK;
    if (!SetupDiCreateDeviceInfoW(set, L"DeuxDisplayIdd", &GUID_DEVCLASS_DISPLAY, L"DeuxDisplay Virtual Monitor",
                                  nullptr, DICD_GENERATE_ID, &data) ||
        !SetupDiSetDeviceRegistryPropertyW(set, &data, SPDRP_HARDWAREID,
                                           reinterpret_cast<const BYTE*>(driver::kRootHardwareIds),
                                           sizeof(driver::kRootHardwareIds)) ||
        !SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &data))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
    }
    else
    {
        // No driver given: the best match from the driver store (pnputil /add-driver put it there).
        BOOL reboot = FALSE;
        if (!DiInstallDevice(nullptr, set, &data, nullptr, 0, &reboot))
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
            SetupDiCallClassInstaller(DIF_REMOVE, set, &data); // no half-installed device left behind
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return hr;
}

} // namespace

HRESULT InstallDevice()
{
    // Drops the software device earlier versions created: it didn't come back after a reboot.
    bool haveRoot = false;
    HRESULT hr = RemoveDevices(true, haveRoot);
    if (FAILED(hr) || haveRoot)
    {
        return hr; // already installed; Windows restores the root device at every boot
    }
    return CreateRootDevice();
}

HRESULT RemoveDevice()
{
    bool haveRoot = false;
    return RemoveDevices(false, haveRoot);
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
    driver::PlugResult result;
    if (!DeviceIoControl(m_device, driver::kIoctlPlug, &copy, sizeof(copy), &result, sizeof(result), &returned,
                         nullptr))
    {
        hr = HRESULT_FROM_WIN32(GetLastError());
        Log(L"display: PLUG ioctl failed 0x%08lX", Hr(hr));
        return hr;
    }
    // A driver from before multi-monitor support returns nothing and has only connector 0.
    m_index = returned >= sizeof(result) && result.index < driver::kMaxMonitors ? result.index : 0;
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

bool WaitForExtendedDisplay(unsigned index, DWORD timeoutMs)
{
    bool switchedTopology = false;
    capture::LocatedOutput output;
    for (DWORD waited = 0; waited <= timeoutMs; waited += 100)
    {
        if (capture::FindVirtualOutput(index, output))
        {
            Log(L"display: virtual monitor %u is %s", index + 1, output.deviceName.c_str());
            return true;
        }
        if (!switchedTopology && capture::IsVirtualMonitorMirrored(index))
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
