# Architecture

DeuxDisplay turns Android devices into extended (not mirrored) Windows 11 displays over USB or
a direct Wi-Fi link, optimised for end-to-end latency.

```
 Windows 11 PC                                                Android tablet
┌──────────────────────────────────────────────┐            ┌──────────────────────────────┐
│ DeuxDisplayIdd (IddCx virtual monitor, UMDF) │            │ DeuxDisplay app (Kotlin)     │
│   └─ one monitor per device, offering every  │            │                              │
│      size and rate the device can show       │            │  TCP client ──► demux        │
│                  │ desktop composition       │            │                   │          │
│                  ▼                           │  USB:      │                   ▼          │
│ DeuxDisplayHost.exe (C++)                    │  adb       │  MediaCodec (HW H.264/HEVC/  │
│   capture: DXGI Desktop Duplication  ────────┼─ reverse ──┼─►  VP9, low-latency keys)    │
│   encode:  Media Foundation HW MFT           │  tunnel    │                   │          │
│            (H.264/HEVC/VP9, adaptive bitrate)│            │                   ▼          │
│   transport: TCP, TCP_NODELAY                │  Wi-Fi:    │  SurfaceView (direct render) │
│   input: touch, dock shortcuts, PC volume    │  PC's AP   │  touch · dock · volume keys  │
└──────────────────────────────────────────────┘            └──────────────────────────────┘
```

## Components

### Virtual display driver — `driver/`
An Indirect Display Driver built on IddCx (user-mode, UMDF 2), derived from Microsoft's IddCx
sample, which is **MS-PL** licensed, so `driver/` is MS-PL (see
[`driver/THIRD_PARTY_NOTICES.md`](../driver/THIRD_PARTY_NOTICES.md)). It creates one virtual
monitor per connected client (up to four), each with an EDID and a mode list describing that
device (e.g. OnePlus Pad Go: 2408×1720 plus scaled sizes, at 90/60/50/48/30 Hz).
Windows composes the extended desktop onto it like any real monitor. The driver's swap-chain
processor simply drains frames; the host captures them through Desktop Duplication.

**Lifecycle, and no admin at runtime.** Installing (`scripts/install-driver.ps1`, admin, once)
adds the driver package and creates a root-enumerated device (`ROOT\DEUXDISPLAYIDD\0000`).
Windows keeps that device and restores it at every boot, like detected hardware, so the display
adapter always exists but with no monitor attached. (Early versions used a software device, which
Windows didn't restore after a reboot; `--install-device` replaces it.) At runtime the host (a normal user) opens the
driver's device interface (`driver/DeuxDisplayIdd/Public.h`) and sends:

- `PLUG` with the preferred mode, the full mode list and the physical size (from `HELLO`). The
  request (version 2) lists up to 32 modes: every stream size the client can show × every frame
  rate. The driver builds an EDID from the preferred timing, offers exactly the listed modes to
  Windows, and reports monitor arrival, so any tablet gets a correctly sized, correctly scaled
  monitor. Host and driver must be the same version.
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

**Modes switch in place.** Because the monitor offers every mode up front, changing resolution
or refresh rate never replugs it. The user can pick a mode in Windows' *Settings > Display*, or
the app sends `DISPLAY_MODE` and the host calls `ChangeDisplaySettingsEx` on the monitor. Either
way Desktop Duplication reports `ACCESS_LOST`, the host rebuilds capture and encoder for the new
mode (~0.5 s) and sends `CONFIG`. After plugging, the host also sets the app's picked mode, since
Windows otherwise restores the mode it last used for that monitor.

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
- **render/** — draws the cursor, scales, and converts BGRA to NV12 with the GPU's video
  processor, in limited (16–235) or full (0–255) BT.709 range.
- **encode/** — Media Foundation hardware MFTs for H.264, HEVC and VP9 (`MFTEnumEx` with
  `MFT_ENUM_FLAG_HARDWARE`, so NVENC / Quick Sync / AMF are all reached through one path),
  `CODECAPI_AVLowLatencyMode`, no B-frames, CBR, one reference frame. Frames stay on the GPU
  (DXGI device manager). VP9 is profile 0 (4:2:0): Intel's VP9 MFT takes only NV12 input. It
  also ignores the configured colour range, so for full range the host sets the keyframe
  header's `color_range` bit itself (`Vp9Header.h`).
  - **Encoder swap.** Hardware MFTs may accept bitrate and quality changes through `ICodecAPI`
    and silently ignore them (Intel's does). So any change builds a new encoder on a worker
    thread (~200–350 ms) while the current one keeps streaming, and the frame loop swaps it in
    between two frames. The new encoder starts with a keyframe; the old one shuts down on a
    worker thread too.
  - **Adaptive bitrate** (`AdaptiveBitrate.cpp`, unit-tested). Fed from `FRAME_STATS` on the
    session's reader thread: *delivery* is host send → decoded on the client, the part of the
    latency the bitrate drives. It raises the bitrate while frames use most of their budget and
    delivery stays at its recent best, cuts it when delivery rises, and settles instead of
    hunting, since every change costs an encoder and a keyframe. It relearns its baseline
    whenever the stream is rebuilt (new mode, rotation). Rules are in
    [wire-protocol.md](wire-protocol.md#adaptive-bitrate).
- **Sharp refresh** (in the session loop). When enabled and the screen has been still for
  120 ms, the last image is encoded three more times and sent as `REPEAT` frames: each pass
  codes the difference to the encoder's own lossy copy, restoring detail the frame budget
  dropped. The frame wait is shortened only while a pass is due.
- **display/** — plugs and unplugs through the driver, forces the *extend* topology, sets the
  monitor's mode (`SetDisplayMode`) and orientation (`SetDisplayOrientation`), and builds the
  mode list (`ModeList.h`, unit-tested).
- **input/** — `TouchTracker` turns `INPUT` frames into Windows touch injection on the
  session's monitor; `Shortcuts` sends the dock's keys (undo, redo, Task view, play/pause) to the
  active window.
- **media/** — `MediaControls` changes the default playback device's volume directly (no key
  press, so no Windows flyout), watches the media session the play/pause key controls, and sends
  `MEDIA_STATE` when either changes.
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

Each session thread runs the frame loop and a reader thread. The reader handles client messages
(touch, dock actions, settings, `FRAME_STATS`) and feeds the adaptive bitrate controller; the
frame loop owns capture, conversion and the encoder, and applies whatever the reader decided
(`EncoderControl`), since the encoder isn't thread-safe.

### Android client — `android/`
Kotlin, minSdk 26, targets API 35. A single full-screen activity with a `SurfaceView` that
`MediaCodec` decodes straight to, releasing each frame the moment it's decoded (no pacing).

- **Decoder** (`VideoDecoder.kt`): the hardware decoder for the stream's codec, configured with
  `KEY_LOW_LATENCY`, `KEY_PRIORITY` = realtime, `KEY_OPERATING_RATE` = maximum (decoders clock
  themselves from it; ~4.5 ms faster on the reference tablet) and MediaTek's low-latency and
  no-post-processing hints. If configure rejects the format, only the least important key left is
  dropped per retry. The decoder is also told the stream's colour range, standard and transfer.
  Render timestamps from decoders that report them on another clock are replaced with the
  callback's arrival time.
- **Panel refresh**: the app asks for the panel mode matching the stream's rate (or its smallest
  multiple) and sets it as the surface's frame rate on Android 11+. It's a request: some OEM
  policies override it.
- **Settings** (`MainActivity.kt`): Display, Stream quality, Connection and App sections. Mode
  changes go to the host as `DISPLAY_MODE`, encoder options as `ENCODER_SETTINGS`, both applied
  live; codec and connection changes reconnect. Mode changes made on the PC are adopted.
- **Dock and volume keys** (`DockView.kt`, `VolumeIndicator.kt`): shortcut buttons sent as
  `ACTION`, and the volume keys redirected to the PC while streaming. Both appear only once the
  host sends `MEDIA_STATE`.
- **Debug stats** (`StreamStats.kt`, `StatsOverlay.kt`): counters kept only while the overlay is
  shown, read twice a second.

### Transport
The host serves two transports at once; the app picks one (settings panel > Connection > Link).

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
- Up to four virtual displays (one per client), up to 32 modes each, SDR 8-bit 4:2:0 only.
  Rotation follows the tablet (frames stay landscape on the wire).
- No audio streaming (the dock and volume keys control the PC's own audio). Input covers touch
  and the dock's shortcuts: `INPUT` touch frames are injected with Windows touch injection on
  the virtual display; pen is not forwarded yet.
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
| Mode changed (app or Windows) | Pipeline rebuilt for the new mode, `CONFIG` sent, decoder reset (~0.5 s) |
| Bitrate / quality changed     | New encoder built in the background, swapped in with a keyframe |
| Full range toggled            | Pipeline rebuilt like a mode change                            |
| Decoder error on client       | Client resets decoder and sends `REQUEST_KEYFRAME`             |
| Virtual display disabled      | Host waits for the output to reappear                          |

## Alternatives considered

- **Writing a WDDM/IddCx driver from scratch**: a forked sample gets the same result for far
  less work.
- **FFmpeg for encoding**: redundant with MF's hardware MFT dispatch, and hardware-encoder
  builds tend to pull in GPL/non-free flags that conflict with an MIT release.
- **Test-signing mode**: unnecessary for a UMDF driver, and usually blocked by Secure Boot.
- **C#/.NET host**: GC pauses in the per-frame hot loop and a second debugging model.
- **H.264 only**: the first versions used it everywhere. HEVC is now preferred when the client
  can decode it (sharper text per bit), and VP9 is offered too, but on the reference tablet HEVC
  is the fastest to decode.
- **Changing bitrate through `ICodecAPI` mid-stream**: tried first; Intel's MFT returns success
  and ignores it. Hence the encoder swap.
- **4:4:4 colour** (no colour fringing on text): the tablet's decoders support only 4:2:0 for
  H.264 and HEVC. Its VP9 decoder advertises 4:4:4 (profile 1), but Windows' VP9 MFT only takes
  4:2:0 input; Intel's oneVPL SDK could do it, at the cost of a non-Windows-SDK dependency.
- **Reading frames directly inside the IDD swap-chain processor** (skipping Desktop
  Duplication): potentially one fewer copy, but it needs cross-process texture sharing out of
  WUDFHost. Worth revisiting in M4 if capture shows up in the latency budget.
