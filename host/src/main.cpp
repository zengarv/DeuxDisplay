#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

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

            DISPLAY_DEVICEW dd{};
            dd.cb = sizeof(dd);
            EnumDisplayDevicesW(od.DeviceName, 0, &dd, 0);

            std::wprintf(L"  Output %u: %s  %ldx%ld at (%ld,%ld)%s  monitor=\"%s\"\n", o, od.DeviceName,
                         r.right - r.left, r.bottom - r.top, r.left, r.top,
                         (mi.dwFlags & MONITORINFOF_PRIMARY) ? L" [primary]" : L"", dd.DeviceString);
            output.Reset();
        }
        adapter.Reset();
    }
    return 0;
}

void PrintUsage()
{
    std::wprintf(L"DeuxDisplayHost\n\n"
                 L"Usage:\n"
                 L"  DeuxDisplayHost --list-outputs   List DXGI adapters and outputs\n");
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (argc >= 2 && std::wstring_view(argv[1]) == L"--list-outputs")
    {
        return ListOutputs();
    }
    PrintUsage();
    return argc >= 2 ? 1 : 0;
}
