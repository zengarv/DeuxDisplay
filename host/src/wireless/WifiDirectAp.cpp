#include "WifiDirectAp.h"

#include <iphlpapi.h>

#include <windows.security.credentials.h>
#include <wrl/event.h>
#include <wrl/wrappers/corewrappers.h>

#include <vector>

#include "../common/Log.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;
namespace WiFiDirect = ABI::Windows::Devices::WiFiDirect;
namespace Credentials = ABI::Windows::Security::Credentials;

namespace dd::wireless
{
namespace
{

using StatusHandler = ABI::Windows::Foundation::ITypedEventHandler<
    WiFiDirect::WiFiDirectAdvertisementPublisher*, WiFiDirect::WiFiDirectAdvertisementPublisherStatusChangedEventArgs*>;

const wchar_t* StatusName(WiFiDirect::WiFiDirectAdvertisementPublisherStatus status)
{
    switch (status)
    {
    case WiFiDirect::WiFiDirectAdvertisementPublisherStatus_Created:
        return L"created";
    case WiFiDirect::WiFiDirectAdvertisementPublisherStatus_Started:
        return L"started";
    case WiFiDirect::WiFiDirectAdvertisementPublisherStatus_Stopped:
        return L"stopped";
    case WiFiDirect::WiFiDirectAdvertisementPublisherStatus_Aborted:
        return L"aborted";
    default:
        return L"unknown";
    }
}

const wchar_t* ErrorName(WiFiDirect::WiFiDirectError error)
{
    switch (error)
    {
    case WiFiDirect::WiFiDirectError_Success:
        return L"";
    case WiFiDirect::WiFiDirectError_RadioNotAvailable:
        return L" (Wi-Fi radio off or unavailable)";
    case WiFiDirect::WiFiDirectError_ResourceInUse:
        return L" (Wi-Fi Direct in use, e.g. by Mobile Hotspot)";
    default:
        return L" (error)";
    }
}

template <typename T> HRESULT Activate(const wchar_t* runtimeClass, ComPtr<T>& out)
{
    ComPtr<IInspectable> instance;
    HRESULT hr = RoActivateInstance(HStringReference(runtimeClass).Get(), &instance);
    return SUCCEEDED(hr) ? instance.As(&out) : hr;
}

} // namespace

WifiDirectAp::~WifiDirectAp()
{
    Stop();
}

HRESULT WifiDirectAp::Start(const std::wstring& ssid, const std::wstring& passphrase)
{
    Stop();

    ComPtr<WiFiDirect::IWiFiDirectAdvertisementPublisher> publisher;
    HRESULT hr = Activate(RuntimeClass_Windows_Devices_WiFiDirect_WiFiDirectAdvertisementPublisher, publisher);
    if (FAILED(hr))
    {
        return hr;
    }

    ComPtr<WiFiDirect::IWiFiDirectAdvertisement> advertisement;
    ComPtr<WiFiDirect::IWiFiDirectLegacySettings> legacy;
    ComPtr<Credentials::IPasswordCredential> credential;
    hr = publisher->get_Advertisement(&advertisement);
    if (SUCCEEDED(hr))
    {
        // Group owner without a peer negotiation: the network exists as soon as we start.
        hr = advertisement->put_IsAutonomousGroupOwnerEnabled(true);
    }
    if (SUCCEEDED(hr))
    {
        // Not discoverable as a Wi-Fi Direct peer: the tablet joins as a plain Wi-Fi client. (The
        // default already; set explicitly. It does not remove the per-beacon gaps measured in
        // docs/latency-notes.md.)
        hr = advertisement->put_ListenStateDiscoverability(
            WiFiDirect::WiFiDirectAdvertisementListenStateDiscoverability_None);
    }
    if (SUCCEEDED(hr))
    {
        hr = advertisement->get_LegacySettings(&legacy);
    }
    if (SUCCEEDED(hr))
    {
        hr = legacy->put_IsEnabled(true);
    }
    if (SUCCEEDED(hr))
    {
        hr = legacy->put_Ssid(HStringReference(ssid.c_str()).Get());
    }
    if (SUCCEEDED(hr))
    {
        hr = Activate(RuntimeClass_Windows_Security_Credentials_PasswordCredential, credential);
    }
    if (SUCCEEDED(hr))
    {
        hr = credential->put_Password(HStringReference(passphrase.c_str()).Get());
    }
    if (SUCCEEDED(hr))
    {
        hr = legacy->put_Passphrase(credential.Get());
    }
    if (FAILED(hr))
    {
        return hr;
    }

    hr = publisher->add_StatusChanged(
        Callback<StatusHandler>([this](WiFiDirect::IWiFiDirectAdvertisementPublisher*,
                                       WiFiDirect::IWiFiDirectAdvertisementPublisherStatusChangedEventArgs* args) {
            WiFiDirect::WiFiDirectAdvertisementPublisherStatus status{};
            WiFiDirect::WiFiDirectError error = WiFiDirect::WiFiDirectError_Success;
            args->get_Status(&status);
            args->get_Error(&error);
            m_running = status == WiFiDirect::WiFiDirectAdvertisementPublisherStatus_Started;
            Log(L"wifi: access point %s%s", StatusName(status), ErrorName(error));
            return S_OK;
        }).Get(),
        &m_statusToken);
    if (FAILED(hr))
    {
        return hr;
    }
    m_publisher = publisher;

    hr = publisher->Start();
    if (FAILED(hr))
    {
        Stop();
        return hr;
    }
    // Start() returns before the group is up; the status handler reports the outcome.
    for (int i = 0; i < 50; ++i)
    {
        WiFiDirect::WiFiDirectAdvertisementPublisherStatus status{};
        publisher->get_Status(&status);
        if (status == WiFiDirect::WiFiDirectAdvertisementPublisherStatus_Started)
        {
            m_running = true;
            return S_OK;
        }
        if (status == WiFiDirect::WiFiDirectAdvertisementPublisherStatus_Aborted ||
            status == WiFiDirect::WiFiDirectAdvertisementPublisherStatus_Stopped)
        {
            break;
        }
        Sleep(100);
    }
    Stop();
    return HRESULT_FROM_WIN32(ERROR_NOT_READY);
}

void WifiDirectAp::Stop()
{
    if (!m_publisher)
    {
        return;
    }
    m_publisher->remove_StatusChanged(m_statusToken);
    m_publisher->Stop();
    m_publisher.Reset();
    m_running = false;
}

bool WifiDirectAp::WaitForAddress(DWORD timeoutMs, in_addr& address)
{
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    std::vector<uint8_t> buffer;
    for (;;)
    {
        ULONG size = 16 * 1024;
        buffer.resize(size);
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
        if (GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size) == ERROR_BUFFER_OVERFLOW)
        {
            buffer.resize(size);
            adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
            GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size);
        }
        for (auto* a = adapters; a && size; a = a->Next)
        {
            if (a->OperStatus != IfOperStatusUp || !a->Description ||
                wcsncmp(a->Description, L"Microsoft Wi-Fi Direct Virtual Adapter", 38) != 0)
            {
                continue;
            }
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next)
            {
                const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
                const uint8_t first = sin->sin_addr.S_un.S_un_b.s_b1;
                const uint8_t second = sin->sin_addr.S_un.S_un_b.s_b2;
                if (sin->sin_family == AF_INET && !(first == 169 && second == 254)) // skip link-local
                {
                    address = sin->sin_addr;
                    return true;
                }
            }
        }
        if (GetTickCount64() >= deadline)
        {
            return false;
        }
        Sleep(200);
    }
}

} // namespace dd::wireless
