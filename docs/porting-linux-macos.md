# Porting the host to Linux and macOS

Status: **planning notes, nothing implemented.** Read
[understanding-deuxdisplay.md](understanding-deuxdisplay.md) first. Items marked *(verify)* are
platform facts to confirm on real systems before relying on them.

---

## Part 1: Intuition

### What has to change and what doesn't

The system has three jobs: **make a monitor exist**, **move its pixels**, and **show them**.

- **Show them (Android app): unchanged.** The app speaks only the wire protocol to whatever
  answers on `127.0.0.1:27183`. It never learns what OS the host runs.
- **Move the pixels (host): rewritten per OS, same shape.** Every OS has *some* way to capture a
  display, *some* hardware encoder, and sockets. The session logic (HELLO → CONFIG → frames,
  touch, keyframes, stats) is the same everywhere.
- **Make a monitor exist: the hard part, and different on every OS.** Windows has IddCx, a
  supported framework for virtual monitors. Linux has several partial answers, and each desktop
  stack (X11, GNOME, KDE, wlroots) has its own. macOS has no public API for it at all.

So the port is mostly about **replacing the Windows-only parts under a shared session core**.
Before writing any capture or encode code, prove that each target OS can create a virtual monitor.

### The Windows pieces and their counterparts

```
 Job                    Windows (today)                 Linux candidates                    macOS candidates
 ─────────────────────  ──────────────────────────────  ──────────────────────────────────  ──────────────────────────────
 Virtual monitor        IddCx driver + PLUG IOCTL       Wayland: compositor virtual          CGVirtualDisplay (private
                                                        outputs (GNOME/KDE via portal,       API, used by several
                                                        sway `create_output`)                third-party apps) (verify)
                                                        X11: xrandr on a VIRTUAL/dummy
                                                        output; EVDI kernel module
 Capture that monitor   DXGI Desktop Duplication        PipeWire stream from                 ScreenCaptureKit
                                                        xdg-desktop-portal ScreenCast        (SCStream per SCDisplay)
                                                        (DMA-BUF); X11: XShm + XDamage
 Cursor                 composited by FrameComposer     portal cursor_mode = embedded        SCStreamConfiguration
                                                                                             .showsCursor
 Colour convert         D3D11 shader BGRA→NV12          VA-API VPP / EGL shader / encoder    ask SCK for NV12 directly
                                                        accepts RGB
 HW encode              Media Foundation HW MFT         VA-API (Intel/AMD), NVENC            VideoToolbox
                                                        (NVIDIA)                             (VTCompressionSession)
 Transport              Winsock                         POSIX sockets                        POSIX sockets
 adb tunnel             adb smart-socket on :5037       same protocol, same code             same protocol, same code
 Touch injection        InjectTouchInput                uinput multitouch device, or portal  no public touch injection:
                                                        RemoteDesktop NotifyTouch* / libei   emulate mouse via CGEvent
 Rotation               ChangeDisplaySettingsEx         compositor-specific (xrandr,         private API or not
                                                        wlr-output-management, portal)      supported at first
 Pairing store          DPAPI                           libsecret, or 0600 file              Keychain
 HMAC-SHA256            BCrypt                          OpenSSL (Apache-2.0) or a small      CommonCrypto
                                                        vendored implementation
 Wi-Fi AP               Wi-Fi Direct legacy AP          NetworkManager hotspot /             no public API: defer
                                                        hostapd (verify)
 Autostart              tray agent + Run key            systemd --user unit                  LaunchAgent plist
 Build                  MSBuild                         CMake                                CMake (+ Xcode for signing)
```

### Recommended order

1. **USB only, one tablet, Linux on one desktop stack** (e.g. GNOME on Wayland), so the whole
   loop works end to end on a second OS.
2. **macOS USB**, with mouse emulation for touch.
3. Then more Linux desktops, rotation, multi-tablet, Wi-Fi.

Wi-Fi comes last because the protocol has no encryption. The Windows host only serves Wi-Fi on
its own access point (see CLAUDE.md), and a port must keep that rule or add encryption first.

---

## Part 2: What's already portable in `host/src`

Checked by includes:

| File(s)                               | OS deps          | Port effort |
|---------------------------------------|------------------|-------------|
| `protocol/Protocol.{h,cpp}`           | none             | reuse as is |
| `protocol/Pairing.{h,cpp}`            | BCrypt (HMAC, RNG) | swap the crypto backend |
| `encode/AnnexB.{h,cpp}`               | none             | reuse as is |
| `input/TouchTracker.{h,cpp}`          | none             | reuse as is |
| `adb/AdbProtocol.{h,cpp}`             | none             | reuse as is |
| `adb/AdbWatcher.cpp`                  | Winsock, process launch | small: sockets + `posix_spawn` |
| `render/Rotation.h`, `capture/PointerShape.*` | none     | reuse (pointer shape only if a capture API gives raw shapes) |
| `transport/Tcp.cpp`                   | Winsock, qWAVE   | small: POSIX sockets; drop qWAVE or use `SO_PRIORITY`/DSCP |
| `common/Clock.h`, `common/Log.h`      | QPC, Win32       | small: `clock_gettime(CLOCK_MONOTONIC)` / `mach_absolute_time` |
| `Server.cpp`                          | D3D types in `Pipeline`, Win32 threads/sleep | **refactor**: split session logic from the pipeline |
| `display/`, `capture/`, `render/`, `encode/MfH264Encoder`, `input/TouchInjector`, `wireless/` | Windows-only | **rewrite per OS** |

The host tests (`host/tests/`) for the protocol, AnnexB, touch and adb are portable too, and
should run on all three OSes in CI.

---

## Part 3: Proposed structure

Turn `RunSession` into platform-neutral code that depends on a handful of interfaces, one
implementation per OS:

```
 host/
   core/                        platform-neutral (C++20, no OS headers)
     protocol/  AnnexB  TouchTracker  AdbProtocol  Session.cpp (today's RunSession)
   platform/
     IVirtualDisplay   Plug(PlugRequest) -> index | Unplug()      (lifetime = session)
     ICapture          Acquire(timeout) -> Frame{handle, pts, changed} | Release()
     IEncoder          Encode(frame) -> Annex-B AUs | RequestKeyframe()
     ITouchSink        Inject(TouchFrame) | LiftAll()
     IDisplayControl   SetOrientation(deg) | WaitUntilExtended()
     ISecretStore      Load/Save pairing code
     crypto            HmacSha256, RandomBytes
   platform/windows/    today's code, moved behind the interfaces (no behaviour change)
   platform/linux/
   platform/macos/
```

Rules for the refactor (from CLAUDE.md):

- **Don't add a copy or a queue to make the interfaces neat.** Frames must stay on the GPU
  (D3D11 texture / DMA-BUF / IOSurface) from capture to encoder. `ICapture` can hand over an
  opaque GPU handle that only the same platform's `IEncoder` understands.
- **Keep Windows behaviour identical and measured.** Re-run the latency table in
  [latency-notes.md](latency-notes.md) after moving the Windows code behind the interfaces.
- **Keep the Windows build on MSBuild** (driver and CI depend on it). Add CMake for the new
  platforms, sharing `core/`.
- **Licensing:** no GPL in the host. LGPL system libraries linked dynamically (PipeWire, libva,
  libsecret) need an explicit decision, since today's rule is "Windows SDK only". Record it in
  CLAUDE.md when it's made.

---

## Part 4: Linux notes

### Virtual monitor: pick per desktop stack

| Stack | Mechanism | Pros | Cons |
|-------|-----------|------|------|
| GNOME (Mutter, Wayland) | Remote desktop / ScreenCast with a **virtual** source type; Mutter creates a virtual monitor of a requested size (verify version and API surface) | No root, no kernel module; capture comes along through PipeWire | GNOME-only API details; user consent prompt |
| KDE Plasma (KWin, Wayland) | KWin virtual outputs, exposed through the portal / `krfb-virtualmonitor` (verify) | Same shape as GNOME | KDE-only |
| wlroots (sway, Hyprland…) | `swaymsg create_output` (headless output), then `output HEADLESS-1 mode WxH@Hz` | Simple, scriptable | Compositor-specific commands |
| X11 | `xrandr --newmode/--addmode` on a spare or VIRTUAL output (Intel), or the `dummy` driver | Mature | X11 is legacy; varies by GPU driver |
| Any (kernel) | **EVDI** (DisplayLink's virtual connector module; module is GPL, `libevdi` is LGPL) | Real DRM connector: works on every desktop, EDID-driven like IddCx | Out-of-tree kernel module (DKMS), root to install; licence decision needed |

EVDI is the closest analogue to IddCx: you hand it an EDID and a connector appears. The
per-compositor virtual outputs avoid a kernel module. A pragmatic plan is to **start with one
compositor API** and put EVDI behind the same `IVirtualDisplay` as a fallback.

### Capture

- **Wayland:** `xdg-desktop-portal` ScreenCast → a PipeWire video stream of the chosen output.
  Request DMA-BUF buffers so frames stay on the GPU; set the cursor mode to *embedded* so the
  compositor draws it (no `FrameComposer` needed). Consent is once per session unless the portal
  supports persistent restore tokens (verify per portal version).
- **X11:** XDamage for change notification, XShm/XComposite for pixels. CPU copies, so it's
  slower. Fallback only.
- **EVDI:** `libevdi` hands you the connector's framebuffer directly.

### Encode

- **VA-API** (`libva`, MIT) covers Intel and AMD. Import the DMA-BUF as a VA surface, run
  VPP for RGB→NV12, encode H.264/HEVC with low-delay settings (no B-frames, CBR).
- **NVENC** on NVIDIA through the Video Codec SDK headers (verify header licence; the
  `nv-codec-headers` packaging is MIT). Import via CUDA/EGL interop.
- Avoid FFmpeg/GStreamer in the frame path. It's banned for licence reasons (CLAUDE.md), and it
  adds buffering you'd have to fight.

### Touch input

- **uinput** multitouch (protocol type B: `ABS_MT_SLOT`, `ABS_MT_TRACKING_ID`, `ABS_MT_POSITION_X/Y`)
  creates a real touchscreen device. Needs write access to `/dev/uinput` (udev rule for the
  user's group). Map it to the virtual output: `xinput map-to-output` on X11; per-compositor
  config on Wayland (e.g. sway `input … map_to_output`).
- **Portal RemoteDesktop** has `NotifyTouchDown/Motion/Up` with a stream-relative position,
  which targets the right output without mapping config and needs no udev rule (verify
  compositor support). libei is the newer route.

### Everything else

- adb: identical protocol; spawn `adb start-server` with `posix_spawn`.
- Secrets: libsecret (Secret Service), with a `0600` file under `$XDG_CONFIG_HOME` as fallback.
- Autostart: a `systemd --user` service. A udev rule can nudge it when a tablet's USB appears.
- Wi-Fi AP: NetworkManager hotspot (`nmcli device wifi hotspot`, verify band/channel control) or
  `hostapd`. The bind-only-to-own-AP rule still applies.

---

## Part 5: macOS notes

### Virtual monitor

macOS has **no public virtual-display API**. Options:

1. **`CGVirtualDisplay`** (private CoreGraphics class). Several third-party tools use it to
   create virtual monitors with a chosen size, refresh rate and physical size (verify behaviour
   on current macOS). It's the direct analogue of `PLUG`, but private APIs can break between
   releases and aren't allowed in the Mac App Store.
2. **A DriverKit display driver.** No suitable public family exists for this *(verify)*. Not
   realistic.
3. **HDMI/USB-C dummy plug.** Zero code, a real monitor for capture. A useful fallback and for
   early testing of capture/encode before the virtual display works.

Recommendation: prototype with option 3 to build capture → encode → stream, then add option 1
behind `IVirtualDisplay`, isolated in one file, loaded dynamically and failing gracefully.

### Capture: ScreenCaptureKit

- `SCShareableContent` → find the `SCDisplay` for the virtual monitor → `SCStream` with
  `SCStreamConfiguration` (`width/height`, `minimumFrameInterval`, `queueDepth` small,
  `pixelFormat = 420v` NV12, `showsCursor = true`).
- Frames arrive as `CMSampleBuffer`s backed by IOSurface, so they stay on the GPU with no colour
  conversion or cursor compositing.
- Requires the **Screen Recording** permission (TCC). The host must be a signed app bundle, or
  the permission prompt gets attached to the terminal.

### Encode: VideoToolbox

`VTCompressionSession` with `kVTVideoEncoderSpecification_EnableLowLatencyRateControl`,
`kVTCompressionPropertyKey_RealTime = true`, `AllowFrameReordering = false` (no B-frames),
`MaxKeyFrameInterval` large plus forced keyframes on request. Output is AVCC (length-prefixed):
**convert to Annex-B** before sending, because the wire protocol requires it
(`AnnexB.cpp` shows the parameter-set split that follows).

### Touch

macOS has no public way to inject touchscreen input. Start with **mouse emulation** via
`CGEventPost`: one finger → move/click/drag, two fingers → scroll. That needs the
**Accessibility** permission. The wire protocol doesn't change, because the host decides how to
interpret `INPUT`.

### Everything else

- Rotation: skip at first (always landscape). `CONFIG.rotation` = 0.
- Secrets: Keychain. Autostart: a LaunchAgent. Wi-Fi: USB only at first.
- Distribution: signing + notarisation to get stable TCC permissions.

---

## Part 6: First milestones (suggested)

| # | Milestone | Proves |
|---|-----------|--------|
| 0 | Move Windows host code behind the interfaces; latency unchanged | The seams are right |
| 1 | `core/` + tests build with CMake on Linux and macOS in CI | Portable core |
| 2 | Linux: stream an **existing** monitor (like `--output`) over USB with VA-API | Capture + encode + transport |
| 3 | Linux: create a virtual output on one compositor, stream it, unplug on disconnect | Full USB loop |
| 4 | Linux: touch via portal or uinput | Input |
| 5 | macOS: stream an existing/dummy-plug display via ScreenCaptureKit + VideoToolbox | Capture + encode |
| 6 | macOS: `CGVirtualDisplay` behind `IVirtualDisplay`; mouse emulation | Full USB loop |

For each milestone, add a row to [latency-notes.md](latency-notes.md) with the device and host
used, and state what was verified by hand (CLAUDE.md, "Hardware-in-the-loop").
