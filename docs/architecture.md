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
An Indirect Display Driver built on IddCx (user-mode, UMDF 2), derived from Microsoft's
MIT-licensed IddCx sample (see [`driver/THIRD_PARTY_NOTICES.md`](../driver/THIRD_PARTY_NOTICES.md)).
It creates one virtual monitor whose EDID/mode list describes the target tablet
(OnePlus Pad Go: 2408×1720, 60/90 Hz). Windows composes the extended desktop onto it like any
real monitor. The driver's swap-chain processor simply drains frames; the host captures them
through Desktop Duplication.

Local installs use test signing (`scripts/setup-testsigning.ps1`, `scripts/install-driver.ps1`).
Attestation signing for end users is a possible later step, not a v1 goal.

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

## v1 constraints (intentional)

- Exactly one ADB device attached (scripts target it via `adb -s <serial>`).
- One virtual display, one fixed landscape mode, SDR only.
- No audio, no input passthrough (input is milestone M5).
- The cursor is composited into the video frame. A separate cursor channel (`CURSOR` message)
  is reserved for later if cursor latency needs to be decoupled from video.

## Behaviour on disruption

| Event                         | v1 behaviour                                                   |
|-------------------------------|----------------------------------------------------------------|
| Tablet unplugged / app closed | Host detects socket close, stops encoding, waits for reconnect |
| Windows lock / UAC / mode set | Duplication recreated; client gets a fresh keyframe            |
| Decoder error on client       | Client resets decoder and sends `REQUEST_KEYFRAME`             |
| Virtual display disabled      | Host waits for the output to reappear                          |

## Alternatives considered

- **Writing a WDDM/IddCx driver from scratch**: a forked sample gets the same result for far
  less work.
- **FFmpeg for encoding**: redundant with MF's hardware MFT dispatch, and hardware-encoder
  builds tend to pull in GPL/non-free flags that conflict with an MIT release.
- **C#/.NET host**: GC pauses in the per-frame hot loop and a second debugging model.
- **HEVC**: USB bandwidth isn't the constraint, and low-latency AVC decode is more uniformly
  supported on Android.
- **Reading frames directly inside the IDD swap-chain processor** (skipping Desktop
  Duplication): potentially one fewer copy, but it needs cross-process texture sharing out of
  WUDFHost. Worth revisiting in M4 if capture shows up in the latency budget.
