#!/usr/bin/env python3
"""Minimal DeuxDisplay protocol client for testing the host without a tablet.

Connects to DeuxDisplayHost, sends HELLO, and writes the received H.264 stream to a file
(playable with `ffplay out.h264` or VLC). On Windows, time.perf_counter and the host both use
QueryPerformanceCounter, so capture->receive latency can be measured when run on the host PC.

Usage: python tools/dump_receiver.py [--port 27183] [--seconds 10] [--out out.h264]
"""

import argparse
import socket
import statistics
import struct
import sys
import time

HEADER = struct.Struct("<BBHIQ")  # type, flags, reserved, length, timestamp (docs/wire-protocol.md)
MAGIC = 0x50445844
HELLO, CONFIG, BYE, VIDEO_FRAME, REQUEST_KEYFRAME, PING, PONG = 0x01, 0x02, 0x03, 0x10, 0x11, 0x20, 0x21
FLAG_KEYFRAME, FLAG_CODEC_CONFIG = 0x01, 0x02
CODEC_H264 = 0x01


def now_us() -> int:
    return time.perf_counter_ns() // 1000


def send(sock: socket.socket, msg_type: int, payload: bytes = b"", flags: int = 0) -> None:
    sock.sendall(HEADER.pack(msg_type, flags, 0, len(payload), now_us()) + payload)


def recv_exact(sock: socket.socket, size: int) -> bytes:
    buf = bytearray()
    while len(buf) < size:
        chunk = sock.recv(size - len(buf))
        if not chunk:
            raise ConnectionError("host closed the connection")
        buf += chunk
    return bytes(buf)


def recv_message(sock: socket.socket):
    msg_type, flags, reserved, length, ts = HEADER.unpack(recv_exact(sock, HEADER.size))
    if reserved != 0:
        raise ValueError("reserved header bits set")
    return msg_type, flags, ts, recv_exact(sock, length) if length else b""


def hello(width: int, height: int) -> bytes:
    name = b"dump_receiver.py"
    return struct.pack("<IHHHHIIH", MAGIC, 1, width, height, 160, 60000, CODEC_H264, len(name)) + name


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=27183)
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--out", default="out.h264")
    args = ap.parse_args()

    sock = socket.create_connection((args.host, args.port))
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    send(sock, HELLO, hello(2408, 1720))

    msg_type, _, _, payload = recv_message(sock)
    if msg_type != CONFIG:
        print(f"expected CONFIG, got 0x{msg_type:02x}", file=sys.stderr)
        return 1
    version, codec, _, width, height, fps_mhz, kbps = struct.unpack("<HBBHHII", payload[:16])
    print(f"CONFIG v{version} codec={codec} {width}x{height} @ {fps_mhz / 1000:.2f} Hz, {kbps} kbit/s")

    frames = keyframes = total_bytes = 0
    latencies_ms = []
    deadline = time.monotonic() + args.seconds
    with open(args.out, "wb") as out:
        while time.monotonic() < deadline:
            msg_type, flags, ts, payload = recv_message(sock)
            received = now_us()
            if msg_type == VIDEO_FRAME:
                out.write(payload)
                if flags & FLAG_CODEC_CONFIG:
                    print(f"CODEC_CONFIG ({len(payload)} bytes)")
                    continue
                frames += 1
                keyframes += bool(flags & FLAG_KEYFRAME)
                total_bytes += len(payload)
                latencies_ms.append((received - ts) / 1000)
            elif msg_type == CONFIG:
                print("CONFIG changed mid-stream")
            elif msg_type == BYE:
                print("host said BYE")
                break
    send(sock, BYE)
    sock.close()

    print(f"{frames} frames ({keyframes} key), {total_bytes / 1e6:.2f} MB -> {args.out}")
    if latencies_ms:
        latencies_ms.sort()
        p95 = latencies_ms[int(len(latencies_ms) * 0.95) - 1] if len(latencies_ms) >= 20 else latencies_ms[-1]
        print(f"capture->receive latency ms: p50 {statistics.median(latencies_ms):.2f}, "
              f"p95 {p95:.2f}, max {latencies_ms[-1]:.2f}")
    return 0 if frames else 1


if __name__ == "__main__":
    sys.exit(main())
