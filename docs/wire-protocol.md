# DeuxDisplay Wire Protocol (v1)

The host (Windows) is the **server**; the display client (Android) is the **client**.
The protocol is transport-agnostic: it only assumes an ordered, reliable byte stream.
v1 runs over TCP tunnelled through ADB (`adb reverse`), see [architecture.md](architecture.md).

> **Security:** the protocol has no authentication or encryption. The host must only ever
> bind to the loopback interface (`127.0.0.1`). Any future network transport (Wi-Fi, LAN)
> must add authentication before it ships.

## Conventions

- All integers are **little-endian**.
- Strings are UTF-8, prefixed by a `u16` byte length, not NUL-terminated.
- Timestamps are microseconds from the **sender's** monotonic clock unless stated otherwise.
- Unknown message types must be skipped (the length field makes this possible), which keeps
  minor additions backward compatible.

## Framing

Every message is a 16-byte header followed by `length` bytes of payload.

| Offset | Size | Field        | Notes                                               |
|-------:|-----:|--------------|-----------------------------------------------------|
| 0      | 1    | `type`       | Message type, see table below                       |
| 1      | 1    | `flags`      | Type-specific bit flags                             |
| 2      | 2    | `reserved`   | Must be 0                                           |
| 4      | 4    | `length`     | Payload length in bytes, max 16 MiB (`0x01000000`)  |
| 8      | 8    | `timestamp`  | `u64` µs, sender clock (for video: capture time)    |

Senders must write header and payload with a single write/send call (see latency notes).

## Message types

| Type   | Name              | Direction        | Purpose                                        |
|-------:|-------------------|------------------|------------------------------------------------|
| `0x01` | `HELLO`           | client → host    | First message; client capabilities             |
| `0x02` | `CONFIG`          | host → client    | Stream parameters chosen by host               |
| `0x03` | `BYE`             | either           | Graceful close                                 |
| `0x10` | `VIDEO_FRAME`     | host → client    | One encoded access unit                        |
| `0x11` | `REQUEST_KEYFRAME`| client → host    | Ask for an IDR (decoder reset, corruption)     |
| `0x20` | `PING`            | either           | Clock sync / RTT probe                         |
| `0x21` | `PONG`            | either           | Reply to `PING`                                |
| `0x22` | `FRAME_STATS`     | client → host    | Per-frame receive/decode/render timestamps     |
| `0x30` | `CURSOR`          | host → client    | Reserved (cursor overlay, post-v1)             |
| `0x40` | `INPUT`           | client → host    | Reserved (touch/pen passthrough, M5)           |

### `HELLO` (client → host)

| Size | Field               | Notes                                                    |
|-----:|---------------------|----------------------------------------------------------|
| 4    | `magic`             | ASCII `DXDP` (`0x50445844` as LE u32)                    |
| 2    | `protocol_version`  | `1`                                                      |
| 2    | `width_px`          | Native panel width in current orientation                |
| 2    | `height_px`         | Native panel height in current orientation               |
| 2    | `density_dpi`       | Android `densityDpi`                                     |
| 4    | `refresh_mhz`       | Panel refresh rate in millihertz (e.g. 90000)            |
| 4    | `codecs`            | Bitmask of decodable codecs: bit0 = H.264, bit1 = HEVC   |
| str  | `device_name`       | e.g. `OnePlus OPD2305`                                   |
| 4    | `xdpi_milli`        | *Optional.* Physical horizontal DPI × 1000 (Android `xdpi`) |
| 4    | `ydpi_milli`        | *Optional.* Physical vertical DPI × 1000 (Android `ydpi`)   |

The host plugs a virtual monitor matching `width_px` × `height_px` at `refresh_mhz`, with a
physical size derived from `xdpi_milli`/`ydpi_milli` (falling back to `density_dpi`), so Windows
picks sensible scaling for any device.

### `CONFIG` (host → client)

| Size | Field               | Notes                                         |
|-----:|---------------------|-----------------------------------------------|
| 2    | `protocol_version`  | Version the host will speak (≤ client's)      |
| 1    | `codec`             | `0` = H.264, `1` = HEVC                       |
| 1    | `reserved`          | 0                                             |
| 2    | `width_px`          | Encoded frame width                           |
| 2    | `height_px`         | Encoded frame height                          |
| 4    | `fps_mhz`           | Target frame rate in millihertz               |
| 4    | `bitrate_kbps`      | Target bitrate                                |

If the host cannot serve the client (version/codec mismatch) it sends `BYE` instead.

The host sends `CONFIG` again mid-session if the stream size or rate changes (e.g. the user
switches the virtual display's mode). The client must reconfigure its decoder and wait for the
next `CODEC_CONFIG` + keyframe.

### `VIDEO_FRAME` (host → client)

Payload is one complete access unit in **Annex-B** format (start-code delimited NAL units).
`timestamp` is the host capture time of the frame.

| Flag bit | Meaning                                                   |
|---------:|-----------------------------------------------------------|
| 0        | `KEYFRAME` — access unit is an IDR                        |
| 1        | `CODEC_CONFIG` — payload contains only SPS/PPS (/VPS)     |
| 2        | `REPEAT` — re-encode of the previous image (see below)    |

`REPEAT` frames exist because some hardware decoders (e.g. MediaTek) only output frame N once
frame N+1 has been submitted. Right after each frame the host sends a cheap re-encode of the
same image (almost entirely skip blocks), so the real frame leaves the decoder immediately.
Clients decode them like any other frame; they're excluded from `FRAME_STATS`.

The host sends a `CODEC_CONFIG` frame before the first keyframe and whenever parameters change.
Frames arrive at a variable rate: the host only sends when the desktop changed.

### `REQUEST_KEYFRAME` (client → host)

Empty payload. The host responds by forcing an IDR on the next encoded frame.

### `PING` / `PONG`

`PING` payload: `u64 ping_id`. The header `timestamp` is the sender's clock at send time.
`PONG` payload: `u64 ping_id`, `u64 ping_timestamp` (echo of the ping header timestamp).
The `PONG` header `timestamp` is the responder's clock. This gives RTT and a clock-offset
estimate (NTP-style, assuming symmetric paths) used to put `FRAME_STATS` on the host timeline.

### `FRAME_STATS` (client → host)

| Size | Field            | Notes                                                        |
|-----:|------------------|--------------------------------------------------------------|
| 8    | `capture_ts`     | Echo of the `VIDEO_FRAME` header timestamp                   |
| 8    | `received_ts`    | Last payload byte read                                       |
| 8    | `decoded_ts`     | Decoder output buffer available                              |
| 8    | `rendered_ts`    | Frame shown on the panel (Android `OnFrameRenderedListener`) |

All four are on the **host clock**. The client converts its own timestamps using the offset
from `PING`/`PONG` (it pings the host periodically and keeps the minimum-RTT sample). A value of
0 means unknown. Clients may sample (e.g. every Nth frame) to limit overhead.

## Session flow

```
client                              host
  | ---- TCP connect -------------->  |
  | ---- HELLO -------------------->  |
  | <--- CONFIG --------------------  |
  | <--- VIDEO_FRAME (CODEC_CONFIG) - |
  | <--- VIDEO_FRAME (KEYFRAME) ----  |
  | <--- VIDEO_FRAME ... -----------  |
  | ---- PING / FRAME_STATS ------->  |
  | ---- BYE ---------------------->  |
```

## Versioning

`protocol_version` is bumped for incompatible changes. New message types or appended
payload fields that old peers can ignore do not require a bump.
