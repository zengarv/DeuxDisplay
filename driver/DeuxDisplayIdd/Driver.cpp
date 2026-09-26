/*++

Copyright (c) Microsoft Corporation

Modifications Copyright (c) 2026 DeuxDisplay contributors.
Derived from the IddCx sample in microsoft/Windows-driver-samples (video/IndirectDisplay).
Licensed under the Microsoft Public License (MS-PL); see driver/LICENSE.

Abstract:

    Indirect display driver that exposes a single virtual monitor matching the DeuxDisplay
    client device (default: OnePlus Pad Go, 2408x1720 landscape).

Environment:

    User Mode, UMDF

--*/

#include "Driver.h"
#include "Edid.h"

using namespace std;
using namespace DeuxDisplay;
using namespace Microsoft::WRL;

#pragma region MonitorConfig

namespace
{

struct ModeSpec
{
    DWORD Width;
    DWORD Height;
    DWORD VSync;
};

// First entry is the preferred mode. Heights must stay even for 4:2:0 video encoding.
constexpr ModeSpec kModes[] = {
    {2408, 1720, 60},
    {2408, 1720, 90},
    {1204, 860, 60},
};

constexpr dd::edid::MonitorDescription kMonitor = {
    {2408, 1720, 60},
    {2408, 1720, 90},
    235, // mm, landscape
    168,
    1,
    "DeuxDisplay",
};

constexpr auto kEdid = dd::edid::BuildEdid(kMonitor);

// Stable so Windows remembers position/scaling of the virtual monitor across sessions.
// {7E1A5C3D-2B4F-4A6E-9C8D-0F1E2D3C4B5A}
constexpr GUID kMonitorContainerId = {0x7e1a5c3d, 0x2b4f, 0x4a6e, {0x9c, 0x8d, 0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a}};

} // namespace

#pragma endregion

#pragma region helpers

static inline void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& Mode, DWORD Width, DWORD Height, DWORD VSync,
                                  bool bMonitorMode)
{
    Mode.totalSize.cx = Mode.activeSize.cx = Width;
    Mode.totalSize.cy = Mode.activeSize.cy = Height;

    // See https://docs.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_video_signal_info
    Mode.AdditionalSignalInfo.vSyncFreqDivider = bMonitorMode ? 0 : 1;
    Mode.AdditionalSignalInfo.videoStandard = 255;

    Mode.vSyncFreq.Numerator = VSync;
    Mode.vSyncFreq.Denominator = 1;
    Mode.hSyncFreq.Numerator = VSync * Height;
    Mode.hSyncFreq.Denominator = 1;

    Mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;

    Mode.pixelRate = ((UINT64)VSync) * ((UINT64)Width) * ((UINT64)Height);
}

static IDDCX_MONITOR_MODE CreateIddCxMonitorMode(DWORD Width, DWORD Height, DWORD VSync,
                                                 IDDCX_MONITOR_MODE_ORIGIN Origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER)
{
    IDDCX_MONITOR_MODE Mode = {};

    Mode.Size = sizeof(Mode);
    Mode.Origin = Origin;
    FillSignalInfo(Mode.MonitorVideoSignalInfo, Width, Height, VSync, true);

    return Mode;
}

static IDDCX_TARGET_MODE CreateIddCxTargetMode(DWORD Width, DWORD Height, DWORD VSync)
{
    IDDCX_TARGET_MODE Mode = {};

    Mode.Size = sizeof(Mode);
    FillSignalInfo(Mode.TargetVideoSignalInfo.targetVideoSignalInfo, Width, Height, VSync, false);

    return Mode;
}

#pragma endregion

extern "C" DRIVER_INITIALIZE DriverEntry;

EVT_WDF_DRIVER_DEVICE_ADD DeuxDisplayDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY DeuxDisplayDeviceD0Entry;

EVT_IDD_CX_ADAPTER_INIT_FINISHED DeuxDisplayAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES DeuxDisplayAdapterCommitModes;

EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION DeuxDisplayParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES DeuxDisplayMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES DeuxDisplayMonitorQueryModes;

EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN DeuxDisplayMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN DeuxDisplayMonitorUnassignSwapChain;

struct IndirectDeviceContextWrapper
{
    IndirectDeviceContext* pContext;

    void Cleanup()
    {
        delete pContext;
        pContext = nullptr;
    }
};

struct IndirectMonitorContextWrapper
{
    IndirectMonitorContext* pContext;

    void Cleanup()
    {
        delete pContext;
        pContext = nullptr;
    }
};

WDF_DECLARE_CONTEXT_TYPE(IndirectDeviceContextWrapper);

WDF_DECLARE_CONTEXT_TYPE(IndirectMonitorContextWrapper);

extern "C" BOOL WINAPI DllMain(_In_ HINSTANCE hInstance, _In_ UINT dwReason, _In_opt_ LPVOID lpReserved)
{
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(lpReserved);
    UNREFERENCED_PARAMETER(dwReason);

    return TRUE;
}

_Use_decl_annotations_ extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT pDriverObject, PUNICODE_STRING pRegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);

    WDF_DRIVER_CONFIG_INIT(&Config, DeuxDisplayDeviceAdd);

    return WdfDriverCreate(pDriverObject, pRegistryPath, &Attributes, &Config, WDF_NO_HANDLE);
}

_Use_decl_annotations_ NTSTATUS DeuxDisplayDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT pDeviceInit)
{
    NTSTATUS Status = STATUS_SUCCESS;
    WDF_PNPPOWER_EVENT_CALLBACKS PnpPowerCallbacks;

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PnpPowerCallbacks);
    PnpPowerCallbacks.EvtDeviceD0Entry = DeuxDisplayDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &PnpPowerCallbacks);

    IDD_CX_CLIENT_CONFIG IddConfig;
    IDD_CX_CLIENT_CONFIG_INIT(&IddConfig);

    IddConfig.EvtIddCxAdapterInitFinished = DeuxDisplayAdapterInitFinished;

    IddConfig.EvtIddCxParseMonitorDescription = DeuxDisplayParseMonitorDescription;
    IddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = DeuxDisplayMonitorGetDefaultModes;
    IddConfig.EvtIddCxMonitorQueryTargetModes = DeuxDisplayMonitorQueryModes;
    IddConfig.EvtIddCxAdapterCommitModes = DeuxDisplayAdapterCommitModes;
    IddConfig.EvtIddCxMonitorAssignSwapChain = DeuxDisplayMonitorAssignSwapChain;
    IddConfig.EvtIddCxMonitorUnassignSwapChain = DeuxDisplayMonitorUnassignSwapChain;

    Status = IddCxDeviceInitConfig(pDeviceInit, &IddConfig);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);
    Attr.EvtCleanupCallback = [](WDFOBJECT Object) {
        auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Object);
        if (pContext)
        {
            pContext->Cleanup();
        }
    };

    WDFDEVICE Device = nullptr;
    Status = WdfDeviceCreate(&pDeviceInit, &Attr, &Device);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    Status = IddCxDeviceInitialize(Device);

    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    pContext->pContext = new IndirectDeviceContext(Device);

    return Status;
}

_Use_decl_annotations_ NTSTATUS DeuxDisplayDeviceD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    UNREFERENCED_PARAMETER(PreviousState);

    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    pContext->pContext->InitAdapter();

    return STATUS_SUCCESS;
}

#pragma region Direct3DDevice

Direct3DDevice::Direct3DDevice(LUID AdapterLuid) : AdapterLuid(AdapterLuid)
{
}

Direct3DDevice::Direct3DDevice()
{
    AdapterLuid = LUID{};
}

HRESULT Direct3DDevice::Init()
{
    // If a new render adapter appears on the system a new factory is needed, so it isn't cached.
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
    if (FAILED(hr))
    {
        return hr;
    }

    hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
    if (FAILED(hr))
    {
        return hr;
    }

    // BGRA support is required by the WHQL test suite.
    return D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                             nullptr, 0, D3D11_SDK_VERSION, &Device, nullptr, &DeviceContext);
}

#pragma endregion

#pragma region SwapChainProcessor

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, shared_ptr<Direct3DDevice> Device,
                                       HANDLE NewFrameEvent)
    : m_hSwapChain(hSwapChain), m_Device(Device), m_hAvailableBufferEvent(NewFrameEvent)
{
    m_hTerminateEvent.Attach(CreateEvent(nullptr, FALSE, FALSE, nullptr));
    m_hThread.Attach(CreateThread(nullptr, 0, RunThread, this, 0, nullptr));
}

SwapChainProcessor::~SwapChainProcessor()
{
    SetEvent(m_hTerminateEvent.Get());

    if (m_hThread.Get())
    {
        WaitForSingleObject(m_hThread.Get(), INFINITE);
    }
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID Argument)
{
    reinterpret_cast<SwapChainProcessor*>(Argument)->Run();
    return 0;
}

void SwapChainProcessor::Run()
{
    DWORD AvTask = 0;
    HANDLE AvTaskHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &AvTask);

    RunCore();

    // Deleting the swap-chain when the loop ends prompts the OS to provide a new one if needed.
    WdfObjectDelete((WDFOBJECT)m_hSwapChain);
    m_hSwapChain = nullptr;

    AvRevertMmThreadCharacteristics(AvTaskHandle);
}

void SwapChainProcessor::RunCore()
{
    ComPtr<IDXGIDevice> DxgiDevice;
    HRESULT hr = m_Device->Device.As(&DxgiDevice);
    if (FAILED(hr))
    {
        return;
    }

    IDARG_IN_SWAPCHAINSETDEVICE SetDevice = {};
    SetDevice.pDevice = DxgiDevice.Get();

    hr = IddCxSwapChainSetDevice(m_hSwapChain, &SetDevice);
    if (FAILED(hr))
    {
        return;
    }

    for (;;)
    {
        ComPtr<IDXGIResource> AcquiredBuffer;

        IDARG_OUT_RELEASEANDACQUIREBUFFER Buffer = {};
        hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &Buffer);

        if (hr == E_PENDING)
        {
            HANDLE WaitHandles[] = {m_hAvailableBufferEvent, m_hTerminateEvent.Get()};
            DWORD WaitResult = WaitForMultipleObjects(ARRAYSIZE(WaitHandles), WaitHandles, FALSE, 16);
            if (WaitResult == WAIT_OBJECT_0 || WaitResult == WAIT_TIMEOUT)
            {
                continue;
            }
            else if (WaitResult == WAIT_OBJECT_0 + 1)
            {
                break;
            }
            else
            {
                hr = HRESULT_FROM_WIN32(WaitResult);
                break;
            }
        }
        else if (SUCCEEDED(hr))
        {
            // The host captures this output with Desktop Duplication, so the driver only has to
            // release the surface promptly to keep the compositor flowing.
            AcquiredBuffer.Attach(Buffer.MetaData.pSurface);
            AcquiredBuffer.Reset();

            hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
            if (FAILED(hr))
            {
                break;
            }
        }
        else
        {
            // The swap-chain was likely abandoned (e.g. DXGI_ERROR_ACCESS_LOST).
            break;
        }
    }
}

#pragma endregion

#pragma region IndirectDeviceContext

IndirectDeviceContext::IndirectDeviceContext(_In_ WDFDEVICE WdfDevice) : m_WdfDevice(WdfDevice)
{
    m_Adapter = {};
}

IndirectDeviceContext::~IndirectDeviceContext()
{
}

void IndirectDeviceContext::InitAdapter()
{
    IDDCX_ADAPTER_CAPS AdapterCaps = {};
    AdapterCaps.Size = sizeof(AdapterCaps);

    AdapterCaps.MaxMonitorsSupported = 1;
    AdapterCaps.EndPointDiagnostics.Size = sizeof(AdapterCaps.EndPointDiagnostics);
    AdapterCaps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    AdapterCaps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;

    AdapterCaps.EndPointDiagnostics.pEndPointFriendlyName = L"DeuxDisplay";
    AdapterCaps.EndPointDiagnostics.pEndPointManufacturerName = L"DeuxDisplay";
    AdapterCaps.EndPointDiagnostics.pEndPointModelName = L"DeuxDisplay Virtual Monitor";

    IDDCX_ENDPOINT_VERSION Version = {};
    Version.Size = sizeof(Version);
    Version.MajorVer = 0;
    Version.MinorVer = 1;
    AdapterCaps.EndPointDiagnostics.pFirmwareVersion = &Version;
    AdapterCaps.EndPointDiagnostics.pHardwareVersion = &Version;

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectDeviceContextWrapper);

    IDARG_IN_ADAPTER_INIT AdapterInit = {};
    AdapterInit.WdfDevice = m_WdfDevice;
    AdapterInit.pCaps = &AdapterCaps;
    AdapterInit.ObjectAttributes = &Attr;

    IDARG_OUT_ADAPTER_INIT AdapterInitOut;
    NTSTATUS Status = IddCxAdapterInitAsync(&AdapterInit, &AdapterInitOut);

    if (NT_SUCCESS(Status))
    {
        m_Adapter = AdapterInitOut.AdapterObject;

        auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(AdapterInitOut.AdapterObject);
        pContext->pContext = this;
    }
}

void IndirectDeviceContext::FinishInit()
{
    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectMonitorContextWrapper);

    IDDCX_MONITOR_INFO MonitorInfo = {};
    MonitorInfo.Size = sizeof(MonitorInfo);
    MonitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL;
    MonitorInfo.ConnectorIndex = 0;

    MonitorInfo.MonitorDescription.Size = sizeof(MonitorInfo.MonitorDescription);
    MonitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    MonitorInfo.MonitorDescription.DataSize = static_cast<UINT>(kEdid.size());
    MonitorInfo.MonitorDescription.pData = const_cast<BYTE*>(kEdid.data());
    MonitorInfo.MonitorContainerId = kMonitorContainerId;

    IDARG_IN_MONITORCREATE MonitorCreate = {};
    MonitorCreate.ObjectAttributes = &Attr;
    MonitorCreate.pMonitorInfo = &MonitorInfo;

    IDARG_OUT_MONITORCREATE MonitorCreateOut;
    NTSTATUS Status = IddCxMonitorCreate(m_Adapter, &MonitorCreate, &MonitorCreateOut);
    if (NT_SUCCESS(Status))
    {
        auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorCreateOut.MonitorObject);
        pMonitorContextWrapper->pContext = new IndirectMonitorContext(MonitorCreateOut.MonitorObject);

        IDARG_OUT_MONITORARRIVAL ArrivalOut;
        IddCxMonitorArrival(MonitorCreateOut.MonitorObject, &ArrivalOut);
    }
}

IndirectMonitorContext::IndirectMonitorContext(_In_ IDDCX_MONITOR Monitor) : m_Monitor(Monitor)
{
}

IndirectMonitorContext::~IndirectMonitorContext()
{
    m_ProcessingThread.reset();
}

void IndirectMonitorContext::AssignSwapChain(IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent)
{
    m_ProcessingThread.reset();

    auto Device = make_shared<Direct3DDevice>(RenderAdapter);
    if (FAILED(Device->Init()))
    {
        // Deleting the swap-chain tells the OS to generate a new one and try again.
        WdfObjectDelete(SwapChain);
    }
    else
    {
        m_ProcessingThread.reset(new SwapChainProcessor(SwapChain, Device, NewFrameEvent));
    }
}

void IndirectMonitorContext::UnassignSwapChain()
{
    m_ProcessingThread.reset();
}

#pragma endregion

#pragma region DDI Callbacks

_Use_decl_annotations_ NTSTATUS DeuxDisplayAdapterInitFinished(IDDCX_ADAPTER AdapterObject,
                                                              const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs)
{
    auto* pDeviceContextWrapper = WdfObjectGet_IndirectDeviceContextWrapper(AdapterObject);
    if (NT_SUCCESS(pInArgs->AdapterInitStatus))
    {
        pDeviceContextWrapper->pContext->FinishInit();
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS DeuxDisplayAdapterCommitModes(IDDCX_ADAPTER AdapterObject,
                                                             const IDARG_IN_COMMITMODES* pInArgs)
{
    UNREFERENCED_PARAMETER(AdapterObject);
    UNREFERENCED_PARAMETER(pInArgs);

    // Nothing to reconfigure: there is no physical link, and IddCx manages the swap-chain.
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS DeuxDisplayParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION* pInArgs,
                                                                  IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs)
{
    pOutArgs->MonitorModeBufferOutputCount = ARRAYSIZE(kModes);

    if (pInArgs->MonitorModeBufferInputCount < ARRAYSIZE(kModes))
    {
        // No buffer means the caller is only asking for the count.
        return (pInArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
    }

    if (pInArgs->MonitorDescription.DataSize != kEdid.size() ||
        memcmp(pInArgs->MonitorDescription.pData, kEdid.data(), kEdid.size()) != 0)
    {
        return STATUS_INVALID_PARAMETER;
    }

    for (DWORD i = 0; i < ARRAYSIZE(kModes); i++)
    {
        pInArgs->pMonitorModes[i] = CreateIddCxMonitorMode(kModes[i].Width, kModes[i].Height, kModes[i].VSync,
                                                           IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR);
    }
    pOutArgs->PreferredMonitorModeIdx = 0;

    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS DeuxDisplayMonitorGetDefaultModes(IDDCX_MONITOR MonitorObject,
                                                                 const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs,
                                                                 IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs)
{
    UNREFERENCED_PARAMETER(MonitorObject);

    // Only called for EDID-less monitors, which this driver never creates, but kept correct anyway.
    pOutArgs->DefaultMonitorModeBufferOutputCount = ARRAYSIZE(kModes);
    if (pInArgs->DefaultMonitorModeBufferInputCount == 0)
    {
        return STATUS_SUCCESS;
    }
    if (pInArgs->DefaultMonitorModeBufferInputCount < ARRAYSIZE(kModes))
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    for (DWORD i = 0; i < ARRAYSIZE(kModes); i++)
    {
        pInArgs->pDefaultMonitorModes[i] =
            CreateIddCxMonitorMode(kModes[i].Width, kModes[i].Height, kModes[i].VSync, IDDCX_MONITOR_MODE_ORIGIN_DRIVER);
    }
    pOutArgs->PreferredMonitorModeIdx = 0;

    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS DeuxDisplayMonitorQueryModes(IDDCX_MONITOR MonitorObject,
                                                            const IDARG_IN_QUERYTARGETMODES* pInArgs,
                                                            IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
    UNREFERENCED_PARAMETER(MonitorObject);

    // The OS offers the intersection of monitor modes and these target modes.
    pOutArgs->TargetModeBufferOutputCount = ARRAYSIZE(kModes);

    if (pInArgs->TargetModeBufferInputCount >= ARRAYSIZE(kModes))
    {
        for (DWORD i = 0; i < ARRAYSIZE(kModes); i++)
        {
            pInArgs->pTargetModes[i] = CreateIddCxTargetMode(kModes[i].Width, kModes[i].Height, kModes[i].VSync);
        }
    }

    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS DeuxDisplayMonitorAssignSwapChain(IDDCX_MONITOR MonitorObject,
                                                                 const IDARG_IN_SETSWAPCHAIN* pInArgs)
{
    auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
    pMonitorContextWrapper->pContext->AssignSwapChain(pInArgs->hSwapChain, pInArgs->RenderAdapterLuid,
                                                      pInArgs->hNextSurfaceAvailable);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS DeuxDisplayMonitorUnassignSwapChain(IDDCX_MONITOR MonitorObject)
{
    auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject);
    pMonitorContextWrapper->pContext->UnassignSwapChain();
    return STATUS_SUCCESS;
}

#pragma endregion
