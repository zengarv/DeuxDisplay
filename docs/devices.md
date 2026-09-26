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
| HW AVC decoder     | `c2.mtk.avc.decoder` (alias `OMX.MTK.VIDEO.DECODER.AVC`)       |
| HW HEVC decoder    | `c2.mtk.hevc.decoder`                                          |
| SW fallbacks       | `c2.android.avc.decoder`, `c2.android.hevc.decoder`            |

Observed at runtime (M3, 2026-09-27):
- `c2.mtk.avc.decoder` does **not** advertise `FEATURE_LowLatency`. The app still sets
  `KEY_PRIORITY=0` and MediaTek's `vdec-lowlatency=1` hint, and configure accepts them.
- Decodes 2408×1720 High-profile H.264 at 60+ fps. Output format: BT.709 (`color-standard=1`),
  limited range (`color-range=2`), SDR transfer.
- The app's `HELLO`: 2408×1720, 60 Hz, `densityDpi` 360, physical 260.047 × 260.268 dpi,
  which gives a 235 × 168 mm virtual monitor. Windows picks 200% scaling.
