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
//
// The process has one injected touch device, shared by all sessions: each injector owns its own
// range of pointer IDs, and every injected frame also repeats the other sessions' active contacts
// (Windows expects each frame to list all contacts that are down).
class TouchInjector
{
  public:
    TouchInjector();
    ~TouchInjector();
    TouchInjector(const TouchInjector&) = delete;
    TouchInjector& operator=(const TouchInjector&) = delete;

    // Desktop rectangle of the streamed output (physical pixels; the host is per-monitor DPI aware).
    void SetTarget(const RECT& desktopRect);
    void Inject(const protocol::TouchFrame& frame);
    void ReleaseAll();

  private:
    void Send(const std::vector<ContactEvent>& events);

    std::mutex m_lock;
    int m_bank = -1; // pointer IDs bank * kMaxTouchContacts + contact; -1 = no bank, touch off
    RECT m_target{};
    TouchTracker m_tracker;
    int m_failuresLogged = 0;
};

} // namespace dd::input
