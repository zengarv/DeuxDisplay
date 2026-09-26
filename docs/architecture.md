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
monitor whose EDID/mode list describes the target tablet (OnePlus Pad Go: 2408×1720, 60/90 Hz).
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
- **transport/** — loopback-only TCP listener, `TCP_NODELAY`, header+payload in one send.
- **protocol/** — message (de)serialisation for [wire-protocol.md](wire-protocol.md).

### Android client — `android/`
Kotlin, minSdk 26, targets API 35. A single full-screen, orientation-locked activity with a
`SurfaceView`. `MediaCodec` in async mode decodes straight to that surface.
`KEY_LOW_LATENCY` is only set when the decoder advertises `FEATURE_LowLatency`, and
configuration falls back to a plain config if the vendor codec rejects low-latency keys.

### Transport
`adb reverse tcp:<port> tcp:<port>` lets the tablet connect to `127.0.0.1:<port>` and reach
the host listener over USB. This needs nothing beyond USB debugging, and it is the same
mechanism scrcpy relies on. The protocol is transport-agnostic, so USB tethering (RNDIS) or the
Android Open Accessory protocol could replace ADB later without touching frame handling.

## Security notes

- The streaming socket binds to 127.0.0.1 only and is unauthenticated. See
  [wire-protocol.md](wire-protocol.md).
- The driver's device object grants read/write to interactive users (INF `Security` SDDL), so
  any logged-in user can plug/unplug the DeuxDisplay monitor. That's harmless (it's their own
  desktop) and it's what lets the host run without admin rights.
- `install-driver.ps1` trusts a locally generated code-signing certificate machine-wide. Its
  private key is non-exportable; `uninstall-driver.ps1 -RemoveCertificate` removes it.

## v1 constraints (intentional)

- Exactly one ADB device attached (scripts target it via `adb -s <serial>`).
- One virtual display, one fixed landscape mode, SDR only.
- No audio. Input passthrough (M5) covers touch only: `INPUT` touch frames are injected with
  Windows touch injection on the virtual display; pen is not forwarded yet.
- The cursor is composited into the video frame. A separate cursor channel (`CURSOR` message)
  is reserved for later if cursor latency needs to be decoupled from video.

## Behaviour on disruption

| Event                         | v1 behaviour                                                   |
|-------------------------------|----------------------------------------------------------------|
| Tablet unplugged / app closed | Host detects socket close, unplugs the monitor, waits for reconnect |
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
