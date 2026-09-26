#include "WlanTuning.h"

#include <wlanapi.h>

#include "../common/Log.h"

namespace dd::wireless
{

WlanTuning::WlanTuning()
{
    DWORD version = 0;
    DWORD result = WlanOpenHandle(2, nullptr, &version, &m_handle);
    if (result != ERROR_SUCCESS)
    {
        Log(L"wifi: WlanOpenHandle failed (%lu), radio tuning skipped", result);
        m_handle = nullptr;
        return;
    }

    WLAN_INTERFACE_INFO_LIST* interfaces = nullptr;
    result = WlanEnumInterfaces(m_handle, nullptr, &interfaces);
    if (result != ERROR_SUCCESS)
    {
        Log(L"wifi: WlanEnumInterfaces failed (%lu), radio tuning skipped", result);
        return;
    }
    for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i)
    {
        const WLAN_INTERFACE_INFO& info = interfaces->InterfaceInfo[i];
        BOOL streaming = TRUE;
        BOOL backgroundScan = FALSE;
        const DWORD streamingResult = WlanSetInterface(m_handle, &info.InterfaceGuid,
                                                       wlan_intf_opcode_media_streaming_mode, sizeof(streaming),
                                                       &streaming, nullptr);
        const DWORD scanResult =
            WlanSetInterface(m_handle, &info.InterfaceGuid, wlan_intf_opcode_background_scan_enabled,
                             sizeof(backgroundScan), &backgroundScan, nullptr);
        Log(L"wifi: %s: media streaming mode %s, background scan off %s", info.strInterfaceDescription,
            streamingResult == ERROR_SUCCESS ? L"on" : L"FAILED", scanResult == ERROR_SUCCESS ? L"ok" : L"FAILED");
    }
    WlanFreeMemory(interfaces);
}

WlanTuning::~WlanTuning()
{
    if (m_handle)
    {
        WlanCloseHandle(m_handle, nullptr);
    }
}

} // namespace dd::wireless
