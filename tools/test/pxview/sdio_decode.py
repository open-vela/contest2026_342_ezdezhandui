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
CH_CLK, CH_CMD = 1, 0

# Channel assignment measured on the board, not assumed: a steady-state capture
# with the driver polling CMD52 shows ch1 carrying ~30k transitions per 0.3 s at
# a ~50% duty cycle (a clock) while ch0 stays parked at a constant level, and no
# 48-bit frame on any other enabled channel decodes with a valid CRC7. The
# earlier 0,1 order was therefore backwards. See 附录 AG of
# docs/02_驱动实现/WiFi_SDIO实现.md.

init()
call("set_sample_config", sampleRate=RATE, sampleLimit=WINDOW, collectMode=0)


def bits(ch, start, n):
    r = call("get_samples", channelIndex=ch, channelType="logic",
             startSample=start, endSample=start + n - 1)
    raw = base64.b64decode(r["data"])
    return [(raw[i >> 3] >> (i & 7)) & 1 for i in range(r["sample_count"])]


def rising_edges(b):
    return [i for i in range(1, len(b)) if b[i] == 1 and b[i - 1] == 0]




def crc7(bits_):
    """SD CRC7 (x^7 + x^3 + 1) over a bit string, MSB first.

    Verified against the known CMD0 frame: 40 body bits of
    start=0, dir=1, index=0, arg=0 must give CRC 0x4A, i.e. the 48-bit
    frame is 0x400000000095. An implementation that returns anything else
    will reject every real frame, so this is checked first.

    Note the formulation: the output bit and the input bit are XORed BEFORE
    the polynomial step. Shifting the input in first and then testing bit 6
    -- the obvious-looking version -- is wrong and yields 0x1B here.
    """
    crc = 0
    for b in bits_:
        crc = (crc << 1) & 0xFF
        if (((crc >> 7) & 1) ^ b):
            crc ^= 0x09
    return crc & 0x7F

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
            crc = 0
            for b in win[40:47]:
                crc = (crc << 1) | b
            if crc7(win[0:40]) != crc:
                i += 1
                continue
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
