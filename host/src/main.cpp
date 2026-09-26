#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "Server.h"
#include "capture/OutputLocator.h"
#include "display/VirtualDisplay.h"

#include <cstdlib>

#include <cstdio>
#include <cwchar>
#include <string_view>

using Microsoft::WRL::ComPtr;

namespace
{

int ListOutputs()
{
    ComPtr<IDXGIFactory6> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        std::fwprintf(stderr, L"CreateDXGIFactory2 failed: 0x%08lX\n", static_cast<unsigned long>(hr));
        return 1;
    }

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a)
    {
        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);
        std::wprintf(L"Adapter %u: %s%s\n", a, ad.Description,
                     (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? L" (software)" : L"");

        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o)
        {
            DXGI_OUTPUT_DESC od{};
            output->GetDesc(&od);
            const RECT& r = od.DesktopCoordinates;

            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            GetMonitorInfoW(od.Monitor, &mi);

            DISPLAY_DEVICEW monitor{};
            monitor.cb = sizeof(monitor);
            EnumDisplayDevicesW(od.DeviceName, 0, &monitor, 0);

            std::wprintf(L"  Output %u: %s  %ldx%ld at (%ld,%ld)%s  monitor=\"%s\"  id=%s\n", o, od.DeviceName,
                         r.right - r.left, r.bottom - r.top, r.left, r.top,
                         (mi.dwFlags & MONITORINFOF_PRIMARY) ? L" [primary]" : L"", monitor.DeviceString,
                         dd::capture::MonitorDeviceId(od.DeviceName).c_str());
            output.Reset();
        }
        adapter.Reset();
    }
    return 0;
}

int CreateDisplay()
{
    dd::VirtualDisplay display;
    HRESULT hr = display.Create();
    if (FAILED(hr))
    {
        std::fwprintf(stderr,
                      L"Creating the virtual display failed: 0x%08lX\n"
                      L"Is the driver installed? See driver/README.md.\n",
                      static_cast<unsigned long>(hr));
        return 1;
    }
    std::wprintf(L"Virtual display attached. Press Enter to remove it.\n");
    (void)std::getwchar();
    return 0;
}

void PrintUsage()
{
    std::wprintf(L"DeuxDisplayHost\n\n"
                 L"Usage:\n"
                 L"  DeuxDisplayHost --serve [--port N] [--bitrate KBPS] [--no-create-display]\n"
                 L"      Attach the virtual monitor and stream it to a client on 127.0.0.1:N (default 27183)\n"
                 L"  DeuxDisplayHost --list-outputs     List DXGI adapters and outputs\n"
                 L"  DeuxDisplayHost --create-display   Attach the virtual monitor until Enter is pressed\n");
}

bool ParseServeOptions(int argc, wchar_t** argv, dd::ServeOptions& options)
{
    for (int i = 2; i < argc; ++i)
    {
        const std::wstring_view arg = argv[i];
        if (arg == L"--port" && i + 1 < argc)
        {
            options.port = static_cast<uint16_t>(std::wcstoul(argv[++i], nullptr, 10));
        }
        else if (arg == L"--bitrate" && i + 1 < argc)
        {
            options.bitrateKbps = std::wcstoul(argv[++i], nullptr, 10);
        }
        else if (arg == L"--no-create-display")
        {
            options.createDisplay = false;
        }
        else
        {
            return false;
        }
    }
    return options.port != 0 && options.bitrateKbps >= 500;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
    {
        return 1;
    }

    const std::wstring_view command = argc >= 2 ? argv[1] : L"";
    if (command == L"--list-outputs")
    {
        return ListOutputs();
    }
    if (command == L"--create-display")
    {
        return CreateDisplay();
    }
    if (command == L"--serve")
    {
        dd::ServeOptions options;
        if (!ParseServeOptions(argc, argv, options))
        {
            PrintUsage();
            return 1;
        }
        return dd::Serve(options);
    }
    PrintUsage();
    return argc >= 2 ? 1 : 0;
}
