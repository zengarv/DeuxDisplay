#pragma once

#include <windows.h>

#include "../../../driver/DeuxDisplayIdd/Public.h"

namespace dd
{

// One-time setup (administrator): creates/removes the persistent software device that makes
// Windows load DeuxDisplayIdd. Called by scripts/install-driver.ps1 and uninstall-driver.ps1.
HRESULT InstallDevice();
HRESULT RemoveDevice();

// Plugs the virtual monitor through the driver's device interface. No admin rights needed.
// The monitor is unplugged by Unplug(), by destruction, or by the driver if this process dies.
class VirtualDisplay
{
  public:
    VirtualDisplay() = default;
    ~VirtualDisplay();
    VirtualDisplay(const VirtualDisplay&) = delete;
    VirtualDisplay& operator=(const VirtualDisplay&) = delete;

    HRESULT Plug(const driver::PlugRequest& request);
    void Unplug();
    bool IsPlugged() const { return m_plugged; }

  private:
    HRESULT Open();

    HANDLE m_device = INVALID_HANDLE_VALUE;
    bool m_plugged = false;
};

// Plug request for the reference device (OnePlus Pad Go), used by --create-display.
driver::PlugRequest DefaultPlugRequest();

// Waits for the plugged monitor to become its own desktop output. If Windows attached it in
// duplicate mode, switches the topology to "extend" (like Win+P -> Extend; no admin needed).
bool WaitForExtendedDisplay(DWORD timeoutMs);

} // namespace dd
