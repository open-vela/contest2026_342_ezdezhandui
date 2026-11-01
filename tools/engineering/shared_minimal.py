#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""重新计算"公共层最小交付集"（需先构建一次主板）。

依据三件事：
  1. 应用构建图（<build>/build.ninja）里真正编译的 common/espressif/*.c|S；
  2. 这些编译单元 + 全部 P4 编译单元的 `-MM` 头闭包里的直属公共层头文件；
  3. 公共层的构建文件（Kconfig/Make.defs/CMakeLists.txt/Bootloader.*/Wireless.*/esp_ulp.*/ulp*）。

输出可直接替换 gen_manifest_copyfiles.py 里的 SHARED_MINIMAL 集合。
"""
from __future__ import annotations

import json
import os
import re
import shlex
import subprocess
import sys

WS = os.environ.get("OPENVELA_WS", "/home/ez/share/openvela")
BUILD = os.path.join(WS, "cmake_out/esp32p4-function-ev-board_nsh")
SH = os.path.join(WS, "nuttx/arch/risc-v/src/common/espressif")


def main():
    ninja = open(os.path.join(BUILD, "build.ninja"), errors="replace").read()
    compiled = set(re.findall(r"common/espressif/([A-Za-z0-9_.]+\.(?:c|S|cc))", ninja))

    cc = json.load(open(os.path.join(BUILD, "compile_commands.json")))
    sample = next(e for e in cc if e["file"].endswith("esp_hosted/transport/esp_hosted_sdio.c"))
    args, skip = [], False
    for a in shlex.split(sample["command"]):
        if skip:
            skip = False
            continue
        if a == "-o":
            skip = True
            continue
        if a.endswith("esp_hosted_sdio.c"):
            continue
        args.append(a)

    hdrs = set()
    for f in {e["file"] for e in cc}:
        r = subprocess.run(args + ["-MM", "-MG", f], capture_output=True, text=True)
        for line in r.stdout.splitlines():
            if line.startswith((" ", "\t")):
                for tok in line.split():
                    m = re.match(r".*/common/espressif/([A-Za-z0-9_.]+\.h)$", tok)
                    if m:
                        hdrs.add(m.group(1))

    build_files = {f for f in os.listdir(SH)
                   if f.endswith((".cmake", ".mk", ".makefile"))
                   or f in ("Kconfig", "Make.defs", "CMakeLists.txt", "ulp_makefile")}

    required = sorted(compiled | hdrs | build_files
                      | {"chip.h", "esp_config.h", "esp_memorymap.h"})
    print("SHARED_MINIMAL = {")
    for r in required:
        print('    "%s",' % r)
    print("}")
    print(f"# 编译单元 {len(compiled)} | 头闭包 {len(hdrs)} | 构建文件 {len(build_files)} "
          f"| 合计 {len(required)}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
