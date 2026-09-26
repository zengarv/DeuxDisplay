# DeuxDisplayIdd: virtual monitor driver

An IddCx (Indirect Display Driver) that gives Windows one extra monitor matching the DeuxDisplay
client device. It is a **user-mode (UMDF 2)** driver derived from Microsoft's IddCx sample (see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)). Licensed under **MS-PL** ([LICENSE](LICENSE)),
except `Edid.h` which is MIT.

The monitor only exists while `DeuxDisplayHost` holds it: the host creates a software device
(`SwDeviceCreate`) with hardware ID `DeuxDisplayIdd`, and Windows loads this driver for it.
Closing the host removes the monitor, and Windows moves the windows back.

## Default monitor
| | |
|---|---|
| Modes | 2408×1720 @ 60 Hz (preferred), 2408×1720 @ 90 Hz, 1204×860 @ 60 Hz |
| Physical size | 235 × 168 mm (so Windows picks sensible scaling) |
| EDID | generated at compile time by `Edid.h`, vendor `DXD`, name `DeuxDisplay` |

Values come from the OnePlus Pad Go, see [docs/devices.md](../docs/devices.md).

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
From an **elevated** PowerShell:
```powershell
.\scripts\install-driver.ps1
.\host\x64\Release\DeuxDisplayHost.exe --create-display   # monitor appears; Enter removes it
```

Because this is a user-mode driver, **test-signing mode is not required** (it's often blocked by
Secure Boot anyway). `install-driver.ps1` creates a self-signed certificate with a non-exportable
key, trusts it on this machine only, signs the package catalog, and adds the package with
`pnputil`.

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
