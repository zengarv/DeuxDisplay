#pragma once

#include <windows.h>

namespace dd::wireless
{

// While alive, asks every Wi-Fi adapter for media streaming mode (no power-save dozing) and
// no background scans (which park the radio off-channel for tens of ms). Windows reverts
// both when the handle closes, including if the host crashes.
class WlanTuning
{
  public:
    WlanTuning();
    ~WlanTuning();
    WlanTuning(const WlanTuning&) = delete;
    WlanTuning& operator=(const WlanTuning&) = delete;

  private:
    HANDLE m_handle = nullptr;
};

} // namespace dd::wireless
