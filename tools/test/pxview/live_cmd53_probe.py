#!/usr/bin/env python3
"""live_cmd53_probe.py - capture the wire, then decode the DATA PHASE of every
CMD53 the firmware issues.

Why this exists (appendix BY.9): the official esp-hosted stack reaches
"Open data path at slave" and then its four-byte register reads stall.  The
frame list alone says the command went out; it does not say whether the companion
drove DAT[3:0] afterwards.  That single fact decides which side to fix:

  * data phase present  -> the card answered, the host/driver dropped it;
  * data phase absent   -> the companion never sent it.

Usage:
  python3 tools/test/pxview/live_cmd53_probe.py [--seconds 60] [--tag name]
"""

import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import frames as F  # noqa: E402
import px           # noqa: E402

OCD = os.path.expanduser(
    "~/.espressif/tools/openocd-esp32/v0.12.0-esp32-20260831/openocd-esp32")


def jtag_reset():
    cfg = os.path.join(OCD, "share/openocd/scripts")
    p = subprocess.run([os.path.join(OCD, "bin/openocd"), "-s", cfg,
                        "-f", "board/esp32p4-builtin.cfg",
                        "-c", "init", "-c", "reset halt", "-c", "reset run",
                        "-c", "shutdown"],
                       capture_output=True, text=True, timeout=180)
    return p.returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--tag", default="cmd53-probe")
    args = ap.parse_args()

    px.init()
    px.call("set_sample_config", sampleRate=1_000_000,
            sampleLimit=args.seconds * 1_000_000 + 2_000_000)
    px.call("start_capture")
    print("capture started (%d s at 1 MHz)" % args.seconds)

    rc = jtag_reset()
    print("jtag reset rc=%d" % rc)

    time.sleep(args.seconds)

    out = os.path.join(REPO, "logs",
                       time.strftime("%Y%m%d-%H%M%S") + "_" + args.tag)
    os.makedirs(out, exist_ok=True)

    W = 5_000_000
    lines = []
    stats = {"cmd53": 0, "with_data": 0, "without_data": 0}

    for k in range(0, max(1, args.seconds // 5)):
        try:
            fr, _ = F.decode(k * W, W)
        except Exception as exc:                       # noqa: BLE001
            lines.append("window %d-%d s: decode failed: %s"
                         % (k * 5, k * 5 + 5, exc))
            continue

        if not fr:
            continue

        for at, kind, idx, arg in fr:
            if idx != 53:
                continue

            stats["cmd53"] += 1

            # The data phase follows the command frame; probe a few offsets
            # because 'at' is the sample where the command frame ended.
            best = None
            for off in (0, 20, 50, 100, 200, 400, 800):
                try:
                    dp = F.decode_data_phase(at + off, max_clocks=120)
                except Exception:                      # noqa: BLE001
                    dp = None
                if dp and dp.get("start"):
                    best = (off, dp)
                    break

            if best is None:
                stats["without_data"] += 1

                # Detail matters here: "no start bit" can mean the card held the
                # bus busy (DAT0 low) or that no data clocks were produced at
                # all.  Report both, plus the first nibbles.
                detail = ""
                try:
                    dp0 = F.decode_data_phase(at, max_clocks=120)
                    clk = F.sample_bits(F.CH_CLK, at, 600)
                    d0 = F.sample_bits(F.CH_DATA[0], at, 600)
                    edges = sum(1 for i in range(1, len(clk))
                                if clk[i] and not clk[i - 1])
                    d0_low = (sum(1 for v in d0 if v == 0) * 100 // max(1, len(d0)))
                    detail = (" clocks=%d dat0_low=%d%% nibbles=%s"
                              % (edges, d0_low,
                                 (dp0["nibbles"][:16] if dp0 else "")))

                except Exception as exc:               # noqa: BLE001
                    detail = " (detail unavailable: %s)" % exc

                lines.append("%9d  CMD53 %-44s | data phase: NO start bit%s"
                             % (at, F.describe(kind, idx, arg), detail))
            else:
                off, dp = best
                stats["with_data"] += 1
                first = " ".join("%02x" % b for b in dp["bytes"][:8])
                lines.append("%9d  CMD53 %-44s | data phase: start@+%d "
                             "clocks=%d bytes=[%s]"
                             % (at, F.describe(kind, idx, arg), off,
                                dp["clocks"], first))

    lines.append("")
    lines.append("summary: CMD53 frames=%d with data=%d without data=%d"
                 % (stats["cmd53"], stats["with_data"], stats["without_data"]))

    text = "\n".join(lines)
    with open(os.path.join(out, "cmd53_data_phase.txt"), "w") as fh:
        fh.write(text + "\n")

    print(text)
    print("\nsaved:", out)


if __name__ == "__main__":
    main()
