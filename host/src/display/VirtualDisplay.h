#pragma once

#include <windows.h>
#include <swdevice.h>

namespace dd
{

// Owns the software device that makes Windows load DeuxDisplayIdd. The virtual monitor
// exists exactly as long as this object (and the process) does.
class VirtualDisplay
{
  public:
    VirtualDisplay() = default;
    ~VirtualDisplay();
    VirtualDisplay(const VirtualDisplay&) = delete;
    VirtualDisplay& operator=(const VirtualDisplay&) = delete;

    HRESULT Create(DWORD timeoutMs = 10000);
    void Destroy();

  private:
    HSWDEVICE m_device = nullptr;
};

} // namespace dd
