#pragma once

#include <windows.h>

#include <mutex>

#include "TouchTracker.h"

namespace dd::input
{

// Injects client touches onto the virtual monitor with Windows touch injection
// (InjectTouchInput). No admin rights needed; like any injected input, it can't reach windows
// of elevated processes (UIPI). Thread-safe: frames arrive on the network reader thread while
// the capture thread updates the target after mode changes.
class TouchInjector
{
  public:
    TouchInjector() = default;
    ~TouchInjector() { ReleaseAll(); }
    TouchInjector(const TouchInjector&) = delete;
    TouchInjector& operator=(const TouchInjector&) = delete;

    // Desktop rectangle of the streamed output (physical pixels; the host is per-monitor DPI aware).
    void SetTarget(const RECT& desktopRect);
    void Inject(const protocol::TouchFrame& frame);
    void ReleaseAll();

  private:
    void Send(const std::vector<ContactEvent>& events);

    std::mutex m_lock;
    RECT m_target{};
    TouchTracker m_tracker;
    int m_failuresLogged = 0;
};

} // namespace dd::input
