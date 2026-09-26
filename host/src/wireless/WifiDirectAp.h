#pragma once

#include <winsock2.h>

#include <windows.devices.wifidirect.h>
#include <wrl/client.h>

#include <atomic>
#include <string>

namespace dd::wireless
{

// The host's own Wi-Fi network: a Wi-Fi Direct group owner in "legacy" mode, which ordinary
// Wi-Fi clients (the tablet) join like any WPA2 access point. One hop, no router in between.
// Works from an unpackaged desktop process; Windows puts the host at 192.168.137.1.
class WifiDirectAp
{
  public:
    WifiDirectAp() = default;
    ~WifiDirectAp();
    WifiDirectAp(const WifiDirectAp&) = delete;
    WifiDirectAp& operator=(const WifiDirectAp&) = delete;

    HRESULT Start(const std::wstring& ssid, const std::wstring& passphrase);
    void Stop();

    // False once Windows stopped or aborted the network (e.g. Wi-Fi turned off).
    bool Running() const { return m_running; }

    // The network's IPv4 address on this PC, polled until it's assigned or timeoutMs elapses.
    static bool WaitForAddress(DWORD timeoutMs, in_addr& address);

  private:
    Microsoft::WRL::ComPtr<ABI::Windows::Devices::WiFiDirect::IWiFiDirectAdvertisementPublisher> m_publisher;
    EventRegistrationToken m_statusToken{};
    std::atomic<bool> m_running{false};
};

} // namespace dd::wireless
