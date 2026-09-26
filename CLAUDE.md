# CLAUDE.md

Guidance for Claude Code (and humans) working in this repo.

## What this is
DeuxDisplay makes an Android tablet an extended Windows 11 monitor over USB with minimal latency.
Three parts, one repo:

- `driver/` — IddCx virtual monitor driver (C++, UMDF 2, WDK via NuGet). **MS-PL** (derived from
  Microsoft's sample), unlike the rest of the repo (MIT). `Edid.h` is original MIT code.
- `host/` — `DeuxDisplayHost.exe`: Desktop Duplication → Media Foundation H.264 → loopback TCP (C++20)
- `android/` — Kotlin client: TCP → MediaCodec → SurfaceView

`PLAN.md` holds milestones and status; tick checkboxes there when finishing work.
`docs/wire-protocol.md` is the source of truth for the protocol. Host (`host/src/protocol/`) and
Android (`android/.../protocol/`) code must match it, so update the doc in the same commit as any
protocol change.

## Build & run
Toolchain: VS 2022 17.14+ (or Build Tools) with C++ workload, plus the Windows Driver Kit VS
component for the driver. JDK 17, Android SDK and adb come from `scripts/bootstrap-dev.ps1`,
which installs to `%LOCALAPPDATA%\DeuxDisplay\tools`.

```powershell
# Host (+ protocol and EDID tests)
msbuild DeuxDisplay.sln /p:Configuration=Release /p:Platform=x64 /m /nodeReuse:false
host\x64\Release\DeuxDisplayHostTests.exe
host\x64\Release\DeuxDisplayHost.exe --list-outputs
host\x64\Release\DeuxDisplayHost.exe --create-display   # needs the driver installed

# Driver (WDK from NuGet), then install from an elevated shell
msbuild driver\DeuxDisplayIdd.sln /t:restore /p:RestorePackagesConfig=true
msbuild driver\DeuxDisplayIdd.sln /p:Configuration=Release /p:Platform=x64 /nodeReuse:false
scripts\install-driver.ps1

# Android
$env:JAVA_HOME = "$env:LOCALAPPDATA\DeuxDisplay\tools\jdk17"
$env:ANDROID_HOME = "$env:LOCALAPPDATA\DeuxDisplay\tools\android-sdk"
cd android; .\gradlew.bat assembleDebug lint test
```

MSBuild isn't on PATH by default. Use a VS Developer PowerShell, or locate it with
`vswhere -latest -find MSBuild\**\Bin\amd64\MSBuild.exe`. Use the **64-bit** MSBuild: the
driver's NuGet WDK has 64-bit-only InfVerif. Pass `/nodeReuse:false`, because lingering MSBuild
nodes block Visual Studio installer updates.

## Rules that matter
- **Latency is the product.** Don't add buffering, queues, frame pacing, or extra copies in
  the frame path without measuring the effect. Log numbers in `docs/latency-notes.md`.
- Sockets bind to **127.0.0.1** (USB) or to the **host's own Wi-Fi Direct access point
  address** (Wi-Fi), never to all interfaces or the LAN. Wi-Fi sessions must pass the
  pairing-code handshake before anything else; the protocol has no encryption of its own.
- Always set `TCP_NODELAY`, and send header + payload in **one** write.
- No FFmpeg or GPL dependencies. The host uses the Windows SDK only; the repo is MIT.
- Capture only the virtual display's output, never "output 0 / primary".
- Anything the Android decoder does beyond the baseline (`KEY_LOW_LATENCY`, `KEY_PRIORITY`)
  must be capability-checked and have a fallback. Vendor codecs vary.
- Nothing device-specific is hard-coded outside defaults. The client reports its mode in `HELLO`.
- Driver code derived from Microsoft's sample keeps its original copyright headers. See
  `driver/THIRD_PARTY_NOTICES.md`.

## Conventions
- C++: C++20, `/W4 /WX`, WRL `ComPtr`, `HRESULT`s checked and propagated, no exceptions across
  COM boundaries. Formatting via `.clang-format`.
- Kotlin: official style, coroutines/threads kept out of the decode callback hot path.
- PowerShell scripts: `Set-StrictMode`/`$ErrorActionPreference='Stop'`, idempotent where possible.
- Commits: small and imperative ("Add MF encoder wrapper"). One milestone item per commit where
  practical.

## Hardware-in-the-loop
Driver install, capture and anything on the tablet can't run in CI. When you change those,
say what was verified manually (and on which device) in the commit/PR description.
