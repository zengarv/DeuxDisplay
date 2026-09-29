# Tested Devices

Facts gathered from real hardware (via `adb shell dumpsys display`, `wm size`, and the
`/vendor/etc/media_codecs*.xml` files). Use these values for EDID defaults and decoder
selection. Don't guess them.

## OnePlus Pad Go — `OPD2305` (reference device)

| Property           | Value                                                          |
|--------------------|----------------------------------------------------------------|
| Android            | 14 (API 34)                                                    |
| SoC                | MediaTek (Helio G99 family)                                    |
| Native panel       | 1720 × 2408 (portrait native), **2408 × 1720 landscape**       |
| Density            | 360 (`densityDpi`), physical ≈ 260 dpi                         |
| Physical size      | ≈ 168 × 235 mm (portrait) → **235 × 168 mm** landscape         |
| Refresh modes      | 90, 60, 50, 48 Hz (default mode 90 Hz; was running 60 Hz)      |
| HDR                | none                                                           |
| HW AVC decoder     | `c2.mtk.avc.decoder` (alias `OMX.MTK.VIDEO.DECODER.AVC`): Baseline, Main, High |
| HW HEVC decoder    | `c2.mtk.hevc.decoder`: Main only                              |
| HW VP9 decoder     | `c2.mtk.vp9.decoder`: profiles 0 and 1 (1 = 8-bit 4:4:4)      |
| Decoder rating     | 2560 × 1440 max for all three (the native size is out of spec but decodes fine) |
| SW fallbacks       | `c2.android.avc.decoder`, `c2.android.hevc.decoder`            |

Observed at runtime (M3, 2026-09-27):
- `c2.mtk.avc.decoder` does **not** advertise `FEATURE_LowLatency`. The app still sets
  `KEY_PRIORITY=0` and MediaTek's `vdec-lowlatency=1` hint, and configure accepts them.
- Decodes 2408×1720 High-profile H.264 at 60+ fps. Output format: BT.709 (`color-standard=1`),
  limited range (`color-range=2`), SDR transfer.
- The app's `HELLO`: 2408×1720, 60 Hz, `densityDpi` 360, physical 260.047 × 260.268 dpi,
  which gives a 235 × 168 mm virtual monitor. Windows picks 200% scaling.

Observed 2026-09-28 (details and numbers in [latency-notes.md](latency-notes.md)):
- **Refresh rate:** ColorOS keeps the panel at 60 Hz unless the screen is touched, even with a
  90 Hz stream, the app's `preferredDisplayModeId` for the 90 Hz mode and `Surface.setFrameRate(90)`
  (both reach SurfaceFlinger). No public Android 14 API overrides it, and adb can't change the
  system refresh settings (`WRITE_SETTINGS` denied to the shell).
- **Render timestamps:** the MediaTek decoders' `OnFrameRendered` times are on a clock 15–30 h
  behind `System.nanoTime` (the gap grows with time asleep). The app falls back to the
  callback's arrival time.
- **Decode latency:** ~32 ms inside the HEVC decoder with `KEY_OPERATING_RATE` at maximum
  (~37 ms without), unchanged by resolution, bitrate, low-latency keys or vendor parameters.
  VP9 decodes ~7 ms slower than HEVC.
- **Vendor parameters** (`c2.mtk.hevc.decoder`, 31 in all): none of
  `vendor.mtk.ext.vdec.vilte.feature-on`, `vendor.mtk.vdec.bq.guard.interval.time.value`,
  `vendor.mtk.vdec.cpu.boost.mode.value` or `vendor.mtk.vdec.oplus.media.sched.mode.value` lowers
  latency.
- **Full range:** decoded correctly with the colour range set in the bitstream and the format.
