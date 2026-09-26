#include "VirtualDisplay.h"

namespace dd
{
namespace
{

struct CreationResult
{
    HANDLE event;
    HRESULT hr;
};

VOID WINAPI OnCreated(HSWDEVICE, HRESULT hr, PVOID context, PCWSTR)
{
    auto* result = static_cast<CreationResult*>(context);
    result->hr = hr;
    SetEvent(result->event);
}

} // namespace

VirtualDisplay::~VirtualDisplay()
{
    Destroy();
}

HRESULT VirtualDisplay::Create(DWORD timeoutMs)
{
    if (m_device)
    {
        return S_OK;
    }

    // Must match the hardware ID in driver/DeuxDisplayIdd/DeuxDisplayIdd.inf.
    static constexpr wchar_t kIds[] = L"DeuxDisplayIdd\0";

    SW_DEVICE_CREATE_INFO info{};
    info.cbSize = sizeof(info);
    info.pszInstanceId = L"DeuxDisplayIdd";
    info.pszzHardwareIds = kIds;
    info.pszzCompatibleIds = kIds;
    info.pszDeviceDescription = L"DeuxDisplay Virtual Monitor";
    info.CapabilityFlags =
        SWDeviceCapabilitiesRemovable | SWDeviceCapabilitiesSilentInstall | SWDeviceCapabilitiesDriverRequired;

    CreationResult result{CreateEventW(nullptr, TRUE, FALSE, nullptr), E_PENDING};
    if (!result.event)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    HRESULT hr = SwDeviceCreate(L"DeuxDisplayIdd", L"HTREE\\ROOT\\0", &info, 0, nullptr, OnCreated, &result,
                                &m_device);
    if (SUCCEEDED(hr))
    {
        hr = WaitForSingleObject(result.event, timeoutMs) == WAIT_OBJECT_0 ? result.hr
                                                                          : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }

    // Close the device before the event: the creation callback must not fire into freed state.
    if (FAILED(hr))
    {
        Destroy();
    }
    CloseHandle(result.event);
    return hr;
}

void VirtualDisplay::Destroy()
{
    if (m_device)
    {
        SwDeviceClose(m_device);
        m_device = nullptr;
    }
}

} // namespace dd
