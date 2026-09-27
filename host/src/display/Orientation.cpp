#include "Orientation.h"

#include <windows.h>

#include <utility>

#include "../common/Log.h"

namespace dd
{

bool SetDisplayOrientation(const std::wstring& deviceName, uint16_t degrees)
{
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsExW(deviceName.c_str(), ENUM_CURRENT_SETTINGS, &mode, 0))
    {
        Log(L"orientation: can't read the mode of %s (%lu)", deviceName.c_str(), GetLastError());
        return false;
    }

    const DWORD target = (degrees / 90) % 4; // DMDO_DEFAULT / DMDO_90 / DMDO_180 / DMDO_270
    if (mode.dmDisplayOrientation == target)
    {
        return true;
    }
    // Portrait <-> landscape swaps the desktop's width and height.
    if ((mode.dmDisplayOrientation ^ target) & 1)
    {
        std::swap(mode.dmPelsWidth, mode.dmPelsHeight);
    }
    mode.dmDisplayOrientation = target;
    mode.dmFields = DM_DISPLAYORIENTATION | DM_PELSWIDTH | DM_PELSHEIGHT;

    const LONG result = ChangeDisplaySettingsExW(deviceName.c_str(), &mode, nullptr, 0, nullptr);
    if (result != DISP_CHANGE_SUCCESSFUL)
    {
        Log(L"orientation: Windows refused %u degrees on %s (%ld)", degrees, deviceName.c_str(), result);
        return false;
    }
    Log(L"orientation: %s rotated to %u degrees", deviceName.c_str(), degrees);
    return true;
}

} // namespace dd
