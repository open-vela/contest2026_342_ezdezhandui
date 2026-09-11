#!/usr/bin/env python3
"""board.py - 桌伴 DeskMate 真机闭环测试 harness

实测端口拓扑（2026-09-11 确认）:
  /dev/ttyACM0  Espressif USB-Serial/JTAG   -> 烧录用（esptool）+ ROM/MCUboot 日志
  /dev/ttyUSB0  CP2102 UART 桥              -> **NuttX console**（UART0）
                                              ⚠️ 必须 assert DTR/RTS，否则读到 0 字节

用法:
  board.py reset                     复位并捕获启动日志
  board.py run "<cmd>" [<cmd>...]    依次发命令并捕获
  board.py script <file>             按行执行脚本（:sleep N 为延时）
  board.py watch --timeout N         仅监听 N 秒
  board.py login                     等待并确认 nsh> 提示符

选项:
  --expect "<regex>"   断言，未匹配退出码 2
  --save <file>        保存捕获内容
  --quiet              不回显
"""

import argparse
import re
import subprocess
import sys
import time

try:
    import serial
except ImportError:
    sys.stderr.write("ERROR: pyserial 未安装\n")
    sys.exit(3)

CONSOLE = "/dev/ttyUSB0"
FLASH = "/dev/ttyACM0"
BAUD = 115200
PROMPT_RE = re.compile(rb"(nsh>|vela>|READY)")
PROMPT_TXT = re.compile(r"(nsh>|vela>|READY)")


def open_console(port=CONSOLE, retries=12):
    for i in range(retries):
        try:
            s = serial.Serial(port, BAUD, timeout=0.2)
            s.dtr = True      # CP2102: 不 assert 则收不到数据（实测）
            s.rts = True
            time.sleep(0.4)
            s.reset_input_buffer()
            return s
        except Exception as e:
            if i == retries - 1:
                sys.stderr.write(f"ERROR: 打开 {port} 失败: {e}\n")
                return None
            time.sleep(1.0)
    return None


def capture(ser, seconds, idle_stop=True, idle_gap=1.5):
    buf = b""
    t0 = time.time()
    last = time.time()
    while time.time() - t0 < seconds:
        d = ser.read(4096)
        if d:
            buf += d
            last = time.time()
        else:
            if idle_stop and buf and (time.time() - last) > idle_gap:
                break
            time.sleep(0.03)
    return buf


def sync(ser, tries=5):
    """按回车直到 nsh> 出现"""
    out = b""
    for _ in range(tries):
        ser.reset_input_buffer()
        ser.write(b"\r\n")
        ser.flush()
        chunk = capture(ser, 2.5)
        out += chunk
        if PROMPT_RE.search(chunk):
            return out
    return out


def do_reset(ser):
    """用 esptool 经 USJ 口硬复位（UART0 侧无复位线）"""
    try:
        ser.close()
    except Exception:
        pass
    subprocess.run(
        f"timeout 60 ~/.local/bin/esptool.py --chip esp32p4 --port {FLASH} "
        "--before default_reset --after hard_reset --no-stub chip_id "
        ">/tmp/board_reset.log 2>&1",
        shell=True,
    )
    time.sleep(1.0)
    return open_console()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("action",
                    choices=["reset", "run", "script", "watch", "login"])
    ap.add_argument("cmds", nargs="*", default=[])
    ap.add_argument("--port", default=CONSOLE)
    ap.add_argument("--timeout", type=float, default=15.0)
    ap.add_argument("--expect", default=None)
    ap.add_argument("--save", default=None)
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--no-idle", action="store_true",
                    help="不因空闲提前结束，抓满 --timeout（长启动流程用）")
    args = ap.parse_args()

    ser = open_console(args.port)
    if ser is None:
        return 3

    out = b""
    try:
        if args.action == "reset":
            ser = do_reset(ser)
            if ser is None:
                return 3
            out = capture(ser, args.timeout, idle_stop=False)
        elif args.action == "login":
            out = sync(ser)
        elif args.action == "watch":
            out = capture(ser, args.timeout, idle_stop=False)
        elif args.action == "script":
            with open(args.cmds[0], encoding="utf-8") as f:
                lines = [l.strip() for l in f
                         if l.strip() and not l.strip().startswith("#")]
            out += sync(ser)
            for ln in lines:
                if ln.startswith(":sleep"):
                    time.sleep(float(ln.split()[1]))
                    continue
                ser.reset_input_buffer()
                ser.write(ln.encode() + b"\r\n")
                ser.flush()
                time.sleep(0.6)
                chunk = capture(ser, args.timeout, idle_stop=not args.no_idle)
                out += f"\n=== $ {ln}\n".encode() + chunk
        elif args.action == "run":
            out += sync(ser)
            for cmd in args.cmds:
                ser.reset_input_buffer()
                ser.write(cmd.encode() + b"\r\n")
                ser.flush()
                time.sleep(0.6)
                chunk = capture(ser, args.timeout, idle_stop=not args.no_idle)
                out += f"\n=== $ {cmd}\n".encode() + chunk
    finally:
        try:
            ser.close()
        except Exception:
            pass

    text = out.decode(errors="replace")
    if args.save:
        with open(args.save, "w", encoding="utf-8") as f:
            f.write(text)
    if not args.quiet:
        sys.stdout.write(text if text.endswith("\n") else text + "\n")

    if args.expect:
        if not re.search(args.expect, text, re.MULTILINE):
            sys.stderr.write(f"\n[board] ASSERT FAILED: /{args.expect}/\n")
            return 2
        sys.stderr.write(f"[board] ASSERT OK: /{args.expect}/\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
