#include "TouchInjector.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "../../../driver/DeuxDisplayIdd/Public.h"
#include "../common/Log.h"

namespace dd::input
{
namespace
{

// One bank of pointer IDs per concurrent session (at most one per virtual monitor).
constexpr size_t kBanks = driver::kMaxMonitors;
constexpr size_t kMaxPointers = kBanks * protocol::kMaxTouchContacts;

// Process-wide injection state, shared by every session's injector.
struct SharedTouch
{
    std::mutex lock;
    std::array<bool, kBanks> bankUsed{};
    // Contacts currently down, as last injected (flags already "update"), by pointer ID.
    std::array<bool, kMaxPointers> down{};
    std::array<POINTER_TOUCH_INFO, kMaxPointers> last{};
};

SharedTouch& Shared()
{
    static SharedTouch shared;
    return shared;
}

bool EnsureTouchInjection()
{
    // Once per process; later sessions reuse it.
    static const bool ok = [] {
        if (InitializeTouchInjection(static_cast<UINT32>(kMaxPointers), TOUCH_FEEDBACK_DEFAULT))
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

TouchInjector::TouchInjector()
{
    auto& shared = Shared();
    std::lock_guard lock(shared.lock);
    for (size_t bank = 0; bank < kBanks; ++bank)
    {
        if (!shared.bankUsed[bank])
        {
            shared.bankUsed[bank] = true;
            m_bank = static_cast<int>(bank);
            return;
        }
    }
    Log(L"input: too many touch sessions; touch is disabled for this one");
}

TouchInjector::~TouchInjector()
{
    ReleaseAll();
    if (m_bank >= 0)
    {
        auto& shared = Shared();
        std::lock_guard lock(shared.lock);
        shared.bankUsed[m_bank] = false;
    }
}

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
    if (events.empty() || m_bank < 0 || IsRectEmpty(&m_target) || !EnsureTouchInjection())
    {
        return;
    }
    const UINT32 firstId = static_cast<UINT32>(m_bank * protocol::kMaxTouchContacts);
    std::array<POINTER_TOUCH_INFO, kMaxPointers> infos{};
    UINT32 count = 0;
    for (const auto& e : events)
    {
        if (count == protocol::kMaxTouchContacts)
        {
            break;
        }
        const LONG x = MapAxis(e.x, m_target.left, m_target.right);
        const LONG y = MapAxis(e.y, m_target.top, m_target.bottom);
        POINTER_TOUCH_INFO& info = infos[count++];
        info.pointerInfo.pointerType = PT_TOUCH;
        info.pointerInfo.pointerId = firstId + e.id;
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
    const UINT32 ownCount = count;

    auto& shared = Shared();
    std::lock_guard sharedLock(shared.lock);
    // Other sessions' fingers stay down, unmoved. With one session this adds nothing.
    for (UINT32 id = 0; id < kMaxPointers; ++id)
    {
        if (shared.down[id] && (id < firstId || id >= firstId + protocol::kMaxTouchContacts))
        {
            infos[count++] = shared.last[id];
        }
    }
    const bool injected = InjectTouchInput(count, infos.data()) != FALSE;
    for (UINT32 i = 0; i < ownCount; ++i)
    {
        // A lift always clears the contact, even if injection failed, so it can't stay stuck in
        // other sessions' frames.
        const POINTER_TOUCH_INFO& info = infos[i];
        const UINT32 id = info.pointerInfo.pointerId;
        const bool inContact = (info.pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT) != 0;
        if (!inContact || injected)
        {
            shared.down[id] = inContact;
            shared.last[id] = info;
            shared.last[id].pointerInfo.pointerFlags = FlagsFor(ContactState::Update);
        }
    }
    if (!injected && m_failuresLogged < 5)
    {
        // Typical causes: an inconsistent contact sequence, a point outside the desktop, or an
        // elevated window under the finger.
        const DWORD error = GetLastError();
        ++m_failuresLogged;
        for (UINT32 i = 0; i < count; ++i)
        {
            const auto& p = infos[i].pointerInfo;
            Log(L"input: InjectTouchInput failed (%lu): id %u flags 0x%X at (%ld,%ld), target (%ld,%ld)-(%ld,%ld)",
                error, p.pointerId, p.pointerFlags, p.ptPixelLocation.x, p.ptPixelLocation.y, m_target.left,
                m_target.top, m_target.right, m_target.bottom);
        }
    }
}

} // namespace dd::input
