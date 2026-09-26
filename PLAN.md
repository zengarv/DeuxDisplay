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
| License         | MIT                                                                       |

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
- [ ] Driver sources derived from the IddCx sample, renamed to `DeuxDisplayIdd`
- [ ] EDID + mode list for 2408×1720 @ 60/90 Hz with correct physical size
- [ ] Builds via WDK NuGet packages (no WDK install needed); compile-only CI job
- [ ] Test-signing, install and uninstall scripts
- [ ] **Acceptance:** monitor appears in Display Settings at native resolution; windows can be dragged onto it

### M2 — Capture → encode → dummy receiver
- [x] Protocol (de)serialisation + unit tests (host C++ and Android Kotlin, shared test vector)
- [ ] Desktop Duplication of the virtual output, `ACCESS_LOST` recovery
- [ ] MF hardware H.264 encoder, low-latency configuration
- [ ] Loopback TCP server
- [ ] `tools/dump_receiver.py`: speaks the protocol, writes `.h264` playable by ffplay/VLC
- [ ] **Acceptance:** recorded stream plays back correctly

### M3 — Live video on the tablet
- [ ] Android protocol client + MediaCodec low-latency decode to Surface
- [ ] `scripts/run.ps1`: `adb reverse`, launch host and app
- [ ] Decoder-error recovery via `REQUEST_KEYFRAME`, reconnect on disconnect
- [ ] **Acceptance:** moving a window on the virtual display is visible on the tablet

### M4 — Measure and tune
- [ ] PING/PONG clock sync and FRAME_STATS instrumentation; latency overlay/log
- [ ] Tune encoder, TCP, `ADB_BURST_MODE`, decoder flags; record in `docs/latency-notes.md`
- [ ] 90 Hz mode
- [ ] **Acceptance:** p50 capture→render < 50 ms

### M5 — Input passthrough (stretch)
- [ ] Touch/pen → `INPUT` messages → `SendInput`/pointer injection mapped to the virtual display
