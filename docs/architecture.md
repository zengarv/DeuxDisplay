# Architecture

DeuxDisplay turns an Android device into an extended (not mirrored) Windows 11 display over
a wired USB connection, optimised for end-to-end latency.

```
 Windows 11 PC                                                Android tablet
┌──────────────────────────────────────────────┐            ┌──────────────────────────────┐
│ DeuxDisplayIdd (IddCx virtual monitor, UMDF) │            │ DeuxDisplay app (Kotlin)     │
│   └─ appears in Display Settings with the    │            │                              │
│      tablet's native EDID mode               │            │  TCP client ──► demux        │
│                  │ desktop composition       │            │                   │          │
│                  ▼                           │            │                   ▼          │
│ DeuxDisplayHost.exe (C++)                    │  USB-C     │  MediaCodec (HW AVC decoder, │
│   capture: DXGI Desktop Duplication  ────────┼── adb ─────┼─►  low-latency mode)         │
│   encode:  Media Foundation HW MFT (H.264)   │  reverse   │                   │          │
│   transport: TCP 127.0.0.1 (TCP_NODELAY)     │  tunnel    │                   ▼          │
│                                              │            │  SurfaceView (direct render) │
└──────────────────────────────────────────────┘            └──────────────────────────────┘
```

## Components

### Virtual display driver — `driver/`
An Indirect Display Driver built on IddCx (user-mode, UMDF 2), derived from Microsoft's IddCx
sample, which is **MS-PL** licensed, so `driver/` is MS-PL (see
[`driver/THIRD_PARTY_NOTICES.md`](../driver/THIRD_PARTY_NOTICES.md)). It creates one virtual
monitor per connected client (up to four), each with an EDID/mode list describing that device
(e.g. OnePlus Pad Go: 2408×1720, 60/90 Hz).
Windows composes the extended desktop onto it like any real monitor. The driver's swap-chain
processor simply drains frames; the host captures them through Desktop Duplication.

**Lifecycle, and no admin at runtime.** Installing (`scripts/install-driver.ps1`, admin, once)
adds the driver package and creates a *persistent* software device, so the display adapter
always exists but with no monitor attached. At runtime the host (a normal user) opens the
driver's device interface (`driver/DeuxDisplayIdd/Public.h`) and sends:

- `PLUG` with the client's resolution, refresh rates and physical size (from `HELLO`). The driver
  generates a matching EDID and mode list and reports monitor arrival, so any tablet gets a
  correctly sized, correctly scaled monitor.
- `UNPLUG` when the client disconnects. If the host dies, closing its handle triggers the
  driver's file-cleanup callback, which unplugs the monitor, so a crash never strands a phantom
  display.

**Several devices at once.** Each driver handle owns at most one monitor, and the host opens one
handle per session, so every client gets its own monitor and `UNPLUG` or a closed handle only
removes that client's monitor. The driver hands out the lowest free connector index (0–3), and
returns it from `PLUG`. The EDID product code is index + 1, so Windows names the monitors
`MONITOR\DXD0001`, `DXD0002`, …, and the host finds each session's output by that hardware ID.
A lone client always gets index 0, the same monitor identity (and remembered arrangement) as
before multi-display support. A host that doesn't ask for the index still works, on index 0.

Windows sometimes attaches a new monitor in *duplicate* mode. The host detects that and applies
the "extend" topology (the same as Win+P → Extend), which needs no admin rights.

Being user-mode, the driver needs no test-signing boot mode for development. A catalog signed
by a locally trusted certificate is enough (`scripts/install-driver.ps1`). Attestation signing
for end users is a possible later step, not a v1 goal.

### Host service — `host/`
Native C++20, no third-party dependencies beyond the Windows SDK.

- **capture/** — `IDXGIOutputDuplication` bound to the virtual display's `IDXGIOutput`
  (matched by device name), never "the primary display". Handles `DXGI_ERROR_ACCESS_LOST`
  (lock screen, UAC, mode change, GPU reset) by recreating the duplication.
- **encode/** — Media Foundation hardware H.264 MFT (`MFTEnumEx` with
  `MFT_ENUM_FLAG_HARDWARE`, so NVENC / Quick Sync / AMF are all reached through one path),
  `CODECAPI_AVLowLatencyMode`, no B-frames, low-delay rate control. Frames stay on the GPU
  (DXGI device manager) where possible.
- **transport/** — TCP listeners on 127.0.0.1 (USB) and on the host's own access point
  (Wi-Fi), `TCP_NODELAY`, header+payload in one send.
- **adb/** — follows the adb server's `host:track-devices` stream (smart-socket protocol on
  127.0.0.1:5037) and applies `adb reverse` to each device as soon as it's attached and
  authorized. It re-applies after re-plugs, USB mode switches and adb server restarts, and starts
  the server if none is running.
- **agent/** (`DeuxDisplayAgent.exe`) — a tray app started at login (per-user Run key, via
  `scripts/install-autostart.ps1`). It runs the host hidden with `--transport usb`, restarts it
  if it exits, keeps it in a kill-on-close job, and logs to `%LOCALAPPDATA%\DeuxDisplay\host.log`.
- **wireless/** — the Wi-Fi Direct access point, the pairing-code store (DPAPI), and WLAN
  tuning (media streaming mode, no background scans) held during Wi-Fi sessions.
- **protocol/** — message (de)serialisation for [wire-protocol.md](wire-protocol.md).

**Sessions.** The main thread only accepts connections; each client runs on its own session
thread with its own virtual monitor, D3D device, duplication, encoder and socket, so sessions
share nothing on the frame path and one device streams exactly as it would alone. Every USB
device tunnels to the same port (`adb reverse` is applied to each), which is fine because
sessions are told apart by connection, not by port. Log lines carry a session tag (`[1]`,
`[2]`, …). The one shared piece is touch: Windows gives a process one injected touch device, so
sessions use separate pointer-ID ranges and each injected frame repeats the other sessions'
fingers that are still down.

### Android client — `android/`
Kotlin, minSdk 26, targets API 35. A single full-screen, orientation-locked activity with a
`SurfaceView`. `MediaCodec` in async mode decodes straight to that surface.
`KEY_LOW_LATENCY` is only set when the decoder advertises `FEATURE_LowLatency`, and
configuration falls back to a plain config if the vendor codec rejects low-latency keys.

### Transport
The host serves two transports at once; the app picks one (settings panel > Connection).

**USB.** `adb reverse tcp:<port> tcp:<port>` lets the tablet connect to `127.0.0.1:<port>` and
reach the host listener over USB. This needs nothing beyond USB debugging, and it is the same
mechanism scrcpy relies on. USB sessions also hand the tablet the pairing code for Wi-Fi.

Plug and play: adb drops reverse tunnels whenever the device re-enumerates (cable re-plugged,
USB mode switched, adb server restarted). The host's `adb/` watcher re-applies the tunnel as
soon as the device is back, and the app retries its local connection every 250 ms. Measured on
the Pad Go: USB drop to tunnel restored 0.7–1.0 s, then tunnel to picture 0.14–0.21 s.

Why adb and not another USB transport (measured 2026-09-27; see
[latency-notes.md](latency-notes.md#usb-transport)):
- **adb reverse**: works in any USB mode, needs USB debugging. The host→tablet hop costs about
  2–5 ms per frame, with 200+ Mbit/s available, against a ~30 Mbit/s stream. This is ~5% of
  glass-to-glass latency, where the decoder's fixed ~30 ms dominates.
- **USB tethering (RNDIS/NCM)**: kernel networking instead of adbd's userspace relay, maybe
  1–2 ms faster. But it has to be switched on by hand on the tablet every time (apps can't), and
  Windows may start routing internet through the tablet.
- **MTP ("file transfer")**: a file protocol, not a stream. Not usable.
- **Android Open Accessory (raw USB bulk)**: the lowest-overhead option (maybe 1–3 ms less),
  no USB debugging needed, and Android can open the app on plug-in. On Windows it needs a
  WinUSB driver bound to each device in accessory mode. Worth doing if USB debugging becomes
  the obstacle for users, not for latency.

**Wi-Fi (direct).** The host starts its own network with `WiFiDirectAdvertisementPublisher` in
legacy mode (a Wi-Fi Direct group owner that ordinary clients join like a WPA2 access point).
Windows puts the host at `192.168.137.1`. The tablet joins with a `WifiNetworkSpecifier` (one
system prompt), which routes only the app's sockets over it, and holds a low-latency
`WifiLock`. Frames make one hop, with no router and no other traffic in between.

- Network name, WPA2 passphrase and the session auth key all derive from one 20-character
  pairing code ([wire-protocol.md](wire-protocol.md#pairing-and-authentication)).
- The host listens on the access point's address only, never on the LAN, and every session
  passes a mutual HMAC challenge before `HELLO`.
- While a Wi-Fi session runs, the host holds WLAN media streaming mode, turns off background
  scans and tags the socket as audio/video (qWAVE, best effort).
- The tablet can't stay on its home Wi-Fi at the same time (no dual-station support on the Pad
  Go), so it has no internet while streaming over Wi-Fi. It rejoins its home Wi-Fi when the
  app stops.
- Windows Firewall must allow the host inbound (`scripts/enable-wireless.ps1`, run by
  `install-driver.ps1`; otherwise Windows asks on first use).

Why not the home LAN or true Wi-Fi Direct? Through a router every frame crosses the air twice
and competes with other traffic (measured: ~3x slower bulk transfers). Android's
`WifiP2pManager` adds pairing prompts and vendor quirks without a latency gain. Miracast would
bypass our encoder and input path. See [latency-notes.md](latency-notes.md#wi-fi).

## Security notes

- The USB socket binds to 127.0.0.1 and is unauthenticated. The Wi-Fi socket binds only to the
  host's own access point, which is WPA2-protected, and requires the pairing-code handshake.
  See [wire-protocol.md](wire-protocol.md).
- The pairing code is stored DPAPI-encrypted per user (`%LOCALAPPDATA%\DeuxDisplay\pairing.bin`)
  on the PC, and in the app's private storage on the tablet. `DeuxDisplayHost --pair --reset`
  replaces it and unpairs every tablet.
- The driver's device object grants read/write to interactive users (INF `Security` SDDL), so
  any logged-in user can plug/unplug the DeuxDisplay monitor. That's harmless (it's their own
  desktop) and it's what lets the host run without admin rights.
- `install-driver.ps1` trusts a locally generated code-signing certificate machine-wide. Its
  private key is non-exportable; `uninstall-driver.ps1 -RemoveCertificate` removes it.

## v1 constraints (intentional)

- `scripts/run.ps1` installs and launches the app on one ADB device (`-Serial` when several are
  attached); the host itself serves every attached device.
- Up to four virtual displays (one per client), each in one fixed landscape mode, SDR only.
- No audio. Input passthrough (M5) covers touch only: `INPUT` touch frames are injected with
  Windows touch injection on the virtual display; pen is not forwarded yet.
- The cursor is composited into the video frame. A separate cursor channel (`CURSOR` message)
  is reserved for later if cursor latency needs to be decoupled from video.

## Behaviour on disruption

| Event                         | v1 behaviour                                                   |
|-------------------------------|----------------------------------------------------------------|
| Tablet unplugged / app closed | Host detects socket close, unplugs the monitor, waits for reconnect |
| Tablet plugged in (app open)  | adb watcher restores the tunnel, the app connects, and the monitor appears (~1 s) |
| USB mode switched / adb server restarted | Same as unplug + plug: display back in ~1 s |
| Host crashes / is killed      | Driver unplugs the monitor when the host's handle closes       |
| Windows lock / UAC / mode set | Duplication recreated; client gets a fresh keyframe            |
| Decoder error on client       | Client resets decoder and sends `REQUEST_KEYFRAME`             |
| Virtual display disabled      | Host waits for the output to reappear                          |

## Alternatives considered

- **Writing a WDDM/IddCx driver from scratch**: a forked sample gets the same result for far
  less work.
- **FFmpeg for encoding**: redundant with MF's hardware MFT dispatch, and hardware-encoder
  builds tend to pull in GPL/non-free flags that conflict with an MIT release.
- **Test-signing mode**: unnecessary for a UMDF driver, and usually blocked by Secure Boot.
- **C#/.NET host**: GC pauses in the per-frame hot loop and a second debugging model.
- **HEVC**: USB bandwidth isn't the constraint, and low-latency AVC decode is more uniformly
  supported on Android.
- **Reading frames directly inside the IDD swap-chain processor** (skipping Desktop
  Duplication): potentially one fewer copy, but it needs cross-process texture sharing out of
  WUDFHost. Worth revisiting in M4 if capture shows up in the latency budget.
