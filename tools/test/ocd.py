#!/usr/bin/env python3
"""ocd.py - 用 OpenOCD 的 telnet 口做一次"停住看现场"

GDB 连这台 P4 会掉线（`Target disconnected`），但 OpenOCD 自己的 telnet 口
(4444) 一直可用，所以这里直接走 telnet：

  ocd.py pc           两核 PC + halt 原因，并尝试用 ELF 解析符号
  ocd.py halt|resume  停/放
  ocd.py regs         读 cpu0 的寄存器

前提：OpenOCD 已在跑（`openocd -f board/esp32p4-builtin.cfg -c init`）。
"""

import os
import socket
import subprocess
import sys
import time

OCD_TELNET = ("127.0.0.1", 4444)
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ELF = os.path.join(REPO, "..", "cmake_out", "esp32p4-function-ev-board_nsh", "nuttx")
ADDR2LINE = os.path.join(REPO, "..", "prebuilts/gcc/linux-x86_64/riscv-none-elf/bin",
                         "riscv-none-elf-addr2line")


def talk(cmds, wait=1.0):
    s = socket.create_connection(OCD_TELNET, timeout=10)
    time.sleep(0.8)
    out = []
    try:
        s.recv(65535)
    except Exception:
        pass
    for c in cmds:
        s.sendall((c + "\n").encode())
        time.sleep(wait)
        try:
            out.append(s.recv(65535).decode("utf-8", "replace"))
        except Exception as e:
            out.append("ERR %s" % e)
    s.close()
    return out


def resolve(addr):
    try:
        r = subprocess.run([ADDR2LINE, "-f", "-e", ELF, addr],
                           capture_output=True, text=True, timeout=20)
        return r.stdout.strip().replace("\n", " ")
    except Exception as e:
        return "addr2line failed: %s" % e


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else "pc"

    if what == "halt":
        print("".join(talk(["halt"])))
        return 0
    if what == "resume":
        print("".join(talk(["resume"])))
        return 0
    if what == "regs":
        print("".join(talk(["reg"])))
        return 0

    out = talk(["halt", "targets", "reg pc"], wait=1.2)
    print(out[0].strip()[:600])
    print(out[1].strip()[:600])

    pcs = []
    for line in out[2].splitlines():
        line = line.strip()
        if line.startswith("pc"):
            pcs.append(line.split()[-1])

    for label, pc in zip(("cpu0", "cpu1"), pcs):
        print("%s pc=%s -> %s" % (label, pc, resolve(pc)))

    talk(["resume"], 0.5)
    return 0


if __name__ == "__main__":
    sys.exit(main())
