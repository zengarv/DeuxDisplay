# DeuxDisplayIdd: virtual monitor driver

An IddCx (Indirect Display Driver) that gives Windows one extra monitor matching the DeuxDisplay
client device. It is a **user-mode (UMDF 2)** driver derived from Microsoft's IddCx sample (see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). Licensed under **MS-PL** ([LICENSE](LICENSE)),
except `Edid.h` which is MIT.

## How it's used
- **Install (admin, once):** the driver package plus a persistent software device
  (`SWD\DeuxDisplayIdd\DeuxDisplayIdd`). The adapter exists, but no monitor is attached.
- **Runtime (no admin):** `DeuxDisplayHost` opens the device interface declared in
  [`Public.h`](DeuxDisplayIdd/Public.h) and sends `PLUG` with the client's width, height, refresh
  rates and physical size. The driver builds an EDID (`Edid.h`, vendor `DXD`, product `0001`,
  name `DeuxDisplay`) and mode list from that and plugs the monitor. `UNPLUG` removes it. If the
  host's handle closes (including a crash), the driver unplugs it automatically.
- Modes: every requested refresh rate at native size, plus half size @ 60 Hz as a fallback.
- The monitor container ID is fixed, so Windows remembers its arrangement and scaling.

`DeuxDisplayHost --create-display [SECONDS]` plugs a OnePlus Pad Go-sized monitor (2408×1720,
60/90 Hz, 235×168 mm) for testing without a tablet.

## Build
Prerequisites: VS 2022 17.14+ (or Build Tools) with the C++ workload **and the "Windows Driver
Kit" component** (`Component.Microsoft.Windows.DriverKit.BuildTools` for Build Tools). That
component provides the `WindowsUserModeDriver10.0` toolset. The WDK headers, libraries and tools
themselves are restored from NuGet (`DeuxDisplayIdd/packages.config`), so the standalone WDK
installer isn't needed.

Use the **64-bit MSBuild** (`MSBuild\Current\Bin\amd64\MSBuild.exe`). The NuGet WDK's INF
verifier is 64-bit only, and 32-bit MSBuild fails with `Unable to load DLL 'x86\InfVerif.dll'`.
```powershell
msbuild driver\DeuxDisplayIdd.sln /t:restore /p:RestorePackagesConfig=true
msbuild driver\DeuxDisplayIdd.sln /p:Configuration=Release /p:Platform=x64
```
Output: `driver\build\x64\Release\DeuxDisplayIdd\` (INF + DLL + unsigned catalog).

The WDK NuGet version (`driver/Directory.Build.props`) must match the Visual Studio generation:
10.0.26100.x for VS 2022 (17.x). 10.0.28000.x ships build tasks for VS 2026 (18.x) only.

## Install (development)
Build the driver **and** the host first (the script uses the host to create the device). Then,
from an **elevated** PowerShell:
```powershell
.\scripts\install-driver.ps1
```
Then, as a normal user:
```powershell
.\host\x64\Release\DeuxDisplayHost.exe --create-display   # monitor appears; Enter removes it
```

Because this is a user-mode driver, **test-signing mode is not required** (it's often blocked by
Secure Boot anyway). `install-driver.ps1` creates a self-signed certificate with a non-exportable
key, trusts it on this machine only, signs the package catalog, adds the package with `pnputil`,
and runs `DeuxDisplayHost --install-device`. Re-running it updates an existing install in place.

The INF grants interactive users read/write on the device object. That's what lets the host
plug/unplug the monitor without admin rights.

Remove everything:
```powershell
.\scripts\uninstall-driver.ps1 -RemoveCertificate
```

## Troubleshooting
- Device Manager → *Display adapters* → *DeuxDisplay Virtual Monitor*: check the device status.
- `pnputil /enum-drivers` lists the installed package (`deuxdisplayidd.inf`).
- The driver runs in its own `WUDFHost.exe` (device group `DeuxDisplayIddGroup`). Attach a
  debugger there.
- Event Viewer → *Applications and Services Logs → Microsoft → Windows → DriverFrameworks-UserMode*.
