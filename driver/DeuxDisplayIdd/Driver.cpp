/*++

Copyright (c) Microsoft Corporation

Modifications Copyright (c) 2026 DeuxDisplay contributors.
Derived from the IddCx sample in microsoft/Windows-driver-samples (video/IndirectDisplay).
Licensed under the Microsoft Public License (MS-PL); see driver/LICENSE.

Abstract:

    Indirect display driver that exposes up to four virtual monitors, one per DeuxDisplay
    client device. Monitors are plugged/unplugged at runtime by the host through a device
    interface (see Public.h); their EDIDs and mode lists are generated from the host's requests.

Environment:

    User Mode, UMDF

--*/

#include "Driver.h"
#include "Edid.h"

#include <cstdio>

using namespace std;
using namespace DeuxDisplay;
using namespace Microsoft::WRL;

#pragma region MonitorConfig

namespace
{

// Stable so Windows remembers position/scaling of each virtual monitor across sessions.
// {7E1A5C3D-2B4F-4A6E-9C8D-0F1E2D3C4B5A} for connector 0; the last byte grows with the index.
constexpr GUID kMonitorContainerId = {0x7e1a5c3d, 0x2b4f, 0x4a6e, {0x9c, 0x8d, 0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a}};

GUID ContainerIdFor(UINT Index)
{
    GUID Id = kMonitorContainerId;
    Id.Data4[7] = static_cast<unsigned char>(Id.Data4[7] + Index);
    return Id;
}

constexpr uint64_t kMaxDtdPixelClockHz = 655'350'000; // 16-bit field in 10 kHz units

// EDID + modes of each plugged monitor, by connector index. IddCx's ParseMonitorDescription
// callback gets only the EDID bytes (no monitor object), so the mode list is looked up here.
// The EDID's product code is the index + 1, so the bytes identify the monitor.
struct CurrentMonitor
{
    std::array<uint8_t, dd::edid::kEdidSize> Edid{};
    std::vector<ModeSpec> Modes;
};
std::mutex g_CurrentLock;
std::array<CurrentMonitor, dd::driver::kMaxMonitors> g_Current;

// Requested rates at native size (first = preferred), plus half size at 60 Hz as a
// low-bandwidth fallback. Heights stay even for 4:2:0 video encoding.
std::vector<ModeSpec> BuildModes(const dd::driver::PlugRequest& r)
{
    std::vector<ModeSpec> modes;
    for (uint16_t hz : r.refreshHz)
    {
        if (hz != 0)
        {
            modes.push_back({r.width, r.height, hz});
        }
    }
    const DWORD halfW = (r.width / 2) & ~1u;
    const DWORD halfH = (r.height / 2) & ~1u;
    if (halfW >= 640 && halfH >= 480)
    {
        modes.push_back({halfW, halfH, 60});
    }
    return modes;
}

bool FitsInDtd(DWORD width, DWORD height, DWORD hz)
{
    return uint64_t{width + dd::edid::kHBlank} * (height + dd::edid::kVBlank) * hz <= kMaxDtdPixelClockHz;
}

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

EVT_IDD_CX_DEVICE_IO_CONTROL DeuxDisplayDeviceIoControl;
EVT_WDF_FILE_CLEANUP DeuxDisplayFileCleanup;

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

    // IddCx routes IoDeviceControl to its own queue; this callback is how IOCTLs reach the driver.
    IddConfig.EvtIddCxDeviceIoControl = DeuxDisplayDeviceIoControl;
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

    // Cleanup fires when a handle to the device is closed (including when its process dies),
    // which lets us unplug the monitor if the host goes away without saying so.
    WDF_FILEOBJECT_CONFIG FileConfig;
    WDF_FILEOBJECT_CONFIG_INIT(&FileConfig, WDF_NO_EVENT_CALLBACK, WDF_NO_EVENT_CALLBACK, DeuxDisplayFileCleanup);
    WdfDeviceInitSetFileObjectConfig(pDeviceInit, &FileConfig, WDF_NO_OBJECT_ATTRIBUTES);

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
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device);
    pContext->pContext = new IndirectDeviceContext(Device);

    return WdfDeviceCreateDeviceInterface(Device, &dd::driver::kDeviceInterfaceGuid, nullptr);
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

    AdapterCaps.MaxMonitorsSupported = dd::driver::kMaxMonitors;
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

void IndirectDeviceContext::OnAdapterReady()
{
    std::lock_guard Lock(m_Lock);
    m_AdapterReady = true;
}

NTSTATUS IndirectDeviceContext::Plug(const dd::driver::PlugRequest& Request, WDFFILEOBJECT Owner, UINT& Index)
{
    if (!dd::driver::IsValidPlugRequest(Request) || !FitsInDtd(Request.width, Request.height, Request.refreshHz[0]))
    {
        return STATUS_INVALID_PARAMETER;
    }

    std::lock_guard Lock(m_Lock);
    if (!m_AdapterReady)
    {
        return STATUS_DEVICE_NOT_READY;
    }

    // The caller's own slot if it has one (a re-plug keeps its index), else the lowest free one,
    // so a lone client always gets index 0 and the same monitor identity as before.
    MonitorSlot* Slot = nullptr;
    for (auto& Candidate : m_Slots)
    {
        if (Candidate.Monitor && Candidate.Owner == Owner)
        {
            Slot = &Candidate;
            break;
        }
    }
    for (auto& Candidate : m_Slots)
    {
        if (!Slot && !Candidate.Monitor)
        {
            Slot = &Candidate;
        }
    }
    if (!Slot)
    {
        return STATUS_TOO_MANY_NODES;
    }
    UnplugLocked(*Slot);
    Index = static_cast<UINT>(Slot - m_Slots.data());

    // EDID: preferred timing plus one alternate refresh rate (the full list is in the mode list).
    dd::edid::MonitorDescription Description;
    Description.preferred = {Request.width, Request.height, Request.refreshHz[0]};
    if (Request.refreshHz[1] != 0 && FitsInDtd(Request.width, Request.height, Request.refreshHz[1]))
    {
        Description.secondary = {Request.width, Request.height, Request.refreshHz[1]};
    }
    Description.widthMm = Request.widthMm;
    Description.heightMm = Request.heightMm;
    Description.product = static_cast<uint16_t>(Index + 1);
    char Name[16] = "DeuxDisplay";
    if (Index > 0)
    {
        sprintf_s(Name, "DeuxDisplay %u", Index + 1);
    }
    Description.name = Name;

    auto Modes = BuildModes(Request);
    const auto Edid = dd::edid::BuildEdid(Description);
    {
        std::lock_guard CurrentLock(g_CurrentLock);
        g_Current[Index].Edid = Edid;
        g_Current[Index].Modes = Modes;
    }

    WDF_OBJECT_ATTRIBUTES Attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attr, IndirectMonitorContextWrapper);
    Attr.EvtCleanupCallback = [](WDFOBJECT Object) {
        auto* pContext = WdfObjectGet_IndirectMonitorContextWrapper(Object);
        if (pContext)
        {
            pContext->Cleanup();
        }
    };

    IDDCX_MONITOR_INFO MonitorInfo = {};
    MonitorInfo.Size = sizeof(MonitorInfo);
    MonitorInfo.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL;
    MonitorInfo.ConnectorIndex = Index;
    MonitorInfo.MonitorDescription.Size = sizeof(MonitorInfo.MonitorDescription);
    MonitorInfo.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
    MonitorInfo.MonitorDescription.DataSize = static_cast<UINT>(Edid.size());
    MonitorInfo.MonitorDescription.pData = const_cast<uint8_t*>(Edid.data());
    MonitorInfo.MonitorContainerId = ContainerIdFor(Index);

    IDARG_IN_MONITORCREATE MonitorCreate = {};
    MonitorCreate.ObjectAttributes = &Attr;
    MonitorCreate.pMonitorInfo = &MonitorInfo;

    IDARG_OUT_MONITORCREATE MonitorCreateOut;
    NTSTATUS Status = IddCxMonitorCreate(m_Adapter, &MonitorCreate, &MonitorCreateOut);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    auto* pMonitorContextWrapper = WdfObjectGet_IndirectMonitorContextWrapper(MonitorCreateOut.MonitorObject);
    pMonitorContextWrapper->pContext = new IndirectMonitorContext(MonitorCreateOut.MonitorObject, std::move(Modes));

    IDARG_OUT_MONITORARRIVAL ArrivalOut;
    Status = IddCxMonitorArrival(MonitorCreateOut.MonitorObject, &ArrivalOut);
    if (!NT_SUCCESS(Status))
    {
        WdfObjectDelete(MonitorCreateOut.MonitorObject);
        return Status;
    }

    Slot->Monitor = MonitorCreateOut.MonitorObject;
    Slot->Owner = Owner;
    return STATUS_SUCCESS;
}

void IndirectDeviceContext::UnplugLocked(MonitorSlot& Slot)
{
    if (Slot.Monitor)
    {
        // IddCx deletes the monitor object after departure.
        IddCxMonitorDeparture(Slot.Monitor);
        Slot.Monitor = nullptr;
    }
    Slot.Owner = nullptr;
}

void IndirectDeviceContext::Unplug(WDFFILEOBJECT Owner)
{
    std::lock_guard Lock(m_Lock);
    for (auto& Slot : m_Slots)
    {
        if (Slot.Monitor && Slot.Owner == Owner)
        {
            UnplugLocked(Slot);
        }
    }
}

void IndirectDeviceContext::OnFileCleanup(WDFFILEOBJECT FileObject)
{
    Unplug(FileObject);
}

IndirectMonitorContext::IndirectMonitorContext(_In_ IDDCX_MONITOR Monitor, std::vector<ModeSpec> Modes)
    : m_Monitor(Monitor), m_Modes(std::move(Modes))
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
    // No monitor yet: the host plugs one via IOCTL when a client connects.
    auto* pDeviceContextWrapper = WdfObjectGet_IndirectDeviceContextWrapper(AdapterObject);
    if (NT_SUCCESS(pInArgs->AdapterInitStatus))
    {
        pDeviceContextWrapper->pContext->OnAdapterReady();
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
    const auto* Edid = static_cast<const uint8_t*>(pInArgs->MonitorDescription.pData);
    const size_t EdidSize = pInArgs->MonitorDescription.DataSize;

    std::vector<ModeSpec> Modes;
    {
        std::lock_guard Lock(g_CurrentLock);
        for (const auto& Current : g_Current)
        {
            if (EdidSize == Current.Edid.size() && memcmp(Edid, Current.Edid.data(), EdidSize) == 0)
            {
                Modes = Current.Modes;
                break;
            }
        }
    }
    if (Modes.empty())
    {
        // Not a monitor we plugged (shouldn't happen): fall back to the EDID's own timings.
        for (const auto& t : dd::edid::DetailedTimings(Edid, EdidSize))
        {
            Modes.push_back({t.width, t.height, t.refreshHz});
        }
    }
    if (Modes.empty())
    {
        return STATUS_INVALID_PARAMETER;
    }

    pOutArgs->MonitorModeBufferOutputCount = static_cast<UINT>(Modes.size());
    if (pInArgs->MonitorModeBufferInputCount < Modes.size())
    {
        // No buffer means the caller is only asking for the count.
        return (pInArgs->MonitorModeBufferInputCount > 0) ? STATUS_BUFFER_TOO_SMALL : STATUS_SUCCESS;
    }

    for (size_t i = 0; i < Modes.size(); i++)
    {
        pInArgs->pMonitorModes[i] = CreateIddCxMonitorMode(Modes[i].Width, Modes[i].Height, Modes[i].VSync,
                                                           IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR);
    }
    pOutArgs->PreferredMonitorModeIdx = 0;

    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS DeuxDisplayMonitorGetDefaultModes(IDDCX_MONITOR MonitorObject,
                                                                 const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs,
                                                                 IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs)
{
    // Only called for EDID-less monitors, which this driver never creates, but kept correct anyway.
    const auto& Modes = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject)->pContext->Modes();

    pOutArgs->DefaultMonitorModeBufferOutputCount = static_cast<UINT>(Modes.size());
    if (pInArgs->DefaultMonitorModeBufferInputCount == 0)
    {
        return STATUS_SUCCESS;
    }
    if (pInArgs->DefaultMonitorModeBufferInputCount < Modes.size())
    {
        return STATUS_BUFFER_TOO_SMALL;
    }

    for (size_t i = 0; i < Modes.size(); i++)
    {
        pInArgs->pDefaultMonitorModes[i] =
            CreateIddCxMonitorMode(Modes[i].Width, Modes[i].Height, Modes[i].VSync, IDDCX_MONITOR_MODE_ORIGIN_DRIVER);
    }
    pOutArgs->PreferredMonitorModeIdx = 0;

    return STATUS_SUCCESS;
}

_Use_decl_annotations_ NTSTATUS DeuxDisplayMonitorQueryModes(IDDCX_MONITOR MonitorObject,
                                                            const IDARG_IN_QUERYTARGETMODES* pInArgs,
                                                            IDARG_OUT_QUERYTARGETMODES* pOutArgs)
{
    const auto& Modes = WdfObjectGet_IndirectMonitorContextWrapper(MonitorObject)->pContext->Modes();

    // The OS offers the intersection of monitor modes and these target modes.
    pOutArgs->TargetModeBufferOutputCount = static_cast<UINT>(Modes.size());

    if (pInArgs->TargetModeBufferInputCount >= Modes.size())
    {
        for (size_t i = 0; i < Modes.size(); i++)
        {
            pInArgs->pTargetModes[i] = CreateIddCxTargetMode(Modes[i].Width, Modes[i].Height, Modes[i].VSync);
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

_Use_decl_annotations_ VOID DeuxDisplayDeviceIoControl(WDFDEVICE Device, WDFREQUEST Request, size_t OutputBufferLength,
                                                      size_t InputBufferLength, ULONG IoControlCode)
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    auto* pContext = WdfObjectGet_IndirectDeviceContextWrapper(Device)->pContext;
    NTSTATUS Status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR Information = 0;

    switch (IoControlCode)
    {
    case dd::driver::kIoctlPlug: {
        PVOID Buffer = nullptr;
        Status = WdfRequestRetrieveInputBuffer(Request, sizeof(dd::driver::PlugRequest), &Buffer, nullptr);
        if (NT_SUCCESS(Status))
        {
            dd::driver::PlugRequest Plug;
            memcpy(&Plug, Buffer, sizeof(Plug));
            UINT Index = 0;
            Status = pContext->Plug(Plug, WdfRequestGetFileObject(Request), Index);
            // The result is optional: a host that passes no output buffer assumes index 0.
            PVOID Out = nullptr;
            if (NT_SUCCESS(Status) &&
                NT_SUCCESS(WdfRequestRetrieveOutputBuffer(Request, sizeof(dd::driver::PlugResult), &Out, nullptr)))
            {
                dd::driver::PlugResult Result;
                Result.index = Index;
                memcpy(Out, &Result, sizeof(Result));
                Information = sizeof(Result);
            }
        }
        break;
    }
    case dd::driver::kIoctlUnplug:
        pContext->Unplug(WdfRequestGetFileObject(Request));
        Status = STATUS_SUCCESS;
        break;
    default:
        break;
    }

    WdfRequestCompleteWithInformation(Request, Status, Information);
}

_Use_decl_annotations_ VOID DeuxDisplayFileCleanup(WDFFILEOBJECT FileObject)
{
    auto* pWrapper = WdfObjectGet_IndirectDeviceContextWrapper(WdfFileObjectGetDevice(FileObject));
    if (pWrapper && pWrapper->pContext)
    {
        pWrapper->pContext->OnFileCleanup(FileObject);
    }
}

#pragma endregion
