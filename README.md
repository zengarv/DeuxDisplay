# DeuxDisplay

Use an Android tablet as a **low-latency extended monitor** for Windows 11, over a USB-C cable.

DeuxDisplay adds a real virtual monitor to Windows through an Indirect Display Driver. It captures
that monitor with Desktop Duplication, hardware-encodes it to H.264 in low-latency mode, and
streams it over an ADB USB tunnel to an Android app that decodes straight to the screen.

> **Status: early development.** See [PLAN.md](PLAN.md) for milestones. Nothing here is ready
> for end users yet.

Reference hardware: OnePlus Pad Go (2408×1720 @ 90 Hz). Other Windows 11 PCs and Android 8+
devices are meant to work too.

## How it works

```
Windows app windows ─► virtual monitor (IddCx driver)
                         │ Desktop Duplication
                         ▼
                    H.264 hardware encoder (Media Foundation, low-latency)
                         │ TCP 127.0.0.1 ── adb reverse ── USB-C
                         ▼
Android app ─► MediaCodec hardware decoder ─► SurfaceView
```

More detail in [docs/architecture.md](docs/architecture.md) and the protocol spec in
[docs/wire-protocol.md](docs/wire-protocol.md).

## Repository layout

| Path                 | What                                                            |
|----------------------|-----------------------------------------------------------------|
| `driver/`            | IddCx virtual display driver (C++, UMDF)                        |
| `host/`              | Windows capture/encode/stream service (C++20)                   |
| `android/`           | Android client app (Kotlin)                                     |
| `docs/`              | Architecture, wire protocol, latency notes                      |
| `scripts/`           | Dev bootstrap, driver test-signing/install, run helpers         |
| `.github/workflows/` | CI                                                              |

## Building from source

### Prerequisites
- Windows 11 x64
- Visual Studio 2022 or Build Tools 2022 with **Desktop development with C++**
  (MSVC v143, Windows 11 SDK). The WDK is pulled from NuGet automatically for the driver build.
- An Android device with **USB debugging** enabled

The Java/Android toolchain installs per-user with one script (no admin rights needed):

```powershell
.\scripts\bootstrap-dev.ps1            # add -PersistEnv to set JAVA_HOME/ANDROID_HOME/PATH for new shells
```

### Host
From a *Developer PowerShell for VS 2022*:
```powershell
msbuild DeuxDisplay.sln /p:Configuration=Release /p:Platform=x64 /m
.\host\x64\Release\DeuxDisplayHost.exe --list-outputs
```

### Driver
Also needs the VS **Windows Driver Kit** component (VS 17.14+). See
[driver/README.md](driver/README.md).
```powershell
msbuild driver\DeuxDisplayIdd.sln /t:restore /p:RestorePackagesConfig=true
msbuild driver\DeuxDisplayIdd.sln /p:Configuration=Release /p:Platform=x64
.\scripts\install-driver.ps1                               # elevated; no test-signing mode needed
.\host\x64\Release\DeuxDisplayHost.exe --create-display    # virtual monitor appears
```
`install-driver.ps1` trusts a locally generated signing certificate on your machine. Read
[driver/README.md](driver/README.md) first.

### Android app
```powershell
cd android
.\gradlew.bat assembleDebug
adb install -r app\build\outputs\apk\debug\app-debug.apk
```

## Contributing
See [CONTRIBUTING.md](CONTRIBUTING.md).

## License
[MIT](LICENSE), except `driver/`, which is derived from Microsoft's IddCx sample and is
licensed under [MS-PL](driver/LICENSE). See
[driver/THIRD_PARTY_NOTICES.md](driver/THIRD_PARTY_NOTICES.md).
