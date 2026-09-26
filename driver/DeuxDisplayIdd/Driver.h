/*++

Copyright (c) Microsoft Corporation

Modifications Copyright (c) 2026 DeuxDisplay contributors.
Derived from the IddCx sample in microsoft/Windows-driver-samples (video/IndirectDisplay).
Licensed under the Microsoft Public License (MS-PL); see driver/LICENSE.

--*/

#pragma once

#define NOMINMAX
#include <windows.h>
#include <bugcodes.h>
#include <wudfwdm.h>
#include <wdf.h>
#include <iddcx.h>

#include <dxgi1_5.h>
#include <d3d11_2.h>
#include <avrt.h>
#include <wrl.h>

#include <memory>
#include <mutex>
#include <vector>

#include "Public.h"

namespace Microsoft
{
namespace WRL
{
namespace Wrappers
{
// Adds a wrapper for thread handles to the existing set of WRL handle wrapper classes
typedef HandleT<HandleTraits::HANDLENullTraits> Thread;
} // namespace Wrappers
} // namespace WRL
} // namespace Microsoft

namespace DeuxDisplay
{

struct ModeSpec
{
    DWORD Width;
    DWORD Height;
    DWORD VSync;
};

/// Manages the creation and lifetime of a Direct3D render device.
struct Direct3DDevice
{
    Direct3DDevice(LUID AdapterLuid);
    Direct3DDevice();
    HRESULT Init();

    LUID AdapterLuid;
    Microsoft::WRL::ComPtr<IDXGIFactory5> DxgiFactory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> Adapter;
    Microsoft::WRL::ComPtr<ID3D11Device> Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> DeviceContext;
};

/// Manages a thread that consumes buffers from an indirect display swap-chain object.
/// Frames are only drained here; the host service captures them through Desktop Duplication.
class SwapChainProcessor
{
  public:
    SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, std::shared_ptr<Direct3DDevice> Device, HANDLE NewFrameEvent);
    ~SwapChainProcessor();

  private:
    static DWORD CALLBACK RunThread(LPVOID Argument);

    void Run();
    void RunCore();

    IDDCX_SWAPCHAIN m_hSwapChain;
    std::shared_ptr<Direct3DDevice> m_Device;
    HANDLE m_hAvailableBufferEvent;
    Microsoft::WRL::Wrappers::Thread m_hThread;
    Microsoft::WRL::Wrappers::Event m_hTerminateEvent;
};

class IndirectDeviceContext
{
  public:
    IndirectDeviceContext(_In_ WDFDEVICE WdfDevice);
    virtual ~IndirectDeviceContext();

    void InitAdapter();
    void OnAdapterReady();

    // Plugs the virtual monitor described by `Request`, replacing any existing one. `Owner` is the
    // file object of the host's handle; closing it unplugs the monitor.
    NTSTATUS Plug(const dd::driver::PlugRequest& Request, WDFFILEOBJECT Owner);
    void Unplug();
    void OnFileCleanup(WDFFILEOBJECT FileObject);

  protected:
    void UnplugLocked();

    WDFDEVICE m_WdfDevice;
    IDDCX_ADAPTER m_Adapter;
    std::mutex m_Lock;
    bool m_AdapterReady = false;
    IDDCX_MONITOR m_Monitor = nullptr;
    WDFFILEOBJECT m_Owner = nullptr;
};

class IndirectMonitorContext
{
  public:
    IndirectMonitorContext(_In_ IDDCX_MONITOR Monitor, std::vector<ModeSpec> Modes);
    virtual ~IndirectMonitorContext();

    void AssignSwapChain(IDDCX_SWAPCHAIN SwapChain, LUID RenderAdapter, HANDLE NewFrameEvent);
    void UnassignSwapChain();
    const std::vector<ModeSpec>& Modes() const { return m_Modes; }

  private:
    IDDCX_MONITOR m_Monitor;
    std::vector<ModeSpec> m_Modes;
    std::unique_ptr<SwapChainProcessor> m_ProcessingThread;
};

} // namespace DeuxDisplay
