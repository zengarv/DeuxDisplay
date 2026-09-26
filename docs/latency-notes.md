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

| Date | Commit | Mode | Setting change | Capture→render (p50 / p95) | Notes |
|------|--------|------|----------------|----------------------------|-------|
|      |        |      |                |                            |       |
