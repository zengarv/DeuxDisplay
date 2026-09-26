# CLAUDE.md

Guidance for Claude Code (and humans) working in this repo.

## What this is
DeuxDisplay makes an Android tablet an extended Windows 11 monitor over USB with minimal latency.
Three parts, one repo:

- `driver/` — IddCx virtual monitor driver (C++, UMDF 2, WDK via NuGet)
- `host/` — `DeuxDisplayHost.exe`: Desktop Duplication → Media Foundation H.264 → loopback TCP (C++20)
- `android/` — Kotlin client: TCP → MediaCodec → SurfaceView

`PLAN.md` holds milestones and status; tick checkboxes there when finishing work.
`docs/wire-protocol.md` is the source of truth for the protocol. Host (`host/src/protocol/`) and
Android (`android/.../protocol/`) code must match it, so update the doc in the same commit as any
protocol change.

## Build & run
Toolchain: VS 2022 (or Build Tools) with C++ workload. JDK 17, Android SDK and adb come from
`scripts/bootstrap-dev.ps1`, which installs to `%LOCALAPPDATA%\DeuxDisplay\tools`.

```powershell
# Host (+ tests)
msbuild DeuxDisplay.sln /p:Configuration=Release /p:Platform=x64 /m
host\x64\Release\DeuxDisplayHost.exe --list-outputs
host\x64\Release\DeuxDisplayHostTests.exe

# Driver (restores WDK NuGet packages)
msbuild driver\DeuxDisplayIdd.sln /restore /p:Configuration=Release /p:Platform=x64

# Android
$env:JAVA_HOME = "$env:LOCALAPPDATA\DeuxDisplay\tools\jdk17"
$env:ANDROID_HOME = "$env:LOCALAPPDATA\DeuxDisplay\tools\android-sdk"
cd android; .\gradlew.bat assembleDebug lint test
```

MSBuild isn't on PATH by default. Use a VS Developer PowerShell, or locate it with
`vswhere -latest -find MSBuild\**\Bin\MSBuild.exe`.

## Rules that matter
- **Latency is the product.** Don't add buffering, queues, frame pacing, or extra copies in
  the frame path without measuring the effect. Log numbers in `docs/latency-notes.md`.
- Sockets bind to **127.0.0.1 only**. The protocol has no auth.
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
