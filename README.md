# DeuxDisplay

Use an Android tablet as a **low-latency extended monitor** for Windows 11, over a USB-C cable
or wirelessly over a direct Wi-Fi link to the PC.

DeuxDisplay adds a real virtual monitor to Windows through an Indirect Display Driver. It captures
that monitor with Desktop Duplication, hardware-encodes it to H.264/HEVC in low-latency mode, and
streams it to an Android app that decodes straight to the screen. The stream travels over an ADB
USB tunnel, or over a private Wi-Fi network the PC creates itself (no router in between).

> **Status: early development, working end to end on the reference device.** See
> [PLAN.md](PLAN.md) for milestones. Not yet packaged for end users.

Reference hardware: OnePlus Pad Go (2408×1720 @ 90 Hz). Other Windows 11 PCs and Android 8+
devices are meant to work too.

## How it works

A virtual monitor on the PC is captured, hardware-encoded and streamed to the tablet, which
decodes it straight to the screen. Touches on the tablet travel the same connection back and are
injected on that monitor. The host serves **USB** and **Wi-Fi** at the same time, and the app
picks which one to use.

---

### Architecture

```mermaid
flowchart TB
    subgraph PC["Windows 11 PC"]
        Apps["App windows<br/>(extended desktop)"] --> DWM["Desktop Window Manager"]
        DWM --> Monitor["DeuxDisplayIdd virtual monitor<br/>IddCx driver, EDID built from HELLO"]
        Agent["DeuxDisplayAgent.exe<br/>tray · starts at login<br/>restarts the host"] -. "runs hidden" .-> Host
        subgraph Host["DeuxDisplayHost.exe (C++20)"]
            Display["display/<br/>plug · unplug · force extend"]
            Capture["capture/<br/>DXGI Desktop Duplication"]
            Render["render/<br/>cursor overlay · scale · BGRA→NV12"]
            Encode["encode/<br/>Media Foundation HW H.264 / HEVC"]
            Server["Server · protocol/<br/>pacing · stats · keyframes"]
            Input["input/<br/>TouchTracker → InjectTouchInput"]
            Tcp["transport/<br/>TCP · TCP_NODELAY<br/>127.0.0.1 + access point address"]
            Wireless["wireless/<br/>Wi-Fi Direct access point<br/>pairing code · WLAN tuning"]
            Adb["adb/<br/>track-devices → adb reverse<br/>on every plug-in"]
            Capture --> Render --> Encode --> Server --> Tcp
            Tcp -- "INPUT" --> Input
            Wireless -. "starts network,<br/>192.168.137.1" .-> Tcp
            Adb -. "tunnel tcp:27183" .-> Tcp
        end
        Display -- "PLUG / UNPLUG IOCTL" --> Monitor
        Monitor --> Capture
        Input -- "touch" --> DWM
    end

    Tcp <== "USB · adb reverse tcp:27183" ==> Client
    Tcp <== "Wi-Fi · direct, WPA2<br/>pairing-code auth" ==> Client

    subgraph Tablet["Android tablet"]
        Client["StreamClient<br/>TCP · auth · HELLO · clock sync"] --> Decoder["VideoDecoder<br/>MediaCodec HW, low latency"]
        Decoder --> Surface["SurfaceView<br/>full screen"]
        UI["MainActivity<br/>mode + connection picker · touch capture"]
        UI -- "picked mode → HELLO<br/>MotionEvent → INPUT" --> Client
        Link["WifiLink<br/>joins the PC's network<br/>low-latency WifiLock"] -.-> Client
    end
```

| Part | Where | Job |
|------|-------|-----|
| Virtual display driver | `driver/` | Adds a real monitor to Windows, sized to the tablet (MS-PL) |
| Host | `host/` | Plugs the monitor, captures, encodes, streams, injects touch, keeps the USB tunnel up |
| Agent | `host/agent/` | Tray app that runs the host in the background from login |
| Android client | `android/` | Connects, decodes to the screen, sends touch and the picked mode |
| Wire protocol | `docs/wire-protocol.md` | Source of truth for every message both sides exchange |
| Wi-Fi link | `host/src/wireless/`, `android/.../WifiLink.kt` | The PC's own network, pairing, radio tuning |

---

### A session, end to end

```mermaid
sequenceDiagram
    autonumber
    participant T as Android app
    participant H as DeuxDisplayHost
    participant D as IddCx driver
    participant W as Windows

    alt USB
        T->>H: TCP connect (adb reverse → 127.0.0.1)
    else Wi-Fi
        T->>T: Join DeuxDisplay-xxxx (WPA2, from the pairing code)
        T->>H: TCP connect (192.168.137.1)
        H->>T: AUTH_CHALLENGE (host nonce)
        T->>H: AUTH_RESPONSE (client nonce + HMAC)
        H->>T: AUTH_OK (host HMAC)
    end
    T->>H: HELLO (panel size, refresh, DPI, codecs, picked mode)
    H->>D: PLUG (size, refresh rates, physical mm)
    D->>W: Monitor arrives with generated EDID
    W-->>H: New output appears (switched to "extend" if mirrored)
    H->>H: Build pipeline (duplication, composer, encoder)
    H->>T: CONFIG (codec, stream size, fps, bitrate)
    opt USB session
        H->>T: PAIRING (code for Wi-Fi, PC name)
    end
    H->>T: VIDEO_FRAME [CODEC_CONFIG] SPS/PPS
    H->>T: VIDEO_FRAME [KEYFRAME]

    loop Whenever the desktop changes (paced to the frame rate)
        H->>T: VIDEO_FRAME (Annex-B access unit)
    end

    par Telemetry
        T->>H: PING / PONG (clock offset, RTT)
        T->>H: FRAME_STATS (received, decoded, on screen)
    and Touch
        T->>H: INPUT touch frame (all contacts)
        H->>W: InjectTouchInput on the virtual monitor
    and Recovery
        T->>H: REQUEST_KEYFRAME after a decoder error
        H->>T: VIDEO_FRAME [KEYFRAME]
    end

    alt User picks a new resolution / frame rate
        T->>H: Disconnect, reconnect with new HELLO
        H->>D: UNPLUG, then PLUG in the new mode
    else App closed, tablet asleep, cable pulled or Wi-Fi lost
        T--xH: Connection drops
        H->>W: Lift all touch contacts
        H->>D: UNPLUG (also automatic if the host dies)
    end
```

---

### Frame and touch paths

Every stage below is on the latency budget: no queues, no frame pacing on the client, one
`send()` per message. Numbers live in [docs/latency-notes.md](docs/latency-notes.md).

```mermaid
flowchart LR
    subgraph Video["Video: PC → tablet"]
        direction TB
        V1["DWM presents to<br/>the virtual monitor"] --> V2["AcquireNextFrame<br/>Desktop Duplication"]
        V2 --> V3["Draw cursor, scale<br/>BGRA → NV12 on GPU"]
        V3 --> V4["HW encode<br/>low-latency, no B-frames"]
        V4 --> V5["Split SPS/PPS<br/>header + payload, one send"]
        V5 --> V6["TCP over the adb USB tunnel<br/>or the direct Wi-Fi link"]
        V6 --> V7["MediaCodec decode<br/>low-latency if supported"]
        V7 --> V8["Release to Surface<br/>immediately"]
    end

    subgraph Touch["Touch: tablet → PC"]
        direction TB
        T1["MotionEvent<br/>unbuffered dispatch"] --> T2["Normalize to 0..65535<br/>per contact, slot 0-9"]
        T2 --> T3["INPUT touch frame<br/>sent on input thread"]
        T3 --> T4["TCP over USB<br/>or Wi-Fi"]
        T4 --> T5["TouchTracker<br/>Down / Update / Up"]
        T5 --> T6["Map to monitor rect<br/>InjectTouchInput"]
    end

    Video ~~~ Touch
```

---

### Picking the stream format

The app lists only modes the device can show: the panel's own sizes and refresh rates, plus
scaled-down sizes the hardware decoder accepts, and a 30 fps stream-only rate. Press **Back**
while streaming to open the picker. Choices made there override the host's `--max-stream-size`,
`--max-fps` and `--codec` options.

**Codec** is *Auto*, *H.264* or *HEVC (H.265)* (HEVC is listed only if the tablet has a hardware
HEVC decoder). The pick travels in HELLO's `codecs` bitmask: the app advertises only the chosen
codec. *Auto* offers HEVC only at sizes the HEVC decoder is rated for, and the host then prefers
it; an explicit HEVC pick is used at any size. HEVC gives sharper text per bit and more decoder
headroom, but on the reference tablet it doesn't lower latency (see
[docs/latency-notes.md](docs/latency-notes.md)).

```mermaid
flowchart TB
    A["Display.getSupportedModes()"] --> C["Resolution + frame rate + codec<br/>choices (or Auto)"]
    B["MediaCodec isSizeSupported()<br/>+ hardware HEVC decoder?"] --> C
    C -- "Apply" --> E["Reconnect with HELLO<br/>mode_width / height / refresh, codecs"]
    E --> F["Host plugs a monitor with<br/>only that mode"]
    F --> G["Windows renders at that mode<br/>host encodes it 1:1"]
    G --> H["Tablet scales to its panel"]
```

---

### Connecting over Wi-Fi

The PC runs its own Wi-Fi network (a Wi-Fi Direct group in "legacy" mode, which the tablet joins
like any WPA2 access point). Each frame crosses the air once, with no router and no other traffic
on the link. One **pairing code** (20 characters) derives the network name, its WPA2 passphrase
and the key for a mutual challenge-response, so only paired tablets can join or connect.

```mermaid
flowchart TB
    subgraph Pair["Pair once"]
        P1["USB session"] -- "PAIRING message" --> P3["Tablet stores the code"]
        P2["DeuxDisplayHost --pair<br/>shows the code"] -. "or type it<br/>in the app" .-> P3
    end

    subgraph Connect["Every Wi-Fi session"]
        C1["Host starts network<br/>DeuxDisplay-xxxx · 192.168.137.1"]
        C2["App joins it<br/>WifiNetworkSpecifier<br/>(one-time system prompt)"]
        C3["Low-latency WifiLock on tablet<br/>media streaming mode on PC"]
        C4["TCP to the gateway<br/>AUTH challenge → AUTH_OK"]
        C5["HELLO → stream"]
        C1 --> C2 --> C3 --> C4 --> C5
    end

    P3 --> C2
```

- The host listens only on 127.0.0.1 (USB) and on its own network's address, never on your LAN.
- While streaming over Wi-Fi the tablet leaves its usual Wi-Fi (most tablets can't join two
  networks), so it has no internet until the app closes.
- **Latency status:** the direct link is ~2x faster at the median and 3–7x higher in throughput
  than going through a router. But the Windows/Intel AX211 access point is away for ~60 ms of
  every ~100 ms beacon period, so frames currently land 55–60 ms after capture (vs ~23 ms over USB).
  Details and next steps are in [docs/latency-notes.md](docs/latency-notes.md#wi-fi). USB is
  still the lowest-latency option.

More detail in [docs/architecture.md](docs/architecture.md) and the protocol spec in
[docs/wire-protocol.md](docs/wire-protocol.md).

## Repository layout

| Path                 | What                                                            |
|----------------------|-----------------------------------------------------------------|
| `driver/`            | IddCx virtual display driver (C++, UMDF)                        |
| `host/`              | Windows capture/encode/stream service (C++20)                   |
| `android/`           | Android client app (Kotlin)                                     |
| `docs/`              | Architecture, wire protocol, latency notes                      |
| `scripts/`           | Dev bootstrap, driver install, Wi-Fi firewall rule, run helpers |
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
.\scripts\install-driver.ps1                               # elevated, once; no test-signing mode needed
.\host\x64\Release\DeuxDisplayHost.exe --create-display    # normal user: virtual monitor appears
```
`install-driver.ps1` trusts a locally generated signing certificate on your machine. Read
[driver/README.md](driver/README.md) first. After installation nothing needs admin rights.

### Try the stream without a tablet
```powershell
.\host\x64\Release\DeuxDisplayHost.exe --serve             # USB (127.0.0.1:27183) + Wi-Fi
.\host\x64\Release\DeuxDisplayHost.exe --pair              # Wi-Fi pairing code
python tools\dump_receiver.py --seconds 10                 # acts as the tablet; writes out.h264
ffplay out.h264
```

### Android app
```powershell
cd android
.\gradlew.bat assembleDebug
```

### Run it: plug and play (USB)
Once, with the driver installed and the app on the tablet (`.\scripts\run.ps1 -Install` installs it):
```powershell
.\scripts\install-autostart.ps1    # per user, no admin; -Remove undoes it
```
This starts **DeuxDisplayAgent** now and at every login: a tray icon that keeps the host running
in the background (log: `%LOCALAPPDATA%\DeuxDisplay\host.log`). From then on it behaves like a
monitor cable:

1. Open DeuxDisplay on the tablet.
2. Plug in the USB cable. The tablet appears as a monitor to the right of your main display
   within about a second.
3. Unplug it, close the app or let the tablet sleep, and the monitor goes away.

Plugging in with the app closed does nothing: the tablet just charges.

- **USB debugging must be on** (Settings > About tablet > tap *Build number* 7 times, then
  Developer options > USB debugging). The stream travels over adb. The first time, accept
  *Allow USB debugging* on the tablet and tick *Always allow from this computer*.
- **Any USB mode works:** *Charging only* (Android's default), *File transfer* and so on. adb runs
  alongside all of them. Switching modes briefly drops the display, and it comes back by itself.
- The host re-creates the adb tunnel whenever the tablet (re)appears: cable re-plugged, USB mode
  switched, or the adb server restarted by another tool. No need to run `adb reverse` yourself.

To try options or watch the log live, exit the agent from the tray and use the console runner
instead: `.\scripts\run.ps1` (`-Codec hevc`, `-MaxFps 60`, ...). Options can also be set for
the agent: `.\scripts\install-autostart.ps1 -HostArgs '--codec hevc'`.

**Wireless** (not started in the background): after one USB session, which pairs the tablet,
exit the agent from the tray. Then press **Back** on the tablet, set **Connection** to *Wi-Fi
(direct to PC)* and **Apply**. Approve Android's "connect to DeuxDisplay-xxxx" prompt the first
time. From then on the cable is optional:
```powershell
.\scripts\run.ps1 -Transport Wifi    # or Both (default) / Usb
```
Windows Firewall has to allow the host inbound. `install-driver.ps1` sets that up
(`scripts\enable-wireless.ps1`), otherwise Windows asks the first time. The Wi-Fi option needs
Android 10+ and a PC Wi-Fi adapter with Wi-Fi Direct.

On the tablet:
- **Touch** controls the PC: tap, drag, and multi-finger gestures are injected on the tablet's
  monitor (`--no-touch` on the host turns this off).
- **Back** opens the settings: resolution and frame rate from what the device supports (or
  *Auto*), the codec (*Auto*, H.264 or HEVC), the connection (USB or Wi-Fi), and a field for
  typing a pairing code. Applying reconnects, and the monitor comes back in the new mode. The
  choices are remembered.
- For the lowest latency on the reference tablet, stay at **60 fps** or below: its decoder
  manages ~60 fps at native size, and extra frames queue up.

## Contributing
See [CONTRIBUTING.md](CONTRIBUTING.md).

## License
[MIT](LICENSE), except `driver/`, which is derived from Microsoft's IddCx sample and is
licensed under [MS-PL](driver/LICENSE). See
[driver/THIRD_PARTY_NOTICES.md](driver/THIRD_PARTY_NOTICES.md).
