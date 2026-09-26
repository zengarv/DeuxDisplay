// DeuxDisplayAgent: keeps DeuxDisplayHost running in the background from login, so a tablet
// becomes a monitor as soon as it's plugged in (app open). No console window: a tray icon
// shows the state and offers Open log / Restart / Exit. Host output goes to
// %LOCALAPPDATA%\DeuxDisplay\host.log.
//
//   DeuxDisplayAgent.exe [extra host options, e.g. --codec hevc]
//
// runs `DeuxDisplayHost.exe --serve --transport usb [extra options]` from its own folder.

#include <windows.h>

#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace
{

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kStateMessage = WM_APP + 2;
constexpr UINT kMenuOpenLog = 1;
constexpr UINT kMenuRestart = 2;
constexpr UINT kMenuExit = 3;
constexpr ULONGLONG kMaxLogBytes = 5ull * 1024 * 1024;

HWND g_window = nullptr;
UINT g_taskbarCreated = 0;
HANDLE g_job = nullptr;
std::filesystem::path g_hostExe;
std::filesystem::path g_logPath;
std::wstring g_hostArgs;
std::atomic<bool> g_stopping{false};

std::mutex g_stateMutex;
std::wstring g_tooltip = L"DeuxDisplay: starting";
HANDLE g_hostProcess = nullptr; // guarded by g_stateMutex

void SetTooltip(std::wstring text)
{
    {
        std::lock_guard lock(g_stateMutex);
        g_tooltip = std::move(text);
    }
    PostMessageW(g_window, kStateMessage, 0, 0);
}

void UpdateTrayIcon(DWORD action)
{
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = g_window;
    icon.uID = 1;
    icon.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    icon.uCallbackMessage = kTrayMessage;
    icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    {
        std::lock_guard lock(g_stateMutex);
        wcsncpy_s(icon.szTip, g_tooltip.c_str(), _TRUNCATE);
    }
    Shell_NotifyIconW(action, &icon);
}

// The tray state follows the host's session log lines.
void OnHostLine(const std::string& line)
{
    const size_t client = line.find("session: client \"");
    if (client != std::string::npos)
    {
        const size_t start = client + 17;
        const size_t end = line.find('"', start);
        const std::string name = line.substr(start, end == std::string::npos ? std::string::npos : end - start);
        SetTooltip(L"DeuxDisplay: showing on " + std::wstring(name.begin(), name.end()));
    }
    else if (line.find("session: ended") != std::string::npos ||
             line.find("waiting for a client") != std::string::npos)
    {
        SetTooltip(L"DeuxDisplay: waiting for a tablet (plug in with the app open)");
    }
}

HANDLE OpenLog()
{
    std::error_code ec;
    std::filesystem::create_directories(g_logPath.parent_path(), ec);
    const bool tooBig = std::filesystem::exists(g_logPath, ec) && std::filesystem::file_size(g_logPath, ec) > kMaxLogBytes;
    HANDLE log = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             tooBig ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return log == INVALID_HANDLE_VALUE ? nullptr : log;
}

void WriteLog(HANDLE log, const char* data, size_t size)
{
    DWORD written = 0;
    if (log)
    {
        WriteFile(log, data, static_cast<DWORD>(size), &written, nullptr);
    }
}

// Runs the host once and pumps its output into the log. Returns when the host exits.
void RunHostOnce(HANDLE log)
{
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &inherit, 0))
    {
        return;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = nullptr;

    std::wstring command = L"\"" + g_hostExe.wstring() + L"\" --serve --transport usb " + g_hostArgs;
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(g_hostExe.c_str(), command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                        g_hostExe.parent_path().c_str(), &startup, &process);
    CloseHandle(writePipe);
    if (!started)
    {
        char message[128];
        const int n = std::snprintf(message, sizeof(message), "agent: can't start the host (%lu)\r\n", GetLastError());
        WriteLog(log, message, static_cast<size_t>(n));
        CloseHandle(readPipe);
        return;
    }
    AssignProcessToJobObject(g_job, process.hProcess); // dies with the agent
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    {
        std::lock_guard lock(g_stateMutex);
        g_hostProcess = process.hProcess;
    }

    std::string pending;
    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(readPipe, buffer, sizeof(buffer), &got, nullptr) && got > 0)
    {
        WriteLog(log, buffer, got);
        pending.append(buffer, got);
        for (size_t end; (end = pending.find('\n')) != std::string::npos; pending.erase(0, end + 1))
        {
            OnHostLine(pending.substr(0, end));
        }
    }
    CloseHandle(readPipe);

    WaitForSingleObject(process.hProcess, INFINITE);
    {
        std::lock_guard lock(g_stateMutex);
        g_hostProcess = nullptr;
    }
    DWORD exitCode = 0;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    char message[96];
    const int n = std::snprintf(message, sizeof(message), "agent: host exited (%lu)\r\n", exitCode);
    WriteLog(log, message, static_cast<size_t>(n));
}

void Supervise()
{
    HANDLE log = OpenLog();
    DWORD backoffMs = 1000;
    while (!g_stopping)
    {
        const ULONGLONG started = GetTickCount64();
        RunHostOnce(log);
        if (g_stopping)
        {
            break;
        }
        SetTooltip(L"DeuxDisplay: host stopped, restarting (see log)");
        // A host that ran for a while gets restarted right away; a crash loop backs off.
        backoffMs = GetTickCount64() - started > 60'000 ? 1000 : std::min<DWORD>(backoffMs * 2, 30'000);
        for (DWORD waited = 0; waited < backoffMs && !g_stopping; waited += 100)
        {
            Sleep(100);
        }
    }
    if (log)
    {
        CloseHandle(log);
    }
}

void StopHost()
{
    std::lock_guard lock(g_stateMutex);
    if (g_hostProcess)
    {
        // The driver unplugs the monitor as soon as the host's device handle closes.
        TerminateProcess(g_hostProcess, 0);
    }
}

void ShowMenu()
{
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kMenuOpenLog, L"Open log");
    AppendMenuW(menu, MF_STRING, kMenuRestart, L"Restart host");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"Exit DeuxDisplay");
    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(g_window); // so the menu closes when clicking elsewhere
    const UINT choice =
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, g_window, nullptr);
    DestroyMenu(menu);

    switch (choice)
    {
    case kMenuOpenLog:
        ShellExecuteW(nullptr, L"open", g_logPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case kMenuRestart:
        StopHost(); // the supervisor starts it again
        break;
    case kMenuExit:
        g_stopping = true;
        StopHost();
        DestroyWindow(g_window);
        break;
    default:
        break;
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == kTrayMessage)
    {
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_LBUTTONUP)
        {
            ShowMenu();
        }
        return 0;
    }
    if (message == kStateMessage)
    {
        UpdateTrayIcon(NIM_MODIFY);
        return 0;
    }
    if (message == g_taskbarCreated)
    {
        UpdateTrayIcon(NIM_ADD); // Explorer restarted
        return 0;
    }
    if (message == WM_DESTROY)
    {
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = window;
        icon.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &icon);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

std::filesystem::path LocalAppData()
{
    wchar_t path[MAX_PATH] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
    return path;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int)
{
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\DeuxDisplayAgent");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        return 0; // already running (e.g. started at login and again by hand)
    }

    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    g_hostExe = std::filesystem::path(self).parent_path() / L"DeuxDisplayHost.exe";
    g_logPath = LocalAppData() / L"DeuxDisplay" / L"host.log";
    g_hostArgs = commandLine ? commandLine : L"";

    // Kill-on-close: if the agent exits or crashes, the host (and its virtual monitor) goes too.
    g_job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"DeuxDisplayAgent";
    RegisterClassW(&windowClass);
    g_window = CreateWindowW(windowClass.lpszClassName, L"DeuxDisplay", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
                             instance, nullptr);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    UpdateTrayIcon(NIM_ADD);

    std::thread supervisor(Supervise);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    g_stopping = true;
    StopHost();
    supervisor.join();
    CloseHandle(g_job);
    CloseHandle(single);
    return 0;
}
