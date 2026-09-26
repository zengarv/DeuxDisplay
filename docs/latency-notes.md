# Latency Notes

A running log of measured numbers and the tuning settings that produced them. Add a row
whenever something changes the end-to-end number.

## Budget (target, 60 Hz)

| Stage                               | Target   |
|-------------------------------------|----------|
| Desktop composition → capture       | ≤ 1 frame (16.7 ms), usually less |
| HW encode (low-latency, no B-frames)| 2–5 ms   |
| Transport (adb tunnel, USB)         | 1–5 ms   |
| HW decode (low-latency mode)        | 2–8 ms   |
| Render / scan-out on tablet         | ≤ 1 frame |
| **End-to-end goal (v1)**            | **< 50 ms; first milestone < 100 ms** |

## Knobs

- Host: `TCP_NODELAY`, single `send()` per message, `CODECAPI_AVLowLatencyMode`,
  `CODECAPI_AVEncMPVDefaultBPictureCount = 0`, CBR/low-delay VBR, GOP length.
- ADB: `ADB_BURST_MODE=1` (experimental), USB 3 port/cable.
- Android: `KEY_LOW_LATENCY`, `KEY_PRIORITY = 0`, `KEY_OPERATING_RATE`,
  `releaseOutputBuffer(index, true)` immediately (no timestamp pacing).

## Measurements

Host-side numbers come from `DeuxDisplayHost --serve` logs plus `tools/dump_receiver.py` on the
same PC. Both use QPC, so "capture→receive" is exact. That path has no USB/decode, so it's the
floor for what the tablet can see.

| Date | Setup | Setting change | Stage breakdown (avg) | Capture→receive p50 / p95 |
|------|-------|----------------|-----------------------|---------------------------|
| 2026-09-26 | Intel Arc (Meteor Lake) QSV MFT, physical 3200×2000@120 via `--output`, bursty desktop (~18 fps) | baseline | acquire 5–10, compose 2.5–3, **encode 23–24**, send 0.3 ms | 27.0 / 45.0 ms |
| 2026-09-26 | same | `QualityVsSpeed = 0` | encode 20 (all processing, 0 input wait) | 24.5 / 39.6 ms |
| 2026-09-26 | same | `Flush()` after compose | no change | 24.2 / 38.6 ms |
| 2026-09-26 | **Virtual monitor** 2408×1720@60 (IddCx), mostly static desktop | — | acquire 1.7, compose 3.2, **encode 12.2**, send 0.2 | 17.5 / 20.2 ms |
| 2026-09-26 | Virtual monitor 1920×1200@60 | — | acquire 4.5, compose 2.1, encode 10.6, send 0.2 | 14.3 / 24.7 ms |
| 2026-09-27 | **OnePlus Pad Go over adb**, 2408×1720@60, animated wallpaper (sustained 50–110 fps) | — | acquire 0.5–0.9, compose 1.5, **encode 5.9**, send 0.2 → **present→sent 8.0–8.7 ms** | (tablet-side timing: M4) |

Sustained load drops encode from ~12–20 ms to ~6 ms, which strongly suggests the earlier numbers
were GPU/media-engine clock ramp-up under bursty load. M4 lead: keep the media engine warm, or
submit frames at a steady cadence.

## End-to-end telemetry (2026-09-27, OnePlus Pad Go over adb)

From `FRAME_STATS` with PING/PONG clock sync, in ms after Windows presented the frame (p50):

| Config | received | decoded | Notes |
|--------|---------:|--------:|-------|
| H.264 2408×1720, uncapped (up to 110 fps) | 14–26 | 57 → 88 (growing) | decoder queue grows: over its ~60 fps capacity |
| H.264 2408×1720, **60 fps cap** | ~23 | ~60 | stable; ~37 ms inside the decoder |
| H.264 2016×1440 (within decoder spec) | ~25 | ~58 | resolution isn't the cause |
| HEVC 2016×1440 | ~23 | ~56 | codec isn't the cause |
| HEVC 2016×1440, surfaceless decode | ~23 | ~56 | display back-pressure isn't the cause |
| HEVC 2016×1440 + REPEAT frames | ~19 | ~49 | small gain, doubles decode work |
| H.264 2408×1720 + REPEAT frames | 115 | 227 | **overloads the decoder: don't** |
| Software decoder `c2.android.avc.decoder` | 20–80 | 76 → 214 | far too slow on Helio G99 |

Findings:
- `c2.mtk.avc.decoder`/`c2.mtk.hevc.decoder` always hold **~2 frames** (~30 ms at typical rates).
  `KEY_LOW_LATENCY`, `vdec-lowlatency`, disabling OPPO VPP, 1 reference frame, AUD boundaries,
  in-spec resolution, HEVC, and surfaceless output all left it unchanged. This looks like the
  MediaTek LAT/CORE decode pipeline; it can't be tuned away through MediaCodec.
- Both MTK decoders are rated for ≤ 2560×1440; H.264 manages ~60 fps at 2408×1720. **Never send
  more frames than the client can decode** (hence `--max-fps`, default 60).
- `OnFrameRenderedListener` never fires on this device, so on-screen time is unmeasured (est.
  +1–2 vsync).
- Remaining budget (p50): encode ~10–12, capture ~4, transport ~5, decode ~30, display ~16–25.

Next levers, by expected perceived gain:
1. **Draw the cursor on the tablet** (`CURSOR` messages + an overlay view). Pointer motion then
   skips encode/decode entirely, leaving roughly transport + one vsync. This is most of the
   "sluggish" feel.
2. Keep the GPU media engine clocked up (encode 10–12 ms bursty vs ~6 ms sustained).
3. Try other tablets/decoders. Qualcomm/Exynos decoders usually honor low-latency mode.

## Wi-Fi

Echo tests (`nc -L cat` on the tablet, TCP_NODELAY client on the PC at 60–200 Hz), OnePlus Pad Go
(Wi-Fi 5, 1x1, 433 Mbit/s max) and the dev PC (Intel AX211), 2026-09-27. Round trip = PC→tablet→PC.

| Path | Setup | 64 B p50 / p95 | 32–100 KB p50 / p95 | Throughput at p50 |
|------|-------|---------------:|--------------------:|------------------:|
| Router (PC and tablet on DEEPNET5, ch 36) | no app lock (tablet power save on) | 5.4 / 46 ms | 31.5 / 97 ms (32 KB) | ~16 Mbit/s |
| **PC access point** (legacy Wi-Fi Direct GO, ch 149/157/161) | PC also on home Wi-Fi ch 36 | 2.1–3.2 / 62–84 ms | 9.5 / 43–76 ms (32 KB) | ~54 Mbit/s |
| PC access point | app's low-latency lock held (tablet power save off) | 3.2 / 62 ms | 14.5 / 82 ms (100 KB) | ~111 Mbit/s |
| PC access point | PC disconnected from home Wi-Fi (no channel sharing) | 3.1 / 63 ms | 15.5 / 80 ms (100 KB) | ~103 Mbit/s |

Findings:
- The direct link is ~2x faster at the median and 3–7x higher in throughput than the router path,
  so it's the right transport. Windows picks the access point's channel itself (149–161 here, no
  API to choose); it didn't share the home network's channel 36.
- **The tail is set by a periodic gap on the PC side:** spikes of ~60 ms arrive every 102 ms (one
  beacon interval, 100 TU), about 1 in 12 packets at 200 Hz. They persist with the tablet's power
  save off, with the PC off its home Wi-Fi, with WLAN media streaming mode on / background scans
  off, and with the publisher non-discoverable. The likely cause is the Intel AX211 group owner's
  absence schedule (Notice of Absence), which Windows doesn't expose.
- Streaming the desktop over the link today (H.264 2408x1720, 5–15 Mbit/s bursty): present→received
  p50 56–60 ms, p95 90–107 ms, vs ~23 ms over USB. The host's send time rises to 10–70 ms when a
  frame hits a gap.
- Next: a newer Intel driver; the tablet as access point (Android LocalOnlyHotspot) with the PC
  joining as a client; the router path with the app's low-latency lock held.

### Open leads for M4
- Encode takes ~20 ms at 6.4 MP even on the fastest preset, with no input wait. Next suspects:
  GPU clock ramp-up under bursty load, MFT internal async depth
  (`CODECAPI_AVEncNumWorkerThreads`, `MF_TRANSFORM_ASYNC`/sample queue), HRD buffer size
  (`CODECAPI_AVEncCommonBufferSize`) with CBR, slices (`CODECAPI_AVEncSliceControl*`).
- Measure at the real virtual-display size (2408×1720, 4.1 MP) and at a steady 60 fps.
- Present→acquire (5–10 ms) is DWM/DDA overhead; compare with capturing in the IDD swap chain.
- The encoder doesn't write `matrix_coefficients` into the VUI (ffprobe: `color_space=unknown`).
  The Android side should assume BT.709 limited.
