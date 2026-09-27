#include "Shortcuts.h"

#include <windows.h>

#include <array>
#include <atomic>

#include "../common/Log.h"

namespace dd::input
{
namespace
{

struct Keys
{
    WORD modifier = 0; // 0 = none
    WORD key = 0;
};

Keys KeysFor(protocol::Action action)
{
    using protocol::Action;
    switch (action)
    {
    case Action::Undo:
        return {VK_CONTROL, 'Z'};
    case Action::Redo:
        return {VK_CONTROL, 'Y'};
    case Action::TaskView:
        return {VK_LWIN, VK_TAB};
    case Action::PlayPause:
        return {0, VK_MEDIA_PLAY_PAUSE};
    default:
        return {};
    }
}

INPUT KeyInput(WORD vk, bool up)
{
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    if (vk == VK_LWIN || vk == VK_MEDIA_PLAY_PAUSE)
    {
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    return input;
}

} // namespace

bool SendShortcut(protocol::Action action)
{
    const Keys keys = KeysFor(action);
    if (keys.key == 0)
    {
        return false;
    }
    std::array<INPUT, 4> inputs{};
    UINT count = 0;
    if (keys.modifier)
    {
        inputs[count++] = KeyInput(keys.modifier, false);
    }
    inputs[count++] = KeyInput(keys.key, false);
    inputs[count++] = KeyInput(keys.key, true);
    if (keys.modifier)
    {
        inputs[count++] = KeyInput(keys.modifier, true);
    }
    // All in one call, so the keys can't interleave with the user's own typing.
    if (SendInput(count, inputs.data(), sizeof(INPUT)) == count)
    {
        return true;
    }
    static std::atomic<int> failuresLogged{0};
    if (failuresLogged++ < 5)
    {
        // Typically an elevated window in the foreground (UIPI) or the secure desktop.
        Log(L"input: SendInput failed for shortcut %u (%lu)", static_cast<unsigned>(action), GetLastError());
    }
    return false;
}

} // namespace dd::input
