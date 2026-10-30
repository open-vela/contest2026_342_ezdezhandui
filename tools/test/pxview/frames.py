#!/usr/bin/env python3
"""Decode SDIO command/response frames straight from the capture's raw samples.

Why this exists next to the built-in decoder
--------------------------------------------
The analyzer's own SDIO decoder annotates frames, but its "R/W flag" label is
not the one this project needs to reason with: ESP-IDF and NuttX both define
CMD53 bit 31 as *write* when set (``SD_ARG_CMD53_WRITE`` / ``cmd53.rw_flag``),
while the annotation reads the opposite way for the frames measured here.  Two
conventions, one bit -- so the bit itself is decoded here, from the samples,
and printed as a number.

Frame format (SDIO, both directions, 48 bits):

    bit 0    start (0)
    bit 1    direction: 1 = host -> card (command), 0 = card -> host (response)
    bits 2-7 command index, MSB first
    bits 8-39 argument, MSB first
    bits 40-46 CRC7
    bit 47   end (1)

CMD53 argument (NuttX ``struct sdio_cmd53``, same field order as the spec):

    bits 0-8   byte/block count
    bits 9-25  register address
    bit 26     op code (1 = incrementing address)
    bit 27     block mode
    bits 28-30 function number
    bit 31     rw flag (1 = write host->card, 0 = read card->host)

Usage:
  frames.py <startSample> <windowSamples> [sampleRate]

Reads the *current* capture through the pxview MCP server; it never starts a
capture and never changes the sample configuration, so it can be pointed at a
capture that is already on screen.
"""
import base64
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import px                                                       # noqa: E402

CH_CMD, CH_CLK = 0, 1


def crc7(bits_):
    """SD CRC7 (x^7 + x^3 + 1), MSB first -- verified against CMD0/CMD5."""
    crc = 0
    for b in bits_:
        crc = (crc << 1) & 0xFF
        if (((crc >> 7) & 1) ^ b):
            crc ^= 0x09
    return crc & 0x7F


def sample_bits(channel, start, count):
    r = px.call("get_samples", channelIndex=channel, channelType="logic",
                startSample=start, endSample=start + count - 1)
    raw = base64.b64decode(r["data"])
    return [(raw[i >> 3] >> (i & 7)) & 1 for i in range(r["sample_count"])]


def decode(start, count):
    clk = sample_bits(CH_CLK, start, count)
    cmd = sample_bits(CH_CMD, start, count)

    edges = [i for i in range(1, len(clk)) if clk[i] == 1 and clk[i - 1] == 0]
    if len(edges) < 49:
        return [], len(edges)

    sampled = [cmd[e] for e in edges]
    frames = []
    i = 0
    while i < len(sampled) - 48:
        win = sampled[i:i + 48]
        if win[0] == 0 and win[47] == 1:
            crc = 0
            for b in win[40:47]:
                crc = (crc << 1) | b
            if crc7(win[0:40]) == crc:
                idx = 0
                for b in win[2:8]:
                    idx = (idx << 1) | b
                arg = 0
                for b in win[8:40]:
                    arg = (arg << 1) | b
                frames.append((start + edges[i], "cmd" if win[1] else "rsp",
                               idx, arg))
                i += 48
                continue
        i += 1

    return frames, len(edges)


def describe(kind, idx, arg):
    if idx != 53:
        return "CMD%-2d %s arg=0x%08x" % (idx, kind, arg)
    rw = "WRITE" if (arg >> 31) & 1 else "READ "
    return ("CMD53 %s %s blk=%d fn=%d addr=0x%05x count=%d op=%d arg=0x%08x"
            % (kind, rw, (arg >> 27) & 1, (arg >> 28) & 7,
               (arg >> 9) & 0x1ffff, arg & 0x1ff, (arg >> 26) & 1, arg))


def main():
    start = int(sys.argv[1])
    count = int(sys.argv[2])
    rate = int(sys.argv[3]) if len(sys.argv) > 3 else 1_000_000
    px.init()
    frames, nedges = decode(start, count)
    print("window %d..%d (%d samples, %.3f s at %d Hz), CLK edges %d, frames %d"
          % (start, start + count - 1, count, count / rate, rate, nedges,
             len(frames)))
    for at, kind, idx, arg in frames:
        print("  %9d  %8.4f s  %s" % (at, at / rate, describe(kind, idx, arg)))


if __name__ == "__main__":
    main()
