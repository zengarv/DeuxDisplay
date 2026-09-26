#pragma once

#include <winsock2.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace dd::adb
{

// Keeps `adb reverse tcp:port tcp:port` in place for every attached, authorized device, so a
// tablet becomes a display as soon as it's plugged in with the app open. Event-driven: it
// follows the adb server's host:track-devices stream, and re-applies the tunnel whenever a
// device (re)appears (cable plugged, USB mode switched, adb server restarted).
class AdbWatcher
{
  public:
    AdbWatcher(uint16_t port, std::wstring adbPath);
    ~AdbWatcher();
    AdbWatcher(const AdbWatcher&) = delete;
    AdbWatcher& operator=(const AdbWatcher&) = delete;

    void Start();
    void Stop();

    // --adb if given, else the bootstrap SDK's adb.exe, else adb.exe on PATH. Empty if none.
    static std::wstring FindAdb(const std::wstring& preferred);

  private:
    void Run();
    void Track(SOCKET tracker);
    bool Reverse(const std::string& serial);
    bool StartServer();

    uint16_t m_port;
    std::wstring m_adbPath;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::mutex m_socketMutex;
    SOCKET m_tracker = INVALID_SOCKET; // closed by Stop() to unblock the thread
};

} // namespace dd::adb
