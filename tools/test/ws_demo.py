#!/usr/bin/env python3
"""Minimal WebSocket client for the 桌伴 ai_agent (no external deps).

Usage:
    python3 ws_demo.py --host 192.168.1.105 --port 28789 --send "你好" --wait 60
"""
import argparse
import base64
import json
import os
import socket
import struct
import sys
import time


def ws_connect(host, port, path="/", timeout=10):
    s = socket.create_connection((host, port), timeout)
    key = base64.b64encode(os.urandom(16)).decode()
    req = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n"
    )
    s.sendall(req.encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = s.recv(4096)
        if not chunk:
            break
        buf += chunk
    status = buf.split(b"\r\n", 1)[0]
    if b"101" not in status:
        raise RuntimeError(f"handshake failed: {status!r} {buf[:200]!r}")
    return s


def ws_send_text(s, text):
    payload = text.encode()
    mask = os.urandom(4)
    n = len(payload)
    hdr = bytes([0x81])
    if n < 126:
        hdr += bytes([0x80 | n])
    elif n < 65536:
        hdr += bytes([0x80 | 126]) + struct.pack(">H", n)
    else:
        hdr += bytes([0x80 | 127]) + struct.pack(">Q", n)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    s.sendall(hdr + mask + masked)


def _recv_exact(s, n):
    data = b""
    while len(data) < n:
        chunk = s.recv(n - len(data))
        if not chunk:
            return data
        data += chunk
    return data


def ws_recv_text(s, timeout=30.0):
    """Return (opcode, payload_text_or_None). Returns (None, None) on close/timeout."""
    s.settimeout(timeout)
    try:
        h = _recv_exact(s, 2)
    except socket.timeout:
        return None, None
    if len(h) < 2:
        return None, None
    opcode = h[0] & 0x0F
    blen = h[1] & 0x7F
    masked = bool(h[1] & 0x80)
    if blen == 126:
        blen = struct.unpack(">H", _recv_exact(s, 2))[0]
    elif blen == 127:
        blen = struct.unpack(">Q", _recv_exact(s, 8))[0]
    mask = _recv_exact(s, 4) if masked else b""
    data = _recv_exact(s, blen) if blen else b""
    if masked:
        data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
    return opcode, data.decode("utf-8", errors="replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.1.105")
    ap.add_argument("--port", type=int, default=28789)
    ap.add_argument("--path", default="/")
    ap.add_argument("--send", action="append", default=[])
    ap.add_argument("--wait", type=float, default=45.0)
    ap.add_argument("--connect-timeout", type=float, default=8.0)
    args = ap.parse_args()

    t0 = time.time()
    try:
        s = ws_connect(args.host, args.port, args.path, args.connect_timeout)
    except Exception as e:
        print(f"[ws] connect FAILED after {time.time()-t0:.1f}s: {e}")
        return 2
    print(f"[ws] connected to ws://{args.host}:{args.port}{args.path}")

    for text in args.send:
        msg = json.dumps({"type": "message", "content": text}, ensure_ascii=False)
        ws_send_text(s, msg)
        print(f"[ws] -> {msg}")

    deadline = time.time() + args.wait
    frames = 0
    while time.time() < deadline:
        try:
            op, payload = ws_recv_text(s, timeout=min(10.0, deadline - time.time()))
        except Exception as e:
            print(f"[ws] recv error: {e}")
            break
        if op is None:
            continue
        frames += 1
        print(f"[ws] <- op={op} {payload}")
        if op == 0x8:
            break
    print(f"[ws] done: {frames} frames in {time.time()-t0:.1f}s")
    try:
        s.close()
    except Exception:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
