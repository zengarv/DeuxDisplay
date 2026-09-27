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

// Windows moves the one system cursor to every touch. The cursor goes back to where it was once
// the last finger lifts, after this delay so the tap's promoted mouse click lands first.
constexpr DWORD kCursorRestoreDelayMs = 100;

// Process-wide injection state, shared by every session's injector.
struct SharedTouch
{
    std::mutex lock;
    std::array<bool, kBanks> bankUsed{};
    // Contacts currently down, as last injected (flags already "update"), by pointer ID.
    std::array<bool, kMaxPointers> down{};
    std::array<POINTER_TOUCH_INFO, kMaxPointers> last{};

    // Cursor position from before the first finger went down, pending restore.
    bool cursorSaved = false;
    POINT savedCursor{};
    RECT restoreRect{}; // output of the last lift; a cursor outside it was moved by the mouse
    PTP_TIMER restoreTimer = nullptr;
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

bool AnyDown(const SharedTouch& shared)
{
    return std::find(shared.down.begin(), shared.down.end(), true) != shared.down.end();
}

void MoveCursor(POINT pt)
{
    // SetCursorPos is exact but leaves the cursor hidden after touch; mouse input shows it again.
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = std::max(GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1, 1);
    const int height = std::max(GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1, 1);
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = MulDiv(pt.x - left, 65535, width);
    input.mi.dy = MulDiv(pt.y - top, 65535, height);
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &input, sizeof(input));
    SetCursorPos(pt.x, pt.y);
}

void CALLBACK RestoreCursor(PTP_CALLBACK_INSTANCE, PVOID context, PTP_TIMER)
{
    auto& shared = *static_cast<SharedTouch*>(context);
    std::lock_guard lock(shared.lock);
    if (!shared.cursorSaved || AnyDown(shared))
    {
        return; // a new touch started; it restores when it ends
    }
    shared.cursorSaved = false;
    POINT now{};
    // Left the touched output during the touch: the user moved the mouse, keep it there.
    if (GetCursorPos(&now) && PtInRect(&shared.restoreRect, now) &&
        (now.x != shared.savedCursor.x || now.y != shared.savedCursor.y))
    {
        MoveCursor(shared.savedCursor);
    }
}

void ArmCursorRestore(SharedTouch& shared, DWORD delayMs)
{
    if (!shared.restoreTimer)
    {
        // Lives for the process, like the touch device.
        shared.restoreTimer = CreateThreadpoolTimer(RestoreCursor, &shared, nullptr);
        if (!shared.restoreTimer)
        {
            return;
        }
    }
    // Relative due time in 100 ns units; 0 disarms. Re-arming replaces the pending due time.
    ULARGE_INTEGER due{};
    due.QuadPart = static_cast<ULONGLONG>(-static_cast<LONGLONG>(delayMs) * 10'000);
    FILETIME ft{due.LowPart, due.HighPart};
    SetThreadpoolTimer(shared.restoreTimer, delayMs ? &ft : nullptr, 0, 0);
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
    // First finger down anywhere: remember the cursor, unless an earlier touch's restore is
    // still pending (quick taps), which keeps the original position.
    const bool starting = !AnyDown(shared) && std::any_of(events.begin(), events.end(), [](const auto& e) {
                              return e.state == ContactState::Down;
                          });
    POINT cursor{};
    const bool haveCursor = starting && !shared.cursorSaved && GetCursorPos(&cursor);

    const bool injected = InjectTouchInput(count, infos.data()) != FALSE;
    if (starting && injected)
    {
        ArmCursorRestore(shared, 0);
        if (haveCursor)
        {
            shared.cursorSaved = true;
            shared.savedCursor = cursor;
        }
    }
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
    if (shared.cursorSaved && !AnyDown(shared))
    {
        shared.restoreRect = m_target;
        ArmCursorRestore(shared, kCursorRestoreDelayMs);
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
