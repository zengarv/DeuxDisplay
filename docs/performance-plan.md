# Performance plan

How to make DeuxDisplay feel faster, ranked by expected gain. The numbers come from
[latency-notes.md](latency-notes.md) (OnePlus Pad Go, USB, 2026-09-27). Any change made from
this plan must be measured and logged there (CLAUDE.md: "Latency is the product").

---

## Part 1: Intuition

"Performance" here means **latency**: the time from something changing on the PC to it showing
on the tablet. Throughput is not the problem. USB carries 200+ Mbit/s and the stream uses
~30 Mbit/s.

Glass to glass is about **60–90 ms**. Before optimising, look at where that time goes:

| Stage | p50 | Who owns it | Can we move it? |
|---|---:|---|---|
| Hardware decode (tablet) | ~30 ms | MediaTek decoder, holds ~2 frames | **No, on this tablet.** Every MediaCodec knob was tried |
| Display (tablet) | ~16–25 ms | Android compositor + vsync | Only by skipping it for the cursor |
| Encode (PC) | ~10–12 ms | GPU media engine | **Yes**: ~6 ms when the GPU is kept busy |
| Transport (USB via adb) | ~2–5 ms | adb relay | Little to gain |
| Capture (PC) | ~4 ms | DWM / Desktop Duplication | Maybe, at a high engineering cost |

Two lessons follow from the table:

1. **The biggest cost is fixed by the tablet's decoder.** Choosing the device matters more than
   any setting on it. An Android phone with a non-MediaTek decoder reached *decoded* in ~27 ms,
   against ~63 ms for the Pad Go in the same test.
2. **What users feel as "sluggish" is mostly the mouse pointer.** Today the pointer is drawn into
   the video, so every pointer move pays for encode, transport, decode and display. Taking the
   pointer out of the video is the largest perceived win available.

---

## Part 2: Settings that help today (no code)

- **Use USB, not Wi-Fi.** Present→received is ~23 ms over USB and 56–60 ms over Wi-Fi. Wi-Fi also
  has ~60 ms spikes every ~102 ms, traced to the PC's Wi-Fi card.
- **Keep the 60 fps cap** (`--max-fps`, default 60). Sending more frames than the decoder can
  handle makes latency grow without bound (decoded 57 → 88 ms, uncapped).
- **Don't use `--repeat-frames` with H.264 at full resolution.** It doubles decode work and
  overloaded the decoder (decoded 227 ms). With HEVC at 2016×1440 it gave only 56 → 49 ms.
- **Resolution and codec don't matter on the Pad Go.** Lower resolution and HEVC both landed at
  ~56–58 ms decoded. They may matter on other devices.
- **Try another tablet.** Qualcomm and Exynos decoders usually honour low-latency mode.
- USB 3 port and cable (small effect; transport is ~5% of the total).

---

## Part 3: Code changes, ranked

### 1. Draw the pointer on the tablet (largest perceived gain)

**Idea:** send pointer position and shape separately from the video, and draw it in an overlay
view on the tablet. Pointer motion then costs about transport + one vsync instead of the full
video path.

- Protocol: `CURSOR` (`0x30`, host → client) is already reserved in
  [wire-protocol.md](wire-protocol.md). Define its payload: position (x, y normalised like
  `INPUT`), visibility, and a shape update (hotspot + BGRA image, sent only when it changes).
- Host: Desktop Duplication already delivers pointer position and shape
  (`capture/PointerShape.cpp`). Stop compositing it in `render/FrameComposer.cpp` when the
  client supports the cursor channel, and send `CURSOR` from the frame loop whenever the pointer
  changes, without waiting for an encode.
- App: an overlay `View` (or a second `SurfaceView` layer) above the video surface, moved on the
  UI thread with no queue in between. Advertise support in `HELLO` (new capability bit), so old
  clients keep the composited cursor.
- Watch out for: rotation (the pointer must follow `CONFIG.rotation`), scaling when the stream size
  differs from the panel, and the pointer disappearing during touch input.
- Measure: pointer-move latency before and after (for example a high-speed camera on both
  screens), plus the usual `FRAME_STATS`.

### 2. Keep the GPU's media engine warm (encode 10–12 ms → ~6 ms)

**Idea:** encode takes ~6 ms under steady load, but 10–12 ms (up to ~20 ms) when the desktop
changes only occasionally. That suggests GPU clock ramp-up.

- Experiment: while the desktop is idle, submit the last frame to the encoder at a steady cadence,
  but **don't send** the output (extra frames would overload the tablet's decoder). Compare encode
  times on the first frame after an idle period.
- Cost: GPU power while idle. Consider keeping it warm only for a short time after the last change.
- Alternative: a power-management hint (for example a high-performance power request) if one
  measurably helps.

### 3. Tune the encoder (a few ms, each needs measuring on its own)

Candidates from [latency-notes.md](latency-notes.md#open-leads-for-m4), all in
`encode/MfH264Encoder.cpp`:

- Internal async depth: `CODECAPI_AVEncNumWorkerThreads`, the MFT sample queue.
- HRD buffer size with CBR: `CODECAPI_AVEncCommonBufferSize` (a smaller buffer means less
  frame-size variation to absorb).
- Slices: `CODECAPI_AVEncSliceControlMode/Size`.
- Also fix: the encoder doesn't write `matrix_coefficients` in the VUI. It isn't a latency issue,
  but it's cheap to fix while in there.

### 4. Capture inside the driver instead of through Desktop Duplication (high cost)

**Idea:** the driver's swap-chain processor already receives every frame and throws it away
(`driver/DeuxDisplayIdd/Driver.cpp`, `SwapChainProcessor::RunCore`). Handing that surface to the
host would skip the DWM → Desktop Duplication step (present→acquire measured at 0.5–10 ms).

- Needs cross-process GPU texture sharing out of `WUDFHost.exe` (shared handles + keyed mutexes or
  fences), and a new host capture path.
- The pointer must then be handled separately (it's not in the swap-chain image either). Plan 1
  fits well with this.
- Do this last, and only if capture shows up as a significant part of the budget after 1–3.

### 5. Wi-Fi only: remove the periodic gaps

- The ~60 ms spikes every beacon interval most likely come from the Intel AX211's absence
  schedule as group owner, which Windows doesn't expose.
- Try a newer Intel driver.
- Try the tablet as the access point (Android `LocalOnlyHotspot`) with the PC joining as a client.
  The bind-only-to-the-link and pairing rules in CLAUDE.md still apply.
- Try the router path with the app's low-latency Wi-Fi lock held.

---

## Part 4: Where the app spends time (for reference)

The app does almost no per-frame work, so it isn't where the time goes:

```
 socket ─► StreamClient (network thread): read header + payload into a reused buffer
        ─► VideoDecoder.submit(): dequeueInputBuffer → copy → queueInputBuffer
        ─► hardware decoder (~30 ms on the Pad Go) writes into the SurfaceView's buffer
        ─► renderLoop (URGENT_DISPLAY thread): releaseOutputBuffer(index, true) immediately
        ─► SurfaceFlinger shows it at the next vsync (scaling and rotation happen there)
```

Reading and copying take well under 1 ms. Pixels never pass through app memory. Ideas worth
trying on the app side are about the decoder and the display, not the Kotlin code:

- Test other decoders and devices (debug extra `--es debug_decoder <name>` in `MainActivity`).
- Where supported, check whether a higher panel refresh (90/120 Hz) shortens the wait for the
  next vsync. `MainActivity.applyPanelRefresh()` already sets the panel rate.

---

## Order of work

| # | Item | Expected gain | Cost |
|---|---|---|---|
| 1 | Pointer drawn on the tablet (`CURSOR`) | Pointer lag ~60–90 → ~10–25 ms | Medium: protocol + host + app |
| 2 | Warm media engine | Encode −4 to −6 ms on bursty desktops | Low: experiment first |
| 3 | Encoder tuning | A few ms, unknown | Low per knob |
| 4 | Capture in the driver | 0.5–10 ms | High |
| 5 | Wi-Fi gap removal | p95 on Wi-Fi only | Medium, hardware-dependent |

The gains for 1 and 2 are estimates from the measurements above, not results. Record the real
numbers in [latency-notes.md](latency-notes.md) as each item lands, with the PC and tablet used.
