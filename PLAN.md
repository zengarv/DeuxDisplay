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
- [x] Detect/avoid a second adb version (another tool's v40 server kills ours and the reverse tunnel):
      the host's adb watcher reconnects to whatever server runs and re-applies the tunnel (M7)

### M4 — Measure and tune
- [x] PING/PONG clock sync and FRAME_STATS instrumentation; host logs end-to-end p50/p95
- [x] Frame pacing (`--max-fps`, default 60), stream scaling (`--max-stream-size`), HEVC
      (`--codec auto|h264|hevc`), adb-reverse watchdog in `run.ps1`
- [x] Diagnosed the MediaTek decoder's fixed ~2-frame hold (see docs/latency-notes.md)
- [x] Stream resolution/frame-rate picker in the app (panel modes + decodable scaled sizes, plus a
      30 fps stream-only rate); sent in `HELLO`, host plugs and streams exactly that mode
      (verified on the Pad Go: requested 2408×1720 @ 60 streamed as picked)
- [x] Codec picker (Auto / H.264 / HEVC) via HELLO's `codecs` bitmask; Auto keeps HEVC within the
      decoder's rated sizes, an explicit HEVC pick is honored at any size
- [ ] Client-side cursor overlay via `CURSOR` messages (biggest perceived-latency win left)
- [ ] Keep the encoder's GPU clocks up between bursty frames
- [ ] Tune encoder, TCP, `ADB_BURST_MODE`, decoder flags; record in `docs/latency-notes.md`
- [ ] 90 Hz mode
- [ ] **Acceptance:** p50 capture→render < 50 ms

### M6 — Wireless (direct Wi-Fi link)
Branch `feature/wireless`. The tablet joins a network the PC runs itself (one hop, no router);
USB stays available and the app picks the connection. Design: docs/architecture.md, Transport.
- [x] Option study: LAN, PC access point, Wi-Fi Direct P2P, Miracast, adb over Wi-Fi
      (PC access point wins; measurements in docs/latency-notes.md)
- [x] Protocol: `AUTH_CHALLENGE`/`AUTH_RESPONSE`/`AUTH_OK` (mutual HMAC) and `PAIRING`;
      pairing-code derivations with shared test vectors (host + Android)
- [x] Host: Wi-Fi Direct legacy access point (unpackaged, WRL), listener bound to its address,
      USB + Wi-Fi served together, WLAN tuning + QoS during Wi-Fi sessions, `--transport`,
      `--pair [--reset]`, DPAPI pairing store
- [x] Android: Connection picker, `WifiNetworkSpecifier` join, low-latency `WifiLock`,
      auth handshake, pairing over USB or typed code
- [x] Scripts: `enable-wireless.ps1` (firewall), `run.ps1 -Transport`
- [x] **Verified on the Pad Go:** pair over USB, then stream and reconnect over Wi-Fi
- [ ] Fix the per-beacon gap: the Windows/AX211 group owner is away ~60 ms of every 102 ms
      (p95 ~60 ms even at 100 Mbit/s). Try: a newer Intel driver, the tablet as access point
      with the PC as client, and the home LAN with the tablet's low-latency lock held
- [ ] Encoder for Wi-Fi: cap frame sizes, intra refresh instead of large IDRs, bitrate adaptation
- [ ] Wi-Fi touch/pen check on hardware; UDP video + FEC only if TCP stalls show up in p95

### M7 — Plug and play (USB)
Plug the tablet in with the app open and it becomes a monitor, like a cable to a real one.
- [x] Host `adb/` watcher: `host:track-devices` → `adb reverse` on every attach, re-applied
      after re-plug / USB mode switch / adb server restart; starts the server; `--no-adb`, `--adb`
- [x] App retries USB connections every 250 ms
- [x] `DeuxDisplayAgent.exe` tray app (hidden host, restart on exit, kill-on-close job, log file)
      and `install-autostart.ps1` (per-user Run key)
- [x] **Verified on the Pad Go:** USB mode switches (charging ↔ file transfer) recover in ~1 s
      with nothing run by hand; recovers from `adb kill-server`; killing the agent removes the monitor
- [ ] Verify a physical unplug/re-plug and a login autostart on hardware
- [ ] Tray icon artwork and a "Connect over Wi-Fi" toggle in the tray menu
- [ ] Optional: Android Open Accessory transport (no USB debugging needed, app opens on plug-in)

### M5 — Input passthrough (stretch)
- [x] Multi-touch (10 contacts) → `INPUT` touch frames → `InjectTouchInput` mapped to the virtual
      display; host-side contact tracking lifts lost/vanished contacts; `--no-touch` to disable
      (unit tested; not yet verified on hardware)
- [ ] Verify touch on the Pad Go + Windows (taps, drags, pinch; long holds with no movement)
- [ ] Fix held touches: Windows cancels an injected contact that isn't refreshed for ~100 ms
      (`InjectTouchInput` → `ERROR_TIMEOUT` 1460, then 87 for every later update), and Android
      sends nothing while a finger is still. Re-inject active contacts every ~50 ms on the host,
      and treat a cancelled contact as a new down (seen on the Pad Go, 2026-09-27)
- [ ] Pen (pressure/tilt/hover) via synthetic pointer devices (`PT_PEN`)

### M8 — Rotation
- [x] App follows the tablet's rotation (`fullUser`) and sends `ORIENTATION`; host rotates the
      Windows display (`ChangeDisplaySettingsEx`, no admin); `CONFIG` carries the rotation and the
      app applies it with `MediaCodec` `KEY_ROTATION` (frames stay landscape on the wire)
- [x] Pointer drawn rotated into the scan-out frame (`render/Rotation.h`, unit tested)
- [x] **Verified on the Pad Go:** portrait ↔ landscape rotates Windows' display and the stream
      shows upright; note DDA reports the rotated size in `ModeDesc` but hands over scan-out-sized
      textures
- [ ] Confirm pointer position/orientation in portrait by eye; touch in portrait
