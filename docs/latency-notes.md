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

### Open leads for M4
- Encode takes ~20 ms at 6.4 MP even on the fastest preset, with no input wait. Next suspects:
  GPU clock ramp-up under bursty load, MFT internal async depth
  (`CODECAPI_AVEncNumWorkerThreads`, `MF_TRANSFORM_ASYNC`/sample queue), HRD buffer size
  (`CODECAPI_AVEncCommonBufferSize`) with CBR, slices (`CODECAPI_AVEncSliceControl*`).
- Measure at the real virtual-display size (2408×1720, 4.1 MP) and at a steady 60 fps.
- Present→acquire (5–10 ms) is DWM/DDA overhead; compare with capturing in the IDD swap chain.
- The encoder doesn't write `matrix_coefficients` into the VUI (ffprobe: `color_space=unknown`).
  The Android side should assume BT.709 limited.
