#!/usr/bin/env python3
"""Wait for SDIO activity and keep the capture that contains it.

Repeats short captures until channel 0 (CLK) shows more than the single
power-up transition. Run it, then press the board's reset button: the boot
burst happens in the first couple of seconds after reset and this keeps a
window open across it.

Do NOT use the server's find_next_edge for this: it returns `fromSample`
unchanged whenever the starting sample is past the real edge, so it reports
"activity" at every position you ask about. Samples are read and compared
here instead.

  python3 tools/test/pxview/watch.py [minutes] [sample_rate_hz] [window_samples]
"""
import base64, json, os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from px import init, call                                     # noqa: E402

MINUTES = float(sys.argv[1]) if len(sys.argv) > 1 else 8.0
RATE    = int(sys.argv[2])   if len(sys.argv) > 2 else 4_000_000
WINDOW  = int(sys.argv[3])   if len(sys.argv) > 3 else 2_000_000
OUTDIR  = "/tmp/pxwatch"

os.makedirs(OUTDIR, exist_ok=True)
init()
call("set_sample_config", sampleRate=RATE, sampleLimit=WINDOW, collectMode=0)


def bits(ch, start, n):
    r = call("get_samples", channelIndex=ch, channelType="logic",
             startSample=start, endSample=start + n - 1)
    raw = base64.b64decode(r["data"])
    return [(raw[i >> 3] >> (i & 7)) & 1 for i in range(r["sample_count"])]


def transitions(ch, total, chunk=500_000):
    count, prev, pos = 0, None, 0
    while pos < total:
        n = min(chunk, total - pos)
        for v in bits(ch, pos, n):
            if prev is not None and v != prev:
                count += 1
            prev = v
        pos += n
    return count


deadline = time.time() + MINUTES * 60
n = 0
while time.time() < deadline:
    n += 1
    try:
        call("start_capture")
        call("wait_capture", timeoutMs=30000)

        total = call("get_samples", channelIndex=0, channelType="logic",
                     startSample=0, endSample=99999999)["sample_count"]

        counts = {ch: transitions(ch, total) for ch in (0, 1, 2)}
    except Exception as exc:                                  # noqa: BLE001
        # The tunnel drops now and then; a single bad round should not end the
        # wait, because the reset this is watching for may come at any moment.
        print("capture %d failed: %s" % (n, exc), flush=True)
        time.sleep(1.0)
        try:
            init()
        except Exception:                                     # noqa: BLE001
            pass
        continue

    # One transition is the capture-start artifact; the boot burst produces many.
    if max(counts.values()) > 1:
        print(json.dumps({"capture": n, "transitions": counts}), flush=True)
        try:
            call("save_capture", filepath=os.path.join(OUTDIR, "activity_%03d.pxcap" % n))
        except Exception as exc:                              # noqa: BLE001
            print("save_capture:", exc, flush=True)
        print("SAVED ->", OUTDIR, flush=True)
        sys.exit(0)

    print("capture %d: idle %s" % (n, counts), flush=True)

print("timeout after %d captures, no activity" % n, flush=True)
sys.exit(1)
