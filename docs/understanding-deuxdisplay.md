# Understanding DeuxDisplay

A guided tour for new contributors. It starts with the intuition (what problem each piece solves
and why it's shaped that way), then walks the host and the Android app at code level. For the
exact byte layouts see [wire-protocol.md](wire-protocol.md). For the driver in depth see
[windows-driver-internals.md](windows-driver-internals.md). For Linux and macOS, see
[porting-linux-macos.md](porting-linux-macos.md).

---

## Part 1: Intuition

### The one-sentence version

Windows is tricked into believing a second monitor is plugged in. Whatever it draws on that
monitor is grabbed, compressed like a video call, sent down the USB cable, and played back
full-screen on the tablet. Touches travel the other way.

### Three pieces, three jobs

Think of a real external monitor. It has three parts: a **graphics port** on the PC, a **cable**,
and a **panel**. DeuxDisplay replaces each with software:

| Real monitor                | DeuxDisplay                        | Where         |
|-----------------------------|------------------------------------|---------------|
| Graphics port + plugged monitor | Virtual monitor driver (IddCx)  | `driver/`     |
| Cable carrying pixels       | Host app: capture → encode → TCP   | `host/`       |
| Panel that shows pixels     | Android app: TCP → decode → screen | `android/`    |

- **The driver only makes the monitor exist.** It tells Windows "a 2408×1720 60 Hz display just
  got plugged in", so Windows extends the desktop onto it and lets you drag windows there. It
  doesn't move pixels anywhere; it throws away the frames Windows hands it.
- **The host is the cable.** It asks Windows for a copy of what's on that monitor (Desktop
  Duplication), shrinks it with the GPU's hardware video encoder (H.264/HEVC), and writes it to a
  socket.
- **The app is the panel.** It reads the socket, feeds the hardware video decoder, and lets the
  decoder draw straight onto the screen.

Why a video codec? Raw 2408×1720 at 60 fps is ~7.5 Gbit/s. H.264 brings it to ~30 Mbit/s,
which fits through adb's USB relay with room to spare. Both ends have dedicated silicon for it,
so it costs only a few milliseconds each way.

### The whole picture

```
      Windows PC                                                       Android tablet
 ┌─────────────────────────────────────────────────┐              ┌──────────────────────────┐
 │  Apps draw windows                              │              │                          │
 │        │                                        │              │                          │
 │        ▼                                        │              │                          │
 │  DWM (desktop compositor) ── extends desktop    │              │                          │
 │        │                     onto the monitor   │              │                          │
 │        ▼                   that the driver made │              │                          │
 │  ┌───────────────┐                              │              │                          │
 │  │ DeuxDisplayIdd│◄── PLUG/UNPLUG (IOCTL) ──┐   │              │                          │
 │  │ virtual       │                          │   │              │                          │
 │  │ monitor       │                          │   │              │                          │
 │  └───────────────┘                          │   │              │                          │
 │        ▲ Desktop Duplication (copy of       │   │              │                          │
 │        │ that monitor's image, on the GPU)  │   │              │                          │
 │  ┌─────┴──────────────────────────────────┐ │   │              │ ┌──────────────────────┐ │
 │  │ DeuxDisplayHost.exe (one session       │─┘   │   USB cable  │ │ DeuxDisplay app      │ │
 │  │ per tablet)                            │     │  adb reverse │ │                      │ │
 │  │ capture → cursor+NV12 → HW encode → TCP├─────┼──────────────┼►│ TCP → MediaCodec →   │ │
 │  │                                        │     │   (or Wi-Fi) │ │ SurfaceView          │ │
 │  │ inject touch ◄──────────────────── TCP │◄────┼──────────────┼─┤ touch → INPUT msg    │ │
 │  └────────────────────────────────────────┘     │              │ └──────────────────────┘ │
 └─────────────────────────────────────────────────┘              └──────────────────────────┘
```

### The life of one frame (≈ 8 ms on the host, ~50 ms glass to glass)

1. You move a window onto the tablet's monitor. DWM composes a new image for that monitor.
2. The host's `AcquireNextFrame` wakes up with a GPU texture (BGRA) of that monitor. No copy to
   the CPU.
3. The GPU draws the mouse pointer on top (Desktop Duplication leaves it out) and converts to
   NV12, the YUV format encoders want.
4. The hardware encoder (NVENC / Quick Sync / AMF, all reached through Media Foundation)
   produces one compressed access unit. Low-latency mode, no B-frames: one frame in, one out.
5. The host writes a 16-byte header plus the frame in **one** `send()` with `TCP_NODELAY`.
6. adb carries the bytes over USB to `127.0.0.1:27183` on the tablet.
7. The app reads the message and queues it into `MediaCodec`.
8. The decoder outputs it; the app releases it to the `SurfaceView` **immediately**, with no
   timestamp-based pacing, and the compositor shows it at the next vsync.

Every stage avoids queues on purpose. A queue anywhere in this path adds a frame or more of lag,
and lag is the thing this project exists to minimise ([latency-notes.md](latency-notes.md)).

### The life of one touch

1. Android delivers a `MotionEvent`. The app normalises each finger to 0..65535 across the video
   frame and sends an `INPUT` message with **all** fingers currently down.
2. The host's reader thread maps 0..65535 onto the virtual monitor's desktop rectangle and calls
   `InjectTouchInput`. To Windows it looks like a touchscreen on that monitor.

### The life of one session

```
 tablet app opens            host                                     driver
 ─────────────────           ─────────────────────────────────        ──────────────
 connect 127.0.0.1:27183 ──► accept, spawn session thread
 HELLO (size, Hz, dpi) ────► build PlugRequest ───────── PLUG ──────► create EDID, monitor arrives
                             wait until Windows extends onto it
                             create D3D device, duplication, encoder
                 ◄────────── CONFIG (codec, size, fps, bitrate)
                 ◄────────── PAIRING (USB only: code for Wi-Fi later)
                 ◄────────── VIDEO_FRAME (SPS/PPS), then keyframe, ...
 PING / FRAME_STATS ───────► latency stats in the log
 INPUT / ORIENTATION ──────► inject touch / rotate the Windows display
 app closes / cable out ───► socket closes, session ends ─ UNPLUG ──► monitor departs
                             (host crash: handle closes ────────────► driver unplugs anyway)
```

Key idea: **the monitor exists exactly as long as the session**. Plug the tablet in and a monitor
appears; unplug it and Windows pulls the windows back to your main screen.

### Why it's built this way (the design rules in one place)

- **Virtual driver, not "mirror the primary screen".** An *extended* desktop needs Windows to
  believe there's a real monitor with its own resolution and DPI. Only a display driver can do
  that. IddCx is Microsoft's framework for exactly this, and it runs in user mode, so no kernel
  code and no test-signing boot mode.
- **The driver runs without admin at runtime.** Install once as admin; after that, the host
  plugs and unplugs monitors as a normal user via an IOCTL. Closing the handle (even by
  crashing) unplugs, so you never get a phantom monitor.
- **adb reverse for USB.** It needs only USB debugging, works on every Android device, and costs
  2–5 ms per frame. The tablet connects to its *own* `127.0.0.1`, which adb tunnels to the PC.
- **Wi-Fi is the host's own access point**, not your home LAN: one radio hop, no router, and the
  protocol has no encryption of its own, so it's kept off shared networks. A pairing code
  derives the WPA2 password and a handshake key.
- **Frames only when the desktop changes.** A static desktop sends nothing. That's why the
  client can ask for a keyframe (`REQUEST_KEYFRAME`) after a decoder reset.
- **One session = one monitor = one thread**, sharing nothing on the frame path, so two tablets
  stream exactly as fast as one.

---

## Part 2: The host (`host/`) in detail

### Mental model

The host is a small server. The main thread accepts connections; each tablet gets a
**session thread** that owns a private pipeline:

```
                          ┌───────────────────────── session thread ─────────────────────────┐
 socket ─► reader thread  │                                                                  │
   ▲      (INPUT, PING,   │  VirtualDisplay ──► OutputLocator ──► DesktopDuplicator          │
   │       KEYFRAME req,  │   (driver handle)    (find \\.\DISPLAYn   (AcquireFrame)          │
   │       ORIENTATION)   │                      by MONITOR\DXD000n)       │                 │
   │           │          │                                                ▼                 │
   │           ▼          │                                        FrameComposer             │
   │     TouchInjector    │                                    (cursor + BGRA→NV12, GPU)      │
   │                      │                                                ▼                 │
   │                      │                                        MfH264Encoder             │
   │                      │                                  (HW MFT, H.264 or HEVC)          │
   │                      │                                                ▼                 │
   └──────────────────────┼──────────── Connection::Send ◄──── AnnexB split (SPS/PPS)      │
                          └──────────────────────────────────────────────────────────────────┘
```

Two threads per session: the **frame loop** (capture → encode → send) and a **reader** that
handles everything the client sends. They share only atomics (`stop`, `keyframeRequested`,
`requestedOrientation`) and the thread-safe `TouchInjector`.

### Folder map

| Folder             | Role                                                                            | Main Windows APIs |
|--------------------|---------------------------------------------------------------------------------|-------------------|
| `main.cpp`         | CLI: `--serve` (default), `--list-outputs`, `--create-display`, `--install-device`, `--pair` | — |
| `Server.cpp`       | Accept loop, `Sessions`, `RunSession`, `Pipeline`, pacing and stats            | — |
| `display/`         | `VirtualDisplay`: talks to the driver; force "extend"; rotate the display       | `SwDeviceCreate`, `CM_Get_Device_Interface_List`, `DeviceIoControl`, `SetDisplayConfig`, `ChangeDisplaySettingsEx` |
| `capture/`         | Find the DXGI output for our monitor; duplicate it; decode pointer shapes       | `IDXGIOutput5::DuplicateOutput1`, `AcquireNextFrame` |
| `render/`          | GPU compose: pointer overlay, BGRA→NV12, scaling, rotation-aware cursor         | D3D11, HLSL shaders |
| `encode/`          | Media Foundation hardware encoder, Annex-B parsing                              | `MFTEnumEx(HARDWARE)`, `ICodecAPI`, DXGI device manager |
| `transport/`       | TCP listener/connection; single gathered send; QoS for Wi-Fi                    | Winsock, qWAVE |
| `protocol/`        | Wire (de)serialisation, pairing code derivations (HMAC-SHA256)                  | BCrypt |
| `input/`           | Touch contact tracking, injection, per-session pointer-ID banks                 | `InitializeTouchInjection`, `InjectTouchInput` |
| `adb/`             | Watches `host:track-devices` on adb's port 5037, applies `reverse` per device   | Winsock (plain adb smart-socket protocol) |
| `wireless/`        | Wi-Fi Direct AP, DPAPI pairing store, WLAN media-streaming mode                 | WinRT `WiFiDirectAdvertisementPublisher`, `CryptProtectData`, `WlanSetInterface` |
| `common/`          | `Log`, QPC clock                                                                | QPC |
| `agent/`           | Tray app at login: keeps `DeuxDisplayHost --transport usb` alive                | Job objects, tray icon |

### `RunSession`, step by step (`host/src/Server.cpp`)

1. **Wi-Fi only:** `Authenticate()` runs the HMAC challenge; turn on WLAN tuning and QoS.
2. **Read `HELLO`.** Reject if the client can't decode H.264 or HEVC.
3. **Pick the mode.** A mode picked on the tablet (`mode_*` fields) overrides CLI limits.
4. **`VirtualDisplay::Plug`** sends the `PlugRequest` (size, physical mm, refresh list). If the
   requested mode doesn't fit, retry with the default. The driver returns a connector index.
5. **`WaitForExtendedDisplay`** waits up to 8 s for Windows to show `MONITOR\DXD000<index+1>` as
   its own desktop output, switching to "extend" if Windows chose "duplicate".
6. **`Pipeline::Initialize`** builds the D3D11 device, duplication, composer and encoder on the
   GPU that drives that output. HEVC falls back to H.264 if the PC can't encode it.
7. Send `CONFIG`, then (USB) `PAIRING`.
8. Start the **reader thread**.
9. **Frame loop:**
   - Sleep until `nextFrameUs` (the `maxFps` cap). While sleeping, Desktop Duplication keeps
     merging updates, so the next acquire is the *newest* image, not a stale queued one.
   - Apply a pending rotation (`ChangeDisplaySettingsEx`). This triggers `ACCESS_LOST`.
   - On a keyframe request with a static desktop, re-encode the last image.
   - `AcquireFrame(100 ms)`. Timeout → loop. Failure (`ACCESS_LOST`: lock screen, UAC, mode
     change) → rebuild the pipeline and resend `CONFIG` if anything changed.
   - `Compose` → `Encode` → `sendEncoded` (sends SPS/PPS as a separate `CODEC_CONFIG` frame
     when they change, then the frame).
10. On exit: stop, shut the socket, join the reader. Destructors then unplug the monitor
    (`VirtualDisplay` is declared first, so it dies last) and lift all touches.

### Things that surprise people

- **Frames are always landscape.** When Windows rotates the monitor, Desktop Duplication still
  hands over scan-out (landscape) textures. The host sends `rotation` in `CONFIG`, and the tablet's
  compositor rotates the surface for free.
- **The cursor is not in the captured image.** `FrameComposer` draws it from
  `DXGI_OUTDUPL_POINTER_SHAPE_INFO` (`capture/PointerShape.cpp`).
- **Touch is shared per process.** Windows gives one injected touch device per process, so each
  session owns a bank of pointer IDs and repeats the other sessions' live contacts in every
  injected frame.
- **Timestamps.** `VIDEO_FRAME.timestamp` is the host capture time (QPC µs). The tablet converts
  its own timestamps to the host clock via PING/PONG and returns `FRAME_STATS`, so the host log
  shows true glass-to-glass stage timings.

---

## Part 3: The Android app (`android/`) in detail

### Mental model

The app is a single full-screen activity whose `SurfaceView` *is* the monitor. The stream lives
exactly as long as the surface: when the tablet sleeps or you leave the app, the surface is
destroyed, the socket closes, and the host unplugs the monitor.

```
 MainActivity (UI thread)
   │  surfaceCreated ──► StreamClient.start()
   │  onTouch ─────────► TouchInput.contacts() ──► StreamClient.sendTouch()  (via a sender thread)
   │  onConfigurationChanged ──► StreamClient.setOrientation()
   │
 StreamClient (network thread, reconnect loop)
   │  connect: USB = 127.0.0.1:27183 every 250 ms  │  Wi-Fi = WifiLink joins the PC's AP
   │  [Wi-Fi] authenticate()  →  HELLO (DisplayInfo)  →  ORIENTATION
   │  loop readMessage():
   │     CONFIG        → VideoDecoder.configure(size, codec, rotation)
   │     PAIRING       → PairingStore (enables Wi-Fi later)
   │     VIDEO_FRAME   → VideoDecoder.submit()
   │     PONG          → ClockSync
   │  pinger thread: PING every so often
   │
 VideoDecoder
   │  submit() on the network thread: dequeueInputBuffer → queueInputBuffer
   │  renderLoop() on its own thread: dequeueOutputBuffer → releaseOutputBuffer(render = true)
   │  OnFrameRenderedListener → FRAME_STATS (sampled) back to the host
   └  on error: drop until next keyframe, send REQUEST_KEYFRAME
```

### File map (`android/app/src/main/kotlin/io/github/zengarv/deuxdisplay/`)

| File                         | Role |
|------------------------------|------|
| `MainActivity.kt`            | Full-screen UI, surface lifecycle, touch routing, settings panel (resolution / refresh / codec / connection), rotation |
| `stream/StreamClient.kt`     | Connect/reconnect, auth, HELLO, message loop, sending touch/orientation/stats |
| `stream/VideoDecoder.kt`     | Picks a hardware decoder, configures low-latency keys with fallback, decode → render without pacing |
| `stream/DisplayInfo.kt`      | Builds `HELLO` from the panel (size, dpi, xdpi/ydpi, refresh, codec mask) |
| `stream/StreamModes.kt`      | Which sizes/rates/codecs this device can offer in the settings |
| `stream/TouchInput.kt`       | `MotionEvent` → normalised contacts |
| `stream/ClockSync.kt`        | NTP-style offset from the minimum-RTT PING/PONG |
| `stream/WifiLink.kt`         | `WifiNetworkSpecifier` join of the PC's AP, low-latency `WifiLock` |
| `stream/PairingStore.kt`     | Stores the pairing code in app-private storage |
| `protocol/Protocol.kt`       | Wire (de)serialisation, mirrors `host/src/protocol/Protocol.cpp` |
| `protocol/Pairing.kt`        | Code parsing, SSID / passphrase / auth-key derivation, mirrors `Pairing.cpp` |

### Decoder tactics worth knowing

- **Hardware first, never "secure".** `pickDecoder` prefers a hardware codec and one advertising
  `FEATURE_LowLatency`.
- **Low-latency keys are best effort.** `KEY_LOW_LATENCY`, `KEY_PRIORITY` and vendor keys that
  disable MediaTek's post-processing are tried first. If `configure()` throws, it retries with a
  plain format (CLAUDE.md rule: capability-check and fall back).
- **Render immediately.** `releaseOutputBuffer(index, true)`, not the timestamp overload. A
  desktop isn't a movie; showing it late is worse than showing it unevenly.
- **Recovery.** Any codec error → reset, drop input until a keyframe, ask for one.

### The app is platform-agnostic

Nothing in the app knows the host runs Windows. It speaks the wire protocol to whatever answers
on `127.0.0.1:27183` (USB) or the AP's gateway (Wi-Fi). That's the main reason a Linux or macOS
host is feasible without touching the app much; see
[porting-linux-macos.md](porting-linux-macos.md).

---

## Part 4: Suggested reading order in the code

1. `docs/wire-protocol.md`: the contract between the two sides.
2. `host/src/protocol/Protocol.h` and `android/.../protocol/Protocol.kt`: the same contract in code.
3. `host/src/Server.cpp`, `RunSession`: the whole host story in ~350 lines.
4. `host/src/display/VirtualDisplay.cpp` then `driver/DeuxDisplayIdd/Public.h`: how a monitor
   gets plugged.
5. `host/src/capture/DesktopDuplicator.cpp` → `render/FrameComposer.cpp` →
   `encode/MfH264Encoder.cpp`: the frame path.
6. `android/.../stream/StreamClient.kt` → `VideoDecoder.kt`: the other half of the frame path.
7. `driver/DeuxDisplayIdd/Driver.cpp`, with [windows-driver-internals.md](windows-driver-internals.md)
   open alongside.
