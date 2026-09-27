#pragma once

#include <windows.h>

#include "../../../driver/DeuxDisplayIdd/Public.h"

namespace dd
{

// One-time setup (administrator): creates/removes the root-enumerated device that makes Windows
// load DeuxDisplayIdd. Windows keeps it across reboots, like detected hardware. Idempotent.
// Called by scripts/install-driver.ps1 and uninstall-driver.ps1.
HRESULT InstallDevice();
HRESULT RemoveDevice();

// Plugs a virtual monitor through the driver's device interface. No admin rights needed.
// Each instance holds its own driver handle and so its own monitor: one instance per client.
// The monitor is unplugged by Unplug(), by destruction, or by the driver if this process dies.
class VirtualDisplay
{
  public:
    VirtualDisplay() = default;
    ~VirtualDisplay();
    VirtualDisplay(const VirtualDisplay&) = delete;
    VirtualDisplay& operator=(const VirtualDisplay&) = delete;

    // Fails when all driver::kMaxMonitors monitors are in use by other instances.
    HRESULT Plug(const driver::PlugRequest& request);
    void Unplug();
    bool IsPlugged() const { return m_plugged; }
    // The driver connector the monitor is on; selects its hardware ID (see OutputLocator.h).
    unsigned Index() const { return m_index; }

  private:
    HRESULT Open();

    HANDLE m_device = INVALID_HANDLE_VALUE;
    bool m_plugged = false;
    unsigned m_index = 0;
};

// Plug request for the reference device (OnePlus Pad Go), used by --create-display.
driver::PlugRequest DefaultPlugRequest();

// Waits for virtual monitor `index` to become its own desktop output. If Windows attached it in
// duplicate mode, switches the topology to "extend" (like Win+P -> Extend; no admin needed).
bool WaitForExtendedDisplay(unsigned index, DWORD timeoutMs);

} // namespace dd
