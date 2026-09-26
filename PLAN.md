# DeuxDisplay Plan

Goal: use an Android tablet as an ultra-low-latency **extended** display for Windows 11 over a
wired USB-C connection. The reference device is a OnePlus Pad Go (2408×1720, 90 Hz), but nothing
should be hard-coded to it beyond default EDID values.

See [docs/architecture.md](docs/architecture.md) for the design and
[docs/wire-protocol.md](docs/wire-protocol.md) for the protocol.

## Decisions (settled)

| Area            | Decision                                                                  |
|-----------------|---------------------------------------------------------------------------|
| Virtual monitor | IddCx indirect display driver derived from Microsoft's MIT sample         |
| Capture         | DXGI Desktop Duplication on the virtual output only                       |
| Encode          | Media Foundation hardware H.264 MFT, low-latency mode, no B-frames        |
| Host language   | Native C++20, Windows SDK only (no FFmpeg, no vendor SDKs)                |
| Transport       | TCP over `adb reverse`, loopback only, `TCP_NODELAY`                      |
| Client          | Kotlin, MediaCodec async decode → SurfaceView                             |
| Codec           | H.264 for v1                                                              |
| License         | MIT; `driver/` is MS-PL because it derives from Microsoft's sample        |
| Driver signing  | Local self-signed cert for dev (UMDF: no test-signing mode needed)        |
| Display lifetime| Host creates a software device; monitor exists only while host runs      |

## v1 non-goals

Audio, HDR, multiple displays/devices, dynamic rotation, and a signed end-user installer.
Input passthrough is the stretch milestone M5.

## Milestones

### M0 — Repo scaffold
- [x] Repo layout, license, contributor docs, CLAUDE.md
- [x] Architecture, wire protocol and latency-notes docs
- [x] Dev bootstrap script (JDK, Android SDK, adb installed user-locally)
- [x] Host project builds (enumerates adapters/outputs) with protocol unit tests
- [x] Android project builds (full-screen SurfaceView) with protocol unit tests; verified on OPD2305
- [x] CI: host (build + tests), android (build + lint + tests), Dependabot
- [x] Public GitHub repo `zengarv/DeuxDisplay`

### M1 — Virtual display visible
- [x] Driver sources derived from the IddCx sample, renamed to `DeuxDisplayIdd` (MS-PL)
- [x] EDID generator + mode list for 2408×1720 @ 60/90 Hz with correct physical size; unit tested
- [x] Builds via WDK NuGet packages + VS WDK component (64-bit MSBuild); compile-only CI job
- [x] Install (local cert + pnputil) and uninstall scripts
- [x] Host `--create-display` (software device) to attach/detach the monitor
- [x] Driver package installs (local cert, `pnputil`), verified on the dev PC
- [x] **No admin at runtime:** persistent device created at install; the host plugs/unplugs the
      monitor via device-interface IOCTLs; the driver unplugs it when the host's handle closes
      (verified by killing the host)
- [x] Monitor built from the client's HELLO (size, refresh, physical mm → EDID + modes); verified
      with 2408×1720 and 1920×1200 clients
- [x] Host switches duplicate → extend topology automatically when Windows mirrors the new monitor
- [x] **Acceptance:** 2408×1720 monitor appears as an extended display (`\\.\DISPLAY39`, 200% scaling)

### M2 — Capture → encode → dummy receiver
- [x] Protocol (de)serialisation + unit tests (host C++ and Android Kotlin, shared test vector)
- [x] Desktop Duplication of the virtual output (matched by monitor ID `DXD0001`), `ACCESS_LOST` recovery
- [x] GPU cursor overlay (DDA frames exclude the pointer) + BGRA→NV12 via D3D11 video processor
- [x] MF hardware H.264 encoder, low-latency configuration (Intel Quick Sync verified)
- [x] Loopback TCP server, `--serve` session loop, SPS/PPS split into `CODEC_CONFIG`
- [x] `tools/dump_receiver.py`: speaks the protocol, writes `.h264`, reports capture→receive latency
- [x] `--output \\.\DISPLAYn` debug mode to test the pipeline on a physical monitor without the driver
- [x] **Acceptance:** recorded stream plays back correctly (ffprobe: 146/146 frames, cursor visible)

### M3 — Live video on the tablet
- [x] Android protocol client (HELLO with real size/refresh/physical DPI) + MediaCodec decode to
      Surface (`c2.mtk.avc.decoder` on the Pad Go; MTK low-latency hint; fallback config)
- [x] `scripts/run.ps1`: `adb reverse`, install/launch the app, run the host
- [x] Decoder-error recovery via `REQUEST_KEYFRAME` (observed once at startup, recovered);
      automatic reconnect loop; stream lifetime tied to the Surface
- [x] **Acceptance:** Windows' extended desktop (wallpaper, cursor) renders live on the OnePlus Pad Go
- [ ] Hide the gesture-navigation handle; handle the "no input buffer" drop at decoder start more
      gracefully (wait for the first input buffer instead of dropping the IDR)
- [ ] Detect/avoid a second adb version (another tool's v40 server kills ours and the reverse tunnel)

### M4 — Measure and tune
- [x] PING/PONG clock sync and FRAME_STATS instrumentation; host logs end-to-end p50/p95
- [x] Frame pacing (`--max-fps`, default 60), stream scaling (`--max-stream-size`), HEVC
      (`--codec auto|h264|hevc`), adb-reverse watchdog in `run.ps1`
- [x] Diagnosed the MediaTek decoder's fixed ~2-frame hold (see docs/latency-notes.md)
- [ ] Client-side cursor overlay via `CURSOR` messages (biggest perceived-latency win left)
- [ ] Keep the encoder's GPU clocks up between bursty frames
- [ ] Tune encoder, TCP, `ADB_BURST_MODE`, decoder flags; record in `docs/latency-notes.md`
- [ ] 90 Hz mode
- [ ] **Acceptance:** p50 capture→render < 50 ms

### M5 — Input passthrough (stretch)
- [ ] Touch/pen → `INPUT` messages → `SendInput`/pointer injection mapped to the virtual display
