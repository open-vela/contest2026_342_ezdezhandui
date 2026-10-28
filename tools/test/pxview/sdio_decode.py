#!/usr/bin/env python3
"""Decode SDIO command/response frames from a PXView capture.

The CMD line carries 48-bit frames in both directions:

    bit 0    start (always 0, host drives the line low)
    bit 1    direction: 1 = host -> card (command), 0 = card -> host (response)
    bits 2-7 command index (6 bits, MSB first)
    bits 8-39 argument (32 bits, MSB first)
    bits 40-46 CRC7
    bit 47   end (always 1)

Bits are sampled on the rising edge of CLK. This script works from the raw
sample arrays, so it needs a capture that actually contains traffic -- a window
of all-idle lines decodes to nothing and says so.

  python3 tools/test/pxview/sdio_decode.py [sample_rate_hz] [window_samples]
"""
import base64, sys, os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from px import init, call                                     # noqa: E402

RATE   = int(sys.argv[1]) if len(sys.argv) > 1 else 4_000_000
WINDOW = int(sys.argv[2]) if len(sys.argv) > 2 else 2_000_000
CH_CLK, CH_CMD = 0, 1

init()
call("set_sample_config", sampleRate=RATE, sampleLimit=WINDOW, collectMode=0)


def bits(ch, start, n):
    r = call("get_samples", channelIndex=ch, channelType="logic",
             startSample=start, endSample=start + n - 1)
    raw = base64.b64decode(r["data"])
    return [(raw[i >> 3] >> (i & 7)) & 1 for i in range(r["sample_count"])]


def rising_edges(b):
    return [i for i in range(1, len(b)) if b[i] == 1 and b[i - 1] == 0]


def decode():
    clk = bits(CH_CLK, 0, WINDOW)
    cmd = bits(CH_CMD, 0, WINDOW)
    edges = rising_edges(clk)
    print("CLK rising edges: %d" % len(edges))
    if len(edges) < 60:
        print("not enough clock edges to carry a 48-bit frame -- window looks idle")
        return

    # Sample CMD on each rising clock edge, then slide a 48-bit window looking
    # for a start bit (0) followed by a plausible 1 end bit.
    sampled = [cmd[e] for e in edges]
    frames = []
    i = 0
    while i < len(sampled) - 48:
        win = sampled[i:i + 48]
        if win[0] == 0 and win[47] == 1:
            idx = 0
            for b in win[2:8]:
                idx = (idx << 1) | b
            arg = 0
            for b in win[8:40]:
                arg = (arg << 1) | b
            if 0 <= idx <= 63:
                frames.append((edges[i], "cmd" if win[1] else "rsp", idx, arg))
                i += 48
                continue
        i += 1

    print("decoded %d frame(s)" % len(frames))
    for at, kind, idx, arg in frames[:40]:
        print("  sample %8d  %-3s  CMD%-2d  arg=0x%08x" % (at, kind, idx, arg))


if __name__ == "__main__":
    decode()
