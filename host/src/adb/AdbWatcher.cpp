#include "AdbWatcher.h"

#include <ws2tcpip.h>

#include <chrono>
#include <filesystem>
#include <map>

#include "../common/Log.h"
#include "AdbProtocol.h"

namespace dd::adb
{
namespace
{

SOCKET ConnectServer()
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
    {
        return s;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kServerPort);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    if (connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
    {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

bool SendAll(SOCKET s, const std::string& data)
{
    size_t sent = 0;
    while (sent < data.size())
    {
        const int n = send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (n <= 0)
        {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool RecvAll(SOCKET s, char* data, size_t size)
{
    while (size > 0)
    {
        const int n = recv(s, data, static_cast<int>(size), 0);
        if (n <= 0)
        {
            return false;
        }
        data += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

// A length-prefixed string (4 hex digits + bytes), as used by FAIL replies and track-devices.
bool RecvString(SOCKET s, std::string& out)
{
    char digits[4];
    if (!RecvAll(s, digits, sizeof(digits)))
    {
        return false;
    }
    const auto length = ParseHexLength({digits, sizeof(digits)});
    if (!length)
    {
        return false;
    }
    out.resize(*length);
    return *length == 0 || RecvAll(s, out.data(), out.size());
}

// Sends a request and reads "OKAY", or "FAIL" plus a message (returned in `failure`).
bool Request(SOCKET s, std::string_view payload, std::string& failure)
{
    char status[4];
    if (!SendAll(s, EncodeRequest(payload)) || !RecvAll(s, status, sizeof(status)))
    {
        failure = "adb server closed the connection";
        return false;
    }
    if (std::string_view(status, 4) == "OKAY")
    {
        return true;
    }
    if (std::string_view(status, 4) != "FAIL" || !RecvString(s, failure))
    {
        failure = "unexpected reply";
    }
    return false;
}

std::wstring Wide(const std::string& s)
{
    return std::wstring(s.begin(), s.end()); // serials are ASCII
}

} // namespace

AdbWatcher::AdbWatcher(uint16_t port, std::wstring adbPath) : m_port(port), m_adbPath(std::move(adbPath))
{
}

AdbWatcher::~AdbWatcher()
{
    Stop();
}

void AdbWatcher::Start()
{
    m_stop = false;
    m_thread = std::thread([this] { Run(); });
}

void AdbWatcher::Stop()
{
    m_stop = true;
    {
        std::lock_guard lock(m_socketMutex);
        if (m_tracker != INVALID_SOCKET)
        {
            closesocket(m_tracker);
            m_tracker = INVALID_SOCKET;
        }
    }
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

std::wstring AdbWatcher::FindAdb(const std::wstring& preferred)
{
    std::error_code ec;
    if (!preferred.empty())
    {
        return std::filesystem::exists(preferred, ec) ? preferred : std::wstring{};
    }
    wchar_t localAppData[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH))
    {
        const auto bootstrap = std::filesystem::path(localAppData) / L"DeuxDisplay" / L"tools" / L"android-sdk" /
                               L"platform-tools" / L"adb.exe";
        if (std::filesystem::exists(bootstrap, ec))
        {
            return bootstrap.wstring();
        }
    }
    wchar_t found[MAX_PATH] = {};
    if (SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH, found, nullptr))
    {
        return found;
    }
    return {};
}

void AdbWatcher::Run()
{
    auto pause = [this](int ms) {
        for (int waited = 0; waited < ms && !m_stop; waited += 50)
        {
            Sleep(50);
        }
    };

    while (!m_stop)
    {
        SOCKET tracker = ConnectServer();
        if (tracker == INVALID_SOCKET)
        {
            // No adb server yet (fresh login) or it was killed: start one.
            if (!StartServer())
            {
                pause(5000);
            }
            continue;
        }
        {
            std::lock_guard lock(m_socketMutex);
            if (m_stop)
            {
                closesocket(tracker);
                break;
            }
            m_tracker = tracker;
        }
        Track(tracker);
        {
            std::lock_guard lock(m_socketMutex);
            if (m_tracker != INVALID_SOCKET)
            {
                closesocket(m_tracker);
                m_tracker = INVALID_SOCKET;
            }
        }
        if (!m_stop)
        {
            Log(L"usb: lost the adb server, reconnecting");
            pause(500);
        }
    }
}

void AdbWatcher::Track(SOCKET tracker)
{
    std::string failure;
    if (!Request(tracker, "host:track-devices", failure))
    {
        Log(L"usb: adb track-devices failed: %S", failure.c_str());
        return;
    }
    Log(L"usb: watching for tablets (adb); tunnel tcp:%u", m_port);

    std::map<std::string, std::string> states; // last state per serial
    std::map<std::string, bool> tunnels;       // serial -> tunnel in place
    for (;;)
    {
        // While a tunnel is still missing, wake up to retry it (adbd may not be ready yet).
        bool pending = false;
        for (const auto& [serial, ok] : tunnels)
        {
            pending |= !ok;
        }
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(tracker, &readable);
        timeval timeout{0, 250'000};
        const int ready = select(0, &readable, nullptr, nullptr, pending ? &timeout : nullptr);
        if (ready == SOCKET_ERROR || m_stop)
        {
            return;
        }
        if (ready == 0)
        {
            for (auto& [serial, ok] : tunnels)
            {
                ok = ok || Reverse(serial);
            }
            continue;
        }

        std::string list;
        if (!RecvString(tracker, list))
        {
            return;
        }
        std::map<std::string, std::string> now;
        for (const auto& device : ParseDeviceList(list))
        {
            now[device.serial] = device.state;
        }

        for (const auto& [serial, state] : states)
        {
            if (!now.contains(serial))
            {
                Log(L"usb: %s disconnected", Wide(serial).c_str());
                tunnels.erase(serial);
            }
        }
        for (const auto& [serial, state] : now)
        {
            const auto previous = states.find(serial);
            const bool changed = previous == states.end() || previous->second != state;
            if (state == "device")
            {
                if (changed || !tunnels.contains(serial))
                {
                    tunnels[serial] = Reverse(serial);
                }
            }
            else
            {
                tunnels.erase(serial);
                if (changed && state == "unauthorized")
                {
                    Log(L"usb: %s is waiting for you to allow USB debugging on the tablet", Wide(serial).c_str());
                }
            }
        }
        states = std::move(now);
    }
}

bool AdbWatcher::Reverse(const std::string& serial)
{
    SOCKET s = ConnectServer();
    if (s == INVALID_SOCKET)
    {
        return false;
    }
    const DWORD timeoutMs = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));

    std::string failure;
    const bool ok = Request(s, "host:transport:" + serial, failure) && Request(s, ReverseRequest(m_port), failure);
    closesocket(s);
    if (ok)
    {
        Log(L"usb: %s connected, tunnel ready", Wide(serial).c_str());
    }
    else
    {
        Log(L"usb: %s: adb reverse failed (%S), retrying", Wide(serial).c_str(), failure.c_str());
    }
    return ok;
}

bool AdbWatcher::StartServer()
{
    if (m_adbPath.empty())
    {
        static bool warned = false;
        if (!warned)
        {
            Log(L"usb: adb not found (pass --adb PATH); run `adb reverse tcp:%u tcp:%u` by hand", m_port, m_port);
            warned = true;
        }
        return false;
    }
    std::wstring command = L"\"" + m_adbPath + L"\" start-server";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    // The server outlives this client. Break it out of our job (DeuxDisplayAgent kills its job on
    // exit) so other adb users keep their server; fall back if the job doesn't allow that.
    if (!CreateProcessW(m_adbPath.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &startup, &process) &&
        !CreateProcessW(m_adbPath.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &startup, &process))
    {
        Log(L"usb: can't run %s (%lu)", m_adbPath.c_str(), GetLastError());
        return false;
    }
    WaitForSingleObject(process.hProcess, 20000);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exitCode != 0)
    {
        Log(L"usb: adb start-server failed (%lu)", exitCode);
        return false;
    }
    Log(L"usb: started the adb server");
    return true;
}

} // namespace dd::adb
