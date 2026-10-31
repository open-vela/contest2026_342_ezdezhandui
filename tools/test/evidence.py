#!/usr/bin/env python3
"""evidence.py - 一次真机取证的统一入口（复位 + 串口 + 可选分析仪）

为什么需要它：真机结论必须留证，而取证要同时协调三件东西——
  · P4 复位：按约定走 **USB-JTAG**（OpenOCD），不是 esptool 的软复位；
  · 串口日志：console 在 /dev/ttyUSB0，**但通常被 Tabby 占用**（串口是独占的），
    所以默认经 Tabby 的 MCP 读；Tabby 没开时才退回直接读串口；
  · 分析仪（可选）：pxview 抓一段波形，便于"命令/数据阶段到底有没有发生"这类判决。

用法:
  evidence.py --reset --seconds 180 --tag hosted        # 复位 + 抓 180 s 串口
  evidence.py --seconds 60 --watch '<regex>'            # 不复位，只看
  evidence.py --reset --analyzer 60 --tag dat1          # 顺便抓 60 s 波形
  evidence.py --reset --seconds 60 --analyzer 60 --decode   # 抓完立刻用解码器解析

--decode 会**在同一次运行里**抓完就解码，原因见下：分析仪是连续采样的，
sampleLimit 之外的数据会被滚掉——抓完隔一会儿再解码，往往只剩空窗口。

产物: logs/<UTC 时间戳>_<tag>/{console.txt,meta.txt[,analyzer.json]}
"""

import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
OCD = os.path.expanduser(
    "~/.espressif/tools/openocd-esp32/v0.12.0-esp32-20260831/openocd-esp32")
CONSOLE = "/dev/ttyUSB0"
BAUD = 115200


def sh(cmd, timeout=180):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


def jtag_reset():
    """复位 P4：按目标约定用 USB-JTAG。"""
    cfg = os.path.join(OCD, "share/openocd/scripts")
    p = sh([os.path.join(OCD, "bin/openocd"), "-s", cfg,
            "-f", "board/esp32p4-builtin.cfg",
            "-c", "init", "-c", "reset halt", "-c", "reset run", "-c", "shutdown"])
    return p.returncode, (p.stdout + p.stderr).strip().splitlines()[-1:]


def tabby_read(seconds):
    """经 Tabby MCP 轮询 console 缓冲；返回 (文本, 会话id)。"""
    sid_file = "/tmp/.tabby_mcp_sid"
    if os.path.exists(sid_file):
        os.remove(sid_file)
    p = sh([sys.executable, os.path.join(REPO, "tools/engineering/tabby_mcp.py"), "init"])
    if "protocol" not in (p.stdout + p.stderr):
        return None, None
    p = sh([sys.executable, os.path.join(REPO, "tools/engineering/tabby_mcp.py"), "sessions"])
    try:
        arr = json.loads(json.loads(p.stdout)["content"][0]["text"])
    except Exception:
        return None, None
    sid = None
    for s in arr:
        if s["profile"]["id"].endswith("ttyUSB0"):
            sid = s["sessionId"]
    if sid is None:
        return None, None
    chunks, t0, last = [], time.time(), 0
    while time.time() - t0 < seconds:
        p = sh([sys.executable, os.path.join(REPO, "tools/engineering/tabby_mcp.py"),
                "text", "get_terminal_buffer",
                json.dumps({"sessionId": sid, "lastNLines": 30000})])
        try:
            t = json.loads(p.stdout).get("content", p.stdout)
        except Exception:
            t = p.stdout
        n = len(t)
        if n != last:
            chunks.append(t)
            last = n
        time.sleep(5)
    return (chunks[-1] if chunks else ""), sid


def direct_read(seconds):
    """Tabby 不在时的退路：直接读串口（要求没有别的进程占用）。"""
    try:
        import serial
    except ImportError:
        return None
    try:
        ser = serial.Serial(CONSOLE, BAUD, timeout=1)
        ser.dtr = True
        ser.rts = True
    except Exception as e:
        sys.stderr.write("direct serial open failed: %s\n" % e)
        return None
    buf, t0 = [], time.time()
    try:
        while time.time() - t0 < seconds:
            line = ser.readline()
            if line:
                buf.append(line.decode("utf-8", "replace").rstrip())
    except Exception as e:
        sys.stderr.write("direct serial read stopped: %s\n" % e)
    return "\n".join(buf)


def analyzer_capture(seconds, outdir):
    try:
        sys.path.insert(0, os.path.join(REPO, "tools/test/pxview"))
        import px
    except Exception as e:
        return "pxview unavailable: %s" % e
    px.init()
    px.call("set_sample_config", sampleRate=1000000,
            sampleLimit=int(seconds * 1000000) + 1000000)
    px.call("start_capture")
    return "capture started (%d s at 1 MHz)" % seconds


def decode_capture(outdir, seconds):
    """Decode the capture that is still in the analyser's buffer.

    Runs immediately after the capture on purpose: the instrument samples
    continuously and only the last sampleLimit samples are readable, so a
    decode performed later finds empty windows (measured).
    """

    try:
        sys.path.insert(0, os.path.join(REPO, "tools/test/pxview"))
        import frames as F
    except Exception as e:
        return "decoder unavailable: %s" % e

    W = 5_000_000
    lines, total = [], 0
    for k in range(0, max(1, seconds // 5)):
        try:
            fr, _ = F.decode(k * W, W)
        except Exception:
            break
        if not fr:
            continue
        lines.append("window %d-%d s: %d frame(s)" % (k * 5, k * 5 + 5, len(fr)))
        for at, kind, idx, arg in fr:
            lines.append("  %9d  %s" % (at, F.describe(kind, idx, arg)))
            if idx == 53:
                d = F.decode_data_phase(at + 2000)
                lines.append("        data: clocks=%d start=%s bytes=%s"
                             % (d["clocks"], d["start"],
                                " ".join("%02x" % b for b in d["bytes"][:6]) or "-"))
        total += len(fr)
    lines.append("total frames: %d" % total)
    open(os.path.join(outdir, "decoded.txt"), "w").write("\n".join(lines) + "\n")
    return "%d frames decoded" % total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reset", action="store_true", help="先按 USB-JTAG 约定复位 P4")
    ap.add_argument("--seconds", type=int, default=120)
    ap.add_argument("--tag", default="evidence")
    ap.add_argument("--watch", default=None, help="只报告匹配该正则的行")
    ap.add_argument("--analyzer", type=int, default=0, help="同时抓 N 秒波形")
    ap.add_argument("--decode", action="store_true",
                    help="抓完立刻用 SDIO 解码器解析并保存")
    args = ap.parse_args()

    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    outdir = os.path.join(REPO, "logs", "%s_%s" % (stamp, args.tag))
    os.makedirs(outdir, exist_ok=True)

    meta = ["stamp: %s" % stamp, "reset: %s" % args.reset,
            "seconds: %d" % args.seconds]

    if args.analyzer:
        meta.append("analyzer: " + analyzer_capture(args.analyzer, outdir))

    if args.reset:
        rc, tail = jtag_reset()
        meta.append("openocd rc=%d %s" % (rc, tail))

    text, sid = tabby_read(args.seconds)
    if text is None:
        meta.append("console: direct serial (Tabby MCP unavailable)")
        text = direct_read(args.seconds) or "(no output)"
    else:
        meta.append("console: Tabby session %s" % sid)

    if args.decode:
        dec = decode_capture(outdir, args.seconds)
        meta.append("decode: " + dec)

    open(os.path.join(outdir, "console.txt"), "w").write(text)

    # Save the current boot on its own.
    #
    # The console keeps a scrollback that spans several boots (it survives
    # resets), so a tail of it interleaves them -- measured: lines from three
    # different boots ended up in one capture and sent the analysis chasing a
    # contradiction that did not exist.  Everything from the last boot banner
    # onwards belongs to the run that was just triggered.
    boots = [i for i, l in enumerate(text.split("\n"))
             if ("Booting MCUboot" in l) or ("ESP-ROM:esp32p4" in l)]
    if boots:
        cur = "\n".join(text.split("\n")[boots[-1]:])
        open(os.path.join(outdir, "boot.txt"), "w").write(cur)
        meta.append("boot section starts at line %d of the buffer" % boots[-1])
    open(os.path.join(outdir, "meta.txt"), "w").write("\n".join(meta) + "\n")
    print("saved: %s" % outdir)
    print("\n".join(meta))

    lines = [l.strip() for l in text.replace("\\r\\n", "\n").split("\n") if l.strip()]
    if args.watch:
        hit = [l for l in lines if re.search(args.watch, l)]
        print("--- %d line(s) matching %r (last 20):" % (len(hit), args.watch))
        for l in hit[-20:]:
            print("   ", l[:160])
    else:
        print("--- last 8 lines:")
        for l in lines[-8:]:
            print("   ", l[:160])
    return 0


if __name__ == "__main__":
    sys.exit(main())
