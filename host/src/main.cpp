#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "Server.h"
#include "capture/OutputLocator.h"
#include "display/VirtualDisplay.h"
#include "protocol/Pairing.h"
#include "wireless/PairingStore.h"

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

int CreateDisplay(unsigned seconds)
{
    dd::VirtualDisplay display;
    HRESULT hr = display.Plug(dd::DefaultPlugRequest());
    if (FAILED(hr))
    {
        std::fwprintf(stderr,
                      L"Plugging the virtual display failed: 0x%08lX\n"
                      L"Is the driver installed? See driver/README.md.\n",
                      static_cast<unsigned long>(hr));
        return 1;
    }
    if (!dd::WaitForExtendedDisplay(8000))
    {
        std::fwprintf(stderr, L"The monitor was plugged but didn't become an extended display.\n");
    }
    if (seconds > 0)
    {
        std::wprintf(L"Virtual display plugged (2408x1720) for %u s.\n", seconds);
        std::fflush(stdout);
        Sleep(seconds * 1000);
    }
    else
    {
        std::wprintf(L"Virtual display plugged (2408x1720). Press Enter to unplug it.\n");
        (void)std::getwchar();
    }
    return 0;
}

int DeviceCommand(bool install)
{
    const HRESULT hr = install ? dd::InstallDevice() : dd::RemoveDevice();
    if (FAILED(hr))
    {
        std::fwprintf(stderr, L"%s failed: 0x%08lX%s\n", install ? L"--install-device" : L"--remove-device",
                      static_cast<unsigned long>(hr),
                      hr == E_ACCESSDENIED ? L" (run from an elevated prompt)" : L"");
        return 1;
    }
    std::wprintf(L"%s\n", install ? L"DeuxDisplay device installed." : L"DeuxDisplay device removed.");
    return 0;
}

int PairCommand(bool reset)
{
    const auto code = dd::wireless::LoadOrCreatePairingCode(reset);
    const auto secrets = code ? dd::protocol::DerivePairingSecrets(*code) : std::nullopt;
    if (!secrets)
    {
        std::fwprintf(stderr, L"Couldn't load or create the pairing code.\n");
        return 1;
    }
    std::wprintf(L"Pairing code: %S\n"
                 L"Wi-Fi network: %S (started by --serve)\n\n"
                 L"Tablets pair automatically over USB. Otherwise, open DeuxDisplay on the tablet, press Back,\n"
                 L"pick Connection > Wi-Fi and enter this code. Anyone with the code can connect: keep it private,\n"
                 L"and run --pair --reset to replace it (unpairs every tablet).\n",
                 dd::protocol::FormatPairingCode(*code).c_str(), secrets->ssid.c_str());
    return 0;
}

void PrintUsage()
{
    std::wprintf(L"DeuxDisplayHost\n\n"
                 L"Usage:\n"
                 L"  DeuxDisplayHost --serve [--port N] [--bitrate KBPS] [--max-fps N] [--max-stream-size WxH]\n"
                 L"                          [--codec auto|h264|hevc] [--repeat-frames] [--no-touch]\n"
                 L"                          [--transport both|usb|wifi] [--output \\\\.\\DISPLAYn]\n"
                 L"      Stream to a client, plugging a virtual monitor that matches the client. Clients connect\n"
                 L"      over USB (127.0.0.1:N through adb reverse, default port 27183) or over Wi-Fi (a private\n"
                 L"      network this PC starts; paired tablets only). --output streams an existing monitor\n"
                 L"      instead (debugging).\n"
                 L"      A resolution/frame rate picked in the tablet app overrides --max-stream-size/--max-fps.\n"
                 L"      Touches on the tablet are injected on the display unless --no-touch is given.\n"
                 L"  DeuxDisplayHost --create-display [SECONDS]\n"
                 L"      Plug a 2408x1720 virtual monitor until Enter is pressed (or for SECONDS)\n"
                 L"  DeuxDisplayHost --pair [--reset]   Show (or replace) the Wi-Fi pairing code\n"
                 L"  DeuxDisplayHost --list-outputs     List DXGI adapters and outputs\n"
                 L"  DeuxDisplayHost --install-device   (admin, once) create the persistent virtual display device\n"
                 L"  DeuxDisplayHost --remove-device    (admin) remove it\n");
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
        else if (arg == L"--max-stream-size" && i + 1 < argc)
        {
            wchar_t* end = nullptr;
            options.maxStreamWidth = std::wcstoul(argv[++i], &end, 10);
            options.maxStreamHeight = (end && (*end == L'x' || *end == L'X')) ? std::wcstoul(end + 1, nullptr, 10) : 0;
            if (options.maxStreamWidth < 320 || options.maxStreamHeight < 240)
            {
                return false;
            }
        }
        else if (arg == L"--codec" && i + 1 < argc)
        {
            options.codec = argv[++i];
            if (options.codec != L"auto" && options.codec != L"h264" && options.codec != L"hevc")
            {
                return false;
            }
        }
        else if (arg == L"--repeat-frames")
        {
            options.repeatFrames = true;
        }
        else if (arg == L"--no-touch")
        {
            options.touchInput = false;
        }
        else if (arg == L"--max-fps" && i + 1 < argc)
        {
            options.maxFps = std::wcstoul(argv[++i], nullptr, 10);
        }
        else if (arg == L"--transport" && i + 1 < argc)
        {
            const std::wstring_view transport = argv[++i];
            options.usb = transport == L"both" || transport == L"usb";
            options.wifi = transport == L"both" || transport == L"wifi";
            if (!options.usb && !options.wifi)
            {
                return false;
            }
        }
        else if (arg == L"--output" && i + 1 < argc)
        {
            options.outputName = argv[++i];
        }
        else
        {
            return false;
        }
    }
    return options.port != 0 && options.bitrateKbps >= 500 && options.maxFps >= 1 && options.maxFps <= 240;
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
        return CreateDisplay(argc >= 3 ? std::wcstoul(argv[2], nullptr, 10) : 0);
    }
    if (command == L"--install-device" || command == L"--remove-device")
    {
        return DeviceCommand(command == L"--install-device");
    }
    if (command == L"--pair")
    {
        return PairCommand(argc >= 3 && std::wstring_view(argv[2]) == L"--reset");
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
