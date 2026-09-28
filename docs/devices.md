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

## Nexus 7 (2013) — `flo` (legacy, best effort)

| Property           | Value                                                          |
|--------------------|----------------------------------------------------------------|
| Android            | 6.0.1 (API 23), build `MOB30X`, last official update           |
| SoC                | Qualcomm Snapdragon S4 Pro (`msm8960`), `armeabi-v7a`          |
| Native panel       | 1200 × 1920 (portrait native), **1920 × 1200 landscape**       |
| Density            | 320 (`densityDpi`), physical ≈ 321 dpi                         |
| Refresh modes      | 60 Hz                                                          |
| HW AVC decoder     | `OMX.qcom.video.decoder.avc` (`media_codecs.xml` limit 1920 × 1088) |
| HW HEVC / VP9      | none                                                           |

Observed at runtime (2026-09-29):
- `VideoCapabilities` claims 64–1920 for both width and height, but `configure()` at 1920×1200
  fails with `Set Resolution failed` (-1010). The app therefore test-configures each size it
  offers; here the options are 1440×900, 1280×800 and 960×600, and Auto picks 1440×900.
- `KEY_PRIORITY` and `KEY_OPERATING_RATE` are accepted by configure but ignored by the OMX
  component ("does not support config priority / operating rate"). No `FEATURE_LowLatency`.
- Rated 47 fps at 1440×900. The first `dequeueInputBuffer` after start times out, so the first
  packets (including SPS/PPS) are dropped; the client keeps them and resends before the next
  keyframe.
- Usable at 1440×900 @ 30 Hz (≈ 90 ms to screen); see `latency-notes.md`. The micro-USB port
  drops the adb link when the cable moves.
