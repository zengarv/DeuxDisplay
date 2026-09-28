# DeuxDisplay Wire Protocol (v1)

The host (Windows) is the **server**; the display client (Android) is the **client**.
The protocol is transport-agnostic: it only assumes an ordered, reliable byte stream.
It runs over TCP, either tunnelled through ADB (`adb reverse`, "USB") or over a direct Wi-Fi
link to an access point the host runs itself ("Wi-Fi"), see [architecture.md](architecture.md).

> **Security:** the protocol has no encryption of its own. The host binds only to
> - the loopback interface (`127.0.0.1`, reached through the USB tunnel), where sessions are
>   unauthenticated, and
> - the address of its own Wi-Fi Direct access point. That link is WPA2-encrypted with a
>   passphrase derived from the pairing code, and every session must first pass the
>   [authentication handshake](#authentication-wi-fi).
>
> A transport over a shared network (e.g. the home LAN) would also need per-message encryption
> and must not ship without it.

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
| `0x04` | `AUTH_CHALLENGE`  | host → client    | Wi-Fi only: first message, host nonce          |
| `0x05` | `AUTH_RESPONSE`   | client → host    | Wi-Fi only: client nonce + proof of the key    |
| `0x06` | `AUTH_OK`         | host → client    | Wi-Fi only: host's proof of the key            |
| `0x07` | `PAIRING`         | host → client    | USB only: the pairing code for Wi-Fi           |
| `0x10` | `VIDEO_FRAME`     | host → client    | One encoded access unit                        |
| `0x11` | `REQUEST_KEYFRAME`| client → host    | Ask for an IDR (decoder reset, corruption)     |
| `0x20` | `PING`            | either           | Clock sync / RTT probe                         |
| `0x21` | `PONG`            | either           | Reply to `PING`                                |
| `0x22` | `FRAME_STATS`     | client → host    | Per-frame receive/decode/render timestamps     |
| `0x30` | `CURSOR`          | host → client    | Reserved (cursor overlay, post-v1)             |
| `0x31` | `MEDIA_STATE`     | host → client    | Windows volume and play/pause state            |
| `0x32` | `ENCODER_STATE`   | host → client    | Encoder's live bitrate and quality             |
| `0x40` | `INPUT`           | client → host    | Touch input on the stream                      |
| `0x41` | `ORIENTATION`     | client → host    | Desktop orientation the client wants           |
| `0x42` | `ACTION`          | client → host    | A shortcut from the client's dock              |
| `0x43` | `ENCODER_SETTINGS`| client → host    | The user's bitrate/quality picks, applied live |
| `0x44` | `DISPLAY_MODE`    | client → host    | Switch the virtual monitor's mode, live        |

### `HELLO` (client → host)

| Size | Field               | Notes                                                    |
|-----:|---------------------|----------------------------------------------------------|
| 4    | `magic`             | ASCII `DXDP` (`0x50445844` as LE u32)                    |
| 2    | `protocol_version`  | `1`                                                      |
| 2    | `width_px`          | Native panel width in current orientation                |
| 2    | `height_px`         | Native panel height in current orientation               |
| 2    | `density_dpi`       | Android `densityDpi`                                     |
| 4    | `refresh_mhz`       | Panel refresh rate in millihertz (e.g. 90000)            |
| 4    | `codecs`            | Bitmask of codecs to use: bit0 = H.264, bit1 = HEVC, bit2 = VP9. Clients may advertise a subset of what they can decode to express the user's codec pick; the host prefers HEVC, then H.264, then VP9 |
| str  | `device_name`       | e.g. `OnePlus OPD2305`                                   |
| 4    | `xdpi_milli`        | *Optional.* Physical horizontal DPI × 1000 (Android `xdpi`) |
| 4    | `ydpi_milli`        | *Optional.* Physical vertical DPI × 1000 (Android `ydpi`)   |
| 2    | `mode_width_px`     | *Optional.* Stream width the user picked; 0 = host decides  |
| 2    | `mode_height_px`    | *Optional.* Stream height the user picked; 0 = host decides |
| 4    | `mode_refresh_mhz`  | *Optional.* Frame rate the user picked (mHz); 0 = host decides |
| 4    | `bitrate_kbps`      | *Optional.* Encoder bitrate the user picked; 0 = host decides (see `ENCODER_SETTINGS`) |
| 1    | `encoder_quality`   | *Optional.* 0 (fastest) – 100 (best quality); `0xFF` = host decides |
| 1    | `encoder_flags`     | *Optional.* Stream options, see below                    |
| 1    | `size_count`        | *Optional.* Stream sizes that follow                     |
| 4×n  | `sizes`             | *Optional.* `u16` width, `u16` height (landscape): every stream size the client can show |
| 1    | `rate_count`        | *Optional.* Frame rates that follow                      |
| 2×n  | `rates`             | *Optional.* `u16` Hz: every frame rate the client can show |

`encoder_flags` (also in `ENCODER_SETTINGS`):

| Bit | Option          | Meaning                                                                 |
|----:|-----------------|-------------------------------------------------------------------------|
| 0   | Full range      | YCbCr 0–255 instead of 16–235 (BT.709 either way): a little more contrast, same cost. `CONFIG` confirms it, and the bitstream signals it too (for VP9 the host sets the keyframe header's `color_range` bit, since Intel's encoder doesn't) |
| 1   | Sharp refresh   | Once the screen has been still for 120 ms, the host encodes the last image 3 more times, sent as `REPEAT` frames. Encoders code the difference to their own lossy copy, so each pass restores detail the frame budget dropped; a converged image costs a few bytes. Nothing extra is sent while the screen changes |

The host plugs a virtual monitor matching `width_px` × `height_px` at `refresh_mhz`, with a
physical size derived from `xdpi_milli`/`ydpi_milli` (falling back to `density_dpi`), so Windows
picks sensible scaling for any device.

The virtual monitor offers **every combination** of `sizes` × `rates` (up to 32 modes), so the
user can switch modes in Windows' Settings > Display, or from the client with `DISPLAY_MODE`,
without a new monitor being plugged. Windows renders the desktop at the chosen mode and the host
streams it as is (no host-side scaling; the client scales to its panel), sending `CONFIG` when it
changes. Without the lists, only the native size at the preferred rates is offered.

The `mode_*` fields let the user pick the stream format on the client. Each group is independent:
`mode_width_px`/`mode_height_px` (both non-zero, landscape, even) or `mode_refresh_mhz` may be set
on its own. The picked mode is the monitor's preferred mode, and the host switches the monitor to
it after plugging (Windows would otherwise restore the mode it last used for that monitor). With
no pick, the preferred mode is the native size at `refresh_mhz` capped by `--max-fps`, and
whatever mode Windows restores stands.

- The physical size always comes from the native `width_px`/`height_px`, so Windows scaling
  stays correct for the real panel.
- The stream follows the monitor's mode, so a picked or Windows-chosen mode overrides the host's
  `--max-stream-size` / `--max-fps` limits (which only shape the default).
- A value the host can't serve (outside 640×480–7680×4320, or 24–240 Hz) is ignored with a log line.

Clients only offer modes the device supports: panel sizes and refresh rates, plus scaled-down
panel sizes the decoder accepts. The optional fields are positional: a client sending `mode_*`
must also send `xdpi_milli`/`ydpi_milli`, one sending the encoder fields must send `mode_*`, and
one sending the lists must send the encoder fields.

### `CONFIG` (host → client)

| Size | Field               | Notes                                         |
|-----:|---------------------|-----------------------------------------------|
| 2    | `protocol_version`  | Version the host will speak (≤ client's)      |
| 1    | `codec`             | `0` = H.264, `1` = HEVC, `2` = VP9 (profile 0, 8-bit 4:2:0) |
| 1    | `reserved`          | 0                                             |
| 2    | `width_px`          | Encoded frame width                           |
| 2    | `height_px`         | Encoded frame height                          |
| 4    | `fps_mhz`           | Target frame rate in millihertz               |
| 4    | `bitrate_kbps`      | Target bitrate                                |
| 2    | `rotation`          | *Optional.* Degrees clockwise (0/90/180/270) the client rotates each frame to show it upright; 0 if absent |
| 1    | `flags`             | *Optional.* bit0 = full range (YCbCr 0–255; else 16–235). Clients configure the decoder's color range from it |
| 1    | `reserved`          | *Optional.* 0                                 |

Frames are always encoded in the display's native **scan-out** orientation (landscape), even when
Windows has rotated the display: Desktop Duplication delivers them that way, and the client's
compositor rotates them for free when showing the surface. `width_px`/`height_px` are therefore
the scan-out size, not the rotated desktop size.

If the host cannot serve the client (version/codec mismatch) it sends `BYE` instead.

The host sends `CONFIG` again mid-session if the stream size or rate changes (e.g. the user
switches the virtual display's mode). The client must reconfigure its decoder and wait for the
next `CODEC_CONFIG` + keyframe.

### `VIDEO_FRAME` (host → client)

Payload is one complete access unit: for H.264/HEVC in **Annex-B** format (start-code delimited
NAL units), for VP9 one frame (or superframe) as the encoder produced it. `timestamp` is the host
capture time of the frame.

| Flag bit | Meaning                                                   |
|---------:|-----------------------------------------------------------|
| 0        | `KEYFRAME` — access unit is an IDR (VP9: a keyframe)      |
| 1        | `CODEC_CONFIG` — payload contains only SPS/PPS (/VPS)     |
| 2        | `REPEAT` — re-encode of the previous image (see below)    |

`REPEAT` frames re-encode the image already on screen: sharp-refresh passes (see
`encoder_flags`), or, with the host's `--repeat-frames`, a cheap re-encode right after each frame
for hardware decoders that only output frame N once frame N+1 has been submitted. Clients decode
them like any other frame; they're excluded from `FRAME_STATS`.

For H.264/HEVC the host sends a `CODEC_CONFIG` frame before the first keyframe and whenever
parameters change. VP9 has no parameter sets: every keyframe is self-contained.
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

### `ORIENTATION` (client → host)

| Size | Field     | Notes                                                                     |
|-----:|-----------|---------------------------------------------------------------------------|
| 2    | `degrees` | Desktop orientation, clockwise from native landscape: 0 or 90 (180/270 flipped) |

Sent right after `HELLO` and whenever the client device rotates. The host rotates the Windows
display to match (like Settings > Display > Orientation), capture restarts, and the host sends a
new `CONFIG` whose `rotation` tells the client how to show the frames. Touch coordinates keep
mapping onto the (now rotated) desktop, because the client normalizes them to its upright view.

### `INPUT` (client → host)

Touch on the client, forwarded so the host can inject it on the virtual display. The header
`timestamp` is the client send time.

| Size | Field      | Notes                                                           |
|-----:|------------|-----------------------------------------------------------------|
| 1    | `kind`     | `1` = touch frame. Hosts ignore kinds they don't know            |
| 1    | `count`    | Contacts that follow, 1–10                                      |
| 2    | `reserved` | 0                                                               |

Then `count` contacts, 8 bytes each:

| Size | Field      | Notes                                                              |
|-----:|------------|--------------------------------------------------------------------|
| 1    | `id`       | Contact slot, 0–9. Stable from down to up                          |
| 1    | `action`   | `0` down, `1` move, `2` up, `3` cancel                             |
| 2    | `x`        | `u16`: 0 = left edge of the video frame, 65535 = right edge        |
| 2    | `y`        | `u16`: 0 = top edge, 65535 = bottom edge                           |
| 2    | `pressure` | `u16`, 0–1024; 0 = unknown                                         |

A touch frame carries **every** contact currently on the screen, like Android's `MotionEvent`
and Windows touch injection. Coordinates are normalized to the video frame, which the client shows
stretched over its whole surface, so they don't depend on the stream size. The host keeps
per-contact state: a contact that is active on the host but missing from a frame is lifted, and
all contacts are lifted when the session ends, so a dropped message can't leave a finger stuck.

### `ACTION` (client → host)

A button on the client's shortcut dock, or a volume key. The client names the action; the host
decides how to carry it out, so clients don't depend on Windows key codes.

| Size | Field    | Notes                               |
|-----:|----------|-------------------------------------|
| 1    | `action` | See below. Hosts ignore unknown values |

| Value | Action       | Host does                                         |
|------:|--------------|---------------------------------------------------|
| 1–3   | *Retired*    | Ignored. Cut, copy and paste in 0.2.1; Windows touch already offers them |
| 4     | Undo         | Ctrl+Z                                            |
| 5     | Redo         | Ctrl+Y                                            |
| 6     | Task view    | Win+Tab                                           |
| 7     | Play/pause   | Media play/pause key                              |
| 8     | Volume up    | Windows volume one step (5 %) up, unmuting        |
| 9     | Volume down  | Windows volume one step down; reaching 0 mutes    |

Keys go to the active window, as from a keyboard. Volume changes the default playback device
directly (no key press, so Windows shows no volume flyout).

### `MEDIA_STATE` (host → client)

| Size | Field      | Notes                                                      |
|-----:|------------|------------------------------------------------------------|
| 1    | `playback` | `0` no media session, `1` paused/stopped, `2` playing      |
| 1    | `flags`    | bit0 = muted                                               |
| 1    | `volume`   | Windows volume, 0–100                                      |
| 1    | `reserved` | 0                                                          |

`playback` is the media session the play/pause key controls. The host sends `MEDIA_STATE` right
after `CONFIG` and then whenever it changes. A host sends it only if it accepts `ACTION`, so
clients show their dock (and take over the volume keys) only after receiving one.

### `DISPLAY_MODE` (client → host)

| Size | Field        | Notes                                                  |
|-----:|--------------|--------------------------------------------------------|
| 2    | `width_px`   | Landscape width; 0 = the monitor's preferred width     |
| 2    | `height_px`  | 0 = the monitor's preferred height                     |
| 2    | `refresh_hz` | 0 = the monitor's preferred rate                       |
| 2    | `reserved`   | 0                                                      |

The host switches the virtual monitor to this mode as Windows' display settings would (keeping
its orientation). It must be one the monitor offers (see `HELLO` `sizes`/`rates`); others are
ignored with a log line. The stream then restarts at the new size and rate, announced by `CONFIG`.

### `ENCODER_SETTINGS` (client → host)

| Size | Field          | Notes                                                           |
|-----:|----------------|-----------------------------------------------------------------|
| 4    | `bitrate_kbps` | Bitrate to encode at; 0 = host decides                          |
| 1    | `quality`      | Encoder quality/speed preset, 0 (fastest) – 100; `0xFF` = host decides |
| 1    | `flags`        | `encoder_flags` (see `HELLO`)                                   |

Same meaning as the `HELLO` fields, but mid-session. The host applies bitrate and quality within
a fraction of a second, starting with a keyframe, without a new `CONFIG`; sharp refresh at once.
Toggling full range rebuilds the stream like a mode change, announced by `CONFIG`. A host that decides the bitrate itself runs it **adaptively**
(below). Hosts clamp bitrates to 500–500 000 kbit/s.

### `ENCODER_STATE` (host → client)

| Size | Field          | Notes                                                    |
|-----:|----------------|----------------------------------------------------------|
| 4    | `bitrate_kbps` | Bitrate the encoder is running at now                    |
| 1    | `flags`        | bit0 = adaptive (the host adjusts `bitrate_kbps` itself); bits 1–7 = the `encoder_flags` in effect (bit1 full range, bit2 sharp refresh) |
| 1    | `quality`      | Encoder quality/speed preset in use, 0–100               |
| 2    | `reserved`     | 0                                                        |

Sent right after `CONFIG` and whenever any of these change. `CONFIG.bitrate_kbps` is only the
starting bitrate.

### Adaptive bitrate

When neither the client nor the host's `--bitrate` fixes the bitrate, the host tunes it from
`FRAME_STATS`: *delivery* = host send → `decoded_ts`, the transfer and decode that the bitrate
drives. Every second of motion (15+ frames) the host compares the median delivery with the
lowest median of the last 20 such seconds (the baseline):

- above the baseline by more than max(3 ms, 20 %): the link or the decoder is falling behind, so
  cut the bitrate by 25 % (at most every 2 s; no increase for 4 s after);
- otherwise, if frames used ≥ 60 % of their CBR budget (more bits would show): raise it by
  10 %, at most every 10 s, and not to within 10 % of the bitrate of the last cut except for
  one re-probe a minute.

Hosts may apply a new bitrate or quality by building a new encoder (some hardware encoders ignore
changes mid-stream), so each change can cost a keyframe; the controller settles instead of
hunting.

The range is 5–150 Mbit/s over USB and 5–60 Mbit/s over Wi-Fi, starting at 30. Idle desktops
send nothing, so the bitrate only moves while there is motion to measure it on.

## Pairing and authentication

### Pairing code

Wi-Fi sessions rely on a secret shared by host and client: the **pairing code**, 20 characters
of the RFC 4648 Base32 alphabet (`A`–`Z`, `2`–`7`, 100 random bits), shown as
`XXXXX-XXXXX-XXXXX-XXXXX`. Parsers accept lower case and ignore `-` and spaces; the
*normalized* code is the 20 upper-case characters. The host generates it once and keeps it.
It reaches the client either automatically in a `PAIRING` message over USB, or by the user typing it.

Everything else is derived with HMAC-SHA256, keyed with the ASCII bytes of the normalized code:

| Value            | Derivation                                                                  |
|------------------|-----------------------------------------------------------------------------|
| Network name     | `DeuxDisplay-` + lower-case hex of the first 2 bytes of `HMAC(code, "deuxdisplay ssid")` |
| WPA2 passphrase  | RFC 4648 Base32 (upper case, 24 chars) of the first 15 bytes of `HMAC(code, "deuxdisplay wpa2")` |
| `auth_key`       | `HMAC(code, "deuxdisplay auth")` (32 bytes)                                  |

Test vector: code `ABCDE-FGHIJ-KLMNO-PQRST` → network `DeuxDisplay-9968`, passphrase
`3252SPVCLUVNTF5LBJDW4VOV`, `auth_key` `29a36685…b996a64e` (full values in the unit tests).

### `PAIRING` (host → client, USB only)

| Size | Field       | Notes                                   |
|-----:|-------------|-----------------------------------------|
| str  | `code`      | Pairing code, `XXXXX-XXXXX-XXXXX-XXXXX` |
| str  | `host_name` | PC name, for display                    |

Sent right after `CONFIG` on USB sessions. The client stores it, replacing any older code. It is
never sent over Wi-Fi.

### Authentication (Wi-Fi)

On a Wi-Fi connection the host speaks first, and nothing else is accepted until the handshake
succeeds:

1. Host → `AUTH_CHALLENGE`: `host_nonce` (16 random bytes).
2. Client → `AUTH_RESPONSE`: `client_nonce` (16 random bytes), then
   `mac = HMAC(auth_key, "DXDP-C" ‖ host_nonce ‖ client_nonce)` (32 bytes).
3. Host checks `mac` in constant time. On success → `AUTH_OK`:
   `HMAC(auth_key, "DXDP-H" ‖ client_nonce ‖ host_nonce)` (32 bytes). The client checks it too,
   which proves the host knows the code as well.
4. The client sends `HELLO` and the session continues as on USB.

`"DXDP-C"`/`"DXDP-H"` are the 6 ASCII bytes. If the response is wrong, any other message
arrives first, or no response arrives within 5 s, the host sends `BYE` and closes the
connection. The client closes on a wrong `AUTH_OK`. USB (loopback) sessions skip the handshake.

## Session flow

```
client                              host
  | ---- TCP connect -------------->  |
  | <--- AUTH_CHALLENGE ------------  |   (Wi-Fi only)
  | ---- AUTH_RESPONSE ------------>  |   (Wi-Fi only)
  | <--- AUTH_OK -------------------  |   (Wi-Fi only)
  | ---- HELLO -------------------->  |
  | <--- CONFIG --------------------  |
  | <--- PAIRING -------------------  |   (USB only)
  | <--- VIDEO_FRAME (CODEC_CONFIG) - |
  | <--- VIDEO_FRAME (KEYFRAME) ----  |
  | <--- VIDEO_FRAME ... -----------  |
  | ---- PING / FRAME_STATS ------->  |
  | <--- MEDIA_STATE ---------------  |   (if the host takes ACTION)
  | ---- INPUT (touch) ------------>  |
  | ---- ACTION ------------------->  |
  | <--- ENCODER_STATE -------------  |   (after CONFIG, and on changes)
  | ---- ENCODER_SETTINGS --------->  |   (user moves a slider)
  | ---- DISPLAY_MODE ------------->  |   (user picks another mode)
  | <--- CONFIG --------------------  |   (new size/rate: also after Windows Settings)
  | ---- BYE ---------------------->  |
```

## Versioning

`protocol_version` is bumped for incompatible changes. New message types or appended
payload fields that old peers can ignore do not require a bump.
