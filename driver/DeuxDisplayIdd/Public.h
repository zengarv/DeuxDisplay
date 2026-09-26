// Copyright (c) 2026 DeuxDisplay contributors. MIT License (see LICENSE at the repo root).
// Original code, not derived from the Microsoft sample.
//
// Interface between DeuxDisplayHost (user mode, no admin) and the DeuxDisplayIdd driver.
// The host opens the device interface below and plugs/unplugs the virtual monitor with IOCTLs.
// If the handle that plugged the monitor is closed (including by a crash), the driver unplugs it.

#pragma once

#include <windows.h>
#include <winioctl.h>

#include <cstdint>

namespace dd::driver
{

// {9D4C6A2E-3B1F-4E7A-8C5D-6F2A1B3C4D5E}
inline constexpr GUID kDeviceInterfaceGuid = {
    0x9d4c6a2e, 0x3b1f, 0x4e7a, {0x8c, 0x5d, 0x6f, 0x2a, 0x1b, 0x3c, 0x4d, 0x5e}};

// Software device created (persistently) by `DeuxDisplayHost --install-device`.
inline constexpr wchar_t kSoftwareDeviceEnumerator[] = L"DeuxDisplayIdd";
inline constexpr wchar_t kSoftwareDeviceInstanceId[] = L"DeuxDisplayIdd";
inline constexpr wchar_t kHardwareIds[] = L"DeuxDisplayIdd\0"; // must match DeuxDisplayIdd.inf

inline constexpr DWORD kIoctlPlug = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS);
inline constexpr DWORD kIoctlUnplug = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS);

inline constexpr uint32_t kPlugRequestVersion = 1;
inline constexpr size_t kMaxRefreshRates = 4;

#pragma pack(push, 1)
struct PlugRequest
{
    uint32_t version = kPlugRequestVersion;
    uint16_t width = 0;  // landscape pixels; must be even
    uint16_t height = 0; // must be even
    uint16_t widthMm = 0;
    uint16_t heightMm = 0;
    uint16_t refreshHz[kMaxRefreshRates] = {}; // [0] is preferred; 0 = unused
};
#pragma pack(pop)

// Validation shared by driver and host so both agree on what's acceptable.
inline bool IsValidPlugRequest(const PlugRequest& r)
{
    if (r.version != kPlugRequestVersion || r.width < 640 || r.height < 480 || r.width > 7680 ||
        r.height > 4320 || (r.width & 1) || (r.height & 1) || r.refreshHz[0] == 0)
    {
        return false;
    }
    for (uint16_t hz : r.refreshHz)
    {
        if (hz != 0 && (hz < 24 || hz > 240))
        {
            return false;
        }
    }
    return true;
}

} // namespace dd::driver
