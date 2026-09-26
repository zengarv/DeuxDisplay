#include "TouchInjector.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "../common/Log.h"

namespace dd::input
{
namespace
{

bool EnsureTouchInjection()
{
    // Once per process; later sessions reuse it.
    static const bool ok = [] {
        if (InitializeTouchInjection(static_cast<UINT32>(protocol::kMaxTouchContacts), TOUCH_FEEDBACK_DEFAULT))
        {
            return true;
        }
        Log(L"input: InitializeTouchInjection failed (%lu); touch is disabled", GetLastError());
        return false;
    }();
    return ok;
}

LONG MapAxis(uint16_t value, LONG begin, LONG end)
{
    const LONG span = std::max<LONG>(end - begin - 1, 0);
    return begin + static_cast<LONG>(std::lround(value * static_cast<double>(span) / 65535.0));
}

POINTER_FLAGS FlagsFor(ContactState state)
{
    switch (state)
    {
    case ContactState::Down:
        return POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
    case ContactState::Update:
        return POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
    case ContactState::Up:
        return POINTER_FLAG_UP;
    case ContactState::Cancel:
        return POINTER_FLAG_UP | POINTER_FLAG_CANCELED;
    }
    return POINTER_FLAG_UP;
}

} // namespace

void TouchInjector::SetTarget(const RECT& desktopRect)
{
    std::lock_guard lock(m_lock);
    // A new mode or position: lift fingers so nothing stays pressed at stale coordinates.
    if (!EqualRect(&m_target, &desktopRect))
    {
        Send(m_tracker.ReleaseAll());
        m_target = desktopRect;
    }
}

void TouchInjector::Inject(const protocol::TouchFrame& frame)
{
    std::lock_guard lock(m_lock);
    if (IsRectEmpty(&m_target))
    {
        return; // no output yet
    }
    Send(m_tracker.Apply(frame));
}

void TouchInjector::ReleaseAll()
{
    std::lock_guard lock(m_lock);
    Send(m_tracker.ReleaseAll());
}

void TouchInjector::Send(const std::vector<ContactEvent>& events)
{
    if (events.empty() || IsRectEmpty(&m_target) || !EnsureTouchInjection())
    {
        return;
    }
    std::array<POINTER_TOUCH_INFO, protocol::kMaxTouchContacts> infos{};
    UINT32 count = 0;
    for (const auto& e : events)
    {
        if (count == infos.size())
        {
            break;
        }
        const LONG x = MapAxis(e.x, m_target.left, m_target.right);
        const LONG y = MapAxis(e.y, m_target.top, m_target.bottom);
        POINTER_TOUCH_INFO& info = infos[count++];
        info.pointerInfo.pointerType = PT_TOUCH;
        info.pointerInfo.pointerId = e.id;
        info.pointerInfo.ptPixelLocation = {x, y};
        info.pointerInfo.pointerFlags = FlagsFor(e.state);
        info.touchFlags = TOUCH_FLAG_NONE;
        info.touchMask = TOUCH_MASK_CONTACTAREA;
        info.rcContact = {x - 2, y - 2, x + 2, y + 2};
        if (e.pressure != 0)
        {
            info.touchMask |= TOUCH_MASK_PRESSURE;
            info.pressure = std::min<UINT32>(e.pressure, 1024);
        }
    }
    if (!InjectTouchInput(count, infos.data()) && !m_loggedFailure)
    {
        // Typical causes: an inconsistent contact sequence, or an elevated window under the finger.
        Log(L"input: InjectTouchInput failed (%lu)", GetLastError());
        m_loggedFailure = true;
    }
}

} // namespace dd::input
