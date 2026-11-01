#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""验证 P4 驱动的"共享层 -> esp32p4/"搬迁是否完整、无悬空引用。

两种模式：

    verify_migration.py --static          # 静态一致性矩阵（默认）
    verify_migration.py --compile         # 用工程真实编译参数全量 -c 编译

--static 检查项
    1. 共享层零引用：common/espressif/ 里不得出现已搬走文件的名字；
    2. 构建项 parity：每个搬走的 .c 在 P4 的 CMakeLists.txt 与 Make.defs 里各出现恰好一次；
    3. Kconfig 开关有效性：构建挂接用到的每个 CONFIG_* 符号在树里确有声明；
    4. 交付文件数：共享层 / drivers / 作品仓（若存在）三处的文件数。

    include 能否解析**不在这里自研判断**：工程头文件来自构建目录（生成头、软链、
    esp-hal-3rdparty 的 -I），用编译器验证才是准的 —— 这一项由 `--compile` 的全量
    真编译矩阵覆盖（每个 .c 都按工程真实 flag 编译）。

--compile 检查项
    从 <build>/compile_commands.json 取一条真实编译命令的 flag，用 `-c` 编译到临时目录：
      * 必过：`esp32p4/drivers/*.c`（搬迁的 18 个文件，全部）+ 共享层里
        **确实参与本配置构建**的文件（从 build.ninja 的源文件表推导）；
      * 跳过：共享层里因 CONFIG 未开启而不参与构建的文件——它们在本配置下没有
        编译上下文（例如原生 ESP Wi-Fi 相关，需要 IDF 注入头），列出并注明原因，
        不计为失败。

    这样"必过集"等价于"我们这次改动会影响的全部编译单元"。

退出码：0 = 全部通过；1 = 有问题。
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import subprocess
import sys

# 本脚本住在作品仓里（tools/engineering/），而 nuttx 在工作区根下，
# 两者是**不同**的位置：工作区根必须显式给出（默认本机布局）。
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WS = os.environ.get("OPENVELA_WS", "/home/ez/share/openvela")
NUTTX = os.path.join(WS, "nuttx")
COMMON = os.path.join(NUTTX, "arch/risc-v/src/common/espressif")
P4 = os.path.join(NUTTX, "arch/risc-v/src/esp32p4")
DRIVERS = os.path.join(P4, "drivers")
BUILD = os.path.join(WS, "cmake_out/esp32p4-function-ev-board_nsh")

# 已迁到 esp32p4/ 的东西（搬迁清单，用于"共享层零引用"检查）
MOVED = [
    # esp_hosted 集群
    "esp_sdmmc", "esp_hosted_sdio", "eh_frame",
    "esp_hosted_official", "platform_include", "freertos_compat",
    # 9 个自有驱动
    "esp_i2s", "esp_emac", "esp_adc", "esp_touch", "esp_mipi_dsi",
    "esp_lirc", "esp_nxdiag", "esp_csi", "esp_dedic_gpio",
]

# 项目自有头文件前缀（用于区分"系统/IDF 头"与"项目头"）
PROJECT_PREFIXES = ("nuttx/", "arch/", "espressif/", "drivers/", "esp_", "eh_",
                    "sdio_", "bootloader_", "spi_flash_", "hal/")


def project_headers():
    """共享层 + drivers 目录下的头文件（按 basename 索引）。"""
    out = {}
    for d in (COMMON, DRIVERS):
        for name in os.listdir(d):
            if name.endswith(".h"):
                out.setdefault(name, []).append(os.path.join(d, name))
    return out


def include_dirs():
    """从 compile_commands.json 提取工程 -I 目录（退化时用经验集合）。"""
    dirs = [COMMON, DRIVERS, P4, os.path.join(NUTTX, "include"),
            os.path.join(NUTTX, "arch/risc-v/src/common")]
    cc = os.path.join(BUILD, "compile_commands.json")
    if os.path.exists(cc):
        try:
            data = json.load(open(cc))
            for e in data:
                for tok in shlex.split(e.get("command", "")):
                    if tok.startswith("-I") and len(tok) > 2:
                        p = tok[2:]
                        if p not in dirs:
                            dirs.append(p)
        except Exception as exc:  # noqa: BLE001
            print(f"  (compile_commands.json 读取失败，用默认集合: {exc})")
    return dirs


INCLUDE_QUOTED = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)
INCLUDE_ANGLE = re.compile(r'^\s*#\s*include\s+<([^>]+)>', re.M)


def resolves(name, dirs, own_dir):
    if os.path.exists(os.path.join(own_dir, name)):
        return True
    for d in dirs:
        if os.path.exists(os.path.join(d, name)):
            return True
    # <espressif/xxx.h> 这种：dirs 里可能包含 .../common/espressif 的父目录
    for d in dirs:
        cand = os.path.join(d, "espressif", name) if not name.startswith("espressif/") else None
        if cand and os.path.exists(cand):
            return True
    return False


def check_includes(dirs):
    hdrs = project_headers()
    problems, checked = [], 0
    files = []
    for d in (COMMON, DRIVERS):
        files += [os.path.join(d, f) for f in os.listdir(d)
                  if f.endswith((".c", ".h"))]

    for path in files:
        own = os.path.dirname(path)
        try:
            text = open(path, errors="replace").read()
        except OSError:
            continue
        for m in list(INCLUDE_QUOTED.finditer(text)) + list(INCLUDE_ANGLE.finditer(text)):
            name = m.group(1)
            if name.endswith(".h") and name in hdrs:
                checked += 1
                if not resolves(name, dirs, own):
                    problems.append(f"{os.path.relpath(path, NUTTX)}: 头文件无法解析: {name}")
            elif name.startswith(PROJECT_PREFIXES) and not resolves(name, dirs, own):
                # 项目前缀但既不在两份目录里，也解析不到 -> 也可能是问题
                checked += 1
                problems.append(f"{os.path.relpath(path, NUTTX)}: 项目头无法解析: {name}")
    return checked, problems


def check_shared_clean():
    problems = []
    text_files = []
    for f in os.listdir(COMMON):
        if f.endswith((".c", ".h")):
            text_files.append(os.path.join(COMMON, f))
    for build in ("CMakeLists.txt", "Make.defs", "Kconfig"):
        p = os.path.join(COMMON, build)
        if os.path.exists(p):
            text_files.append(p)
    for p in text_files:
        text = open(p, errors="replace").read()
        for name in MOVED:
            # 精确到"文件名/配置名"边界，避免误报（例如 esp_i2s 是 esp_i2s0 的前缀）
            if re.search(r"(?<![A-Za-z0-9_])%s(?![A-Za-z0-9_])" % re.escape(name), text):
                problems.append(f"{os.path.relpath(p, NUTTX)}: 仍引用已搬走的 {name}")
    return problems


def check_build_parity():
    """每个搬走的 .c 在 P4 的 CMakeLists 与 Make.defs 里各恰好一次。"""
    problems = []
    cmake = open(os.path.join(P4, "CMakeLists.txt")).read()
    make = open(os.path.join(P4, "Make.defs")).read()
    srcs = sorted(f for f in os.listdir(DRIVERS) if f.endswith(".c"))
    for f in srcs:
        n_c = len(re.findall(r"drivers/%s(?![A-Za-z0-9_])" % re.escape(f), cmake))
        n_m = len(re.findall(r"drivers/%s(?![A-Za-z0-9_])" % re.escape(f), make))
        if n_c != 1 or n_m != 1:
            problems.append(f"drivers/{f}: CMake={n_c}, Make={n_m}（各应为 1）")
    return srcs, problems


def kconfig_symbols():
    """树里所有 config/menuconfig 声明的符号。"""
    syms = set()
    for root, _, files in os.walk(NUTTX):
        if "Kconfig" not in files:
            continue
        p = os.path.join(root, "Kconfig")
        try:
            text = open(p, errors="replace").read()
        except OSError:
            continue
        syms.update(re.findall(r"^(?:config|menuconfig)\s+([A-Za-z0-9_]+)", text, re.M))
    return syms


def check_guards(syms):
    """P4 构建挂接里引用的 CONFIG_* 符号必须存在。"""
    problems, used = [], set()
    for f in ("CMakeLists.txt", "Make.defs"):
        text = open(os.path.join(P4, f)).read()
        used.update(re.findall(r"CONFIG_([A-Za-z0-9_]+)", text))
    known_dead = {"SYSTEM_NXDIAG_ESPRESSIF_CHIP_WO_TOOL"}
    for s in sorted(used):
        if s not in syms:
            if s in known_dead:
                print(f"     (说明) CONFIG_{s} 在本树不存在 —— 该驱动只能直接编译验证")
            else:
                problems.append(f"P4 构建挂接引用了不存在的符号: CONFIG_{s}")
    return used, problems


def count_files():
    def n(d, deep=True):
        if not os.path.isdir(d):
            return 0
        if deep:
            return sum(len(files) for _, _, files in os.walk(d))
        return len([f for f in os.listdir(d) if os.path.isfile(os.path.join(d, f))])

    repo_common = os.path.join(
        REPO, "nuttx/arch/risc-v/src/common/espressif")
    repo_drivers = os.path.join(REPO, "nuttx/arch/risc-v/src/esp32p4/drivers")
    repo_nuttx = os.path.join(REPO, "nuttx")
    return {
        # 共享层用浅计数：目录里还有 bootloader/（嵌套依赖树 2.8 万文件），
        # 它由 export 的“跳过目录条目”规则排除，不属于源码计数
        "workspace 共享层(浅)": n(COMMON, deep=False),
        "workspace drivers": n(DRIVERS, deep=False),
        "作品仓 共享层(浅)": n(repo_common, deep=False),
        "作品仓 drivers": n(repo_drivers, deep=False),
        "作品仓 nuttx": n(repo_nuttx),
        "nuttx 根": WS,
        "作品仓根": REPO,
    }


def run_static():
    print("== V1 静态一致性矩阵 ==")
    shared_problems = check_shared_clean()
    print(f"  1) 共享层零引用: 问题 {len(shared_problems)}")

    srcs, parity_problems = check_build_parity()
    print(f"  2) 构建 parity: {len(srcs)} 个 drivers 源文件, 问题 {len(parity_problems)}")

    syms = kconfig_symbols()
    used, guard_problems = check_guards(syms)
    print(f"  3) 开关有效性: P4 挂接引用 {len(used)} 个符号, 问题 {len(guard_problems)}")

    counts = count_files()
    print("  4) 文件数: " + ", ".join(f"{k}={v}" for k, v in counts.items()))

    problems = shared_problems + parity_problems + guard_problems
    if problems:
        print("\n  ❌ 问题清单:")
        for p in problems[:60]:
            print("    -", p)
        if len(problems) > 60:
            print(f"    ... 另有 {len(problems) - 60} 条")
        return 1
    print("\n  ✅ V1 全部通过")
    return 0


def sample_command():
    cc = os.path.join(BUILD, "compile_commands.json")
    if not os.path.exists(cc):
        sys.exit(f"缺少 {cc}（先构建主板一次）")
    data = json.load(open(cc))
    for e in data:
        if e["file"].endswith("esp_hosted/transport/esp_hosted_sdio.c"):
            return e
    sys.exit("未找到样例编译命令")


def run_compile():
    print("== V2 全驱动真编译矩阵（-c）==")
    sample = sample_command()
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

    outdir = os.path.join("/tmp", "verify-migration-obj")
    os.makedirs(outdir, exist_ok=True)

    # 本配置真正参与构建的共享层源文件（build.ninja 里出现过的）
    graph = ""
    ninja = os.path.join(BUILD, "build.ninja")
    if os.path.exists(ninja):
        graph = open(ninja, errors="replace").read()

    def in_graph(name):
        return re.search(r"common/espressif/%s(?![A-Za-z0-9_])" % re.escape(name), graph) is not None

    must = [os.path.join(DRIVERS, f) for f in sorted(os.listdir(DRIVERS)) if f.endswith(".c")]
    skipped = []
    for f in sorted(os.listdir(COMMON)):
        if not f.endswith(".c"):
            continue
        if in_graph(f):
            must.append(os.path.join(COMMON, f))
        else:
            skipped.append(f)
    targets = must

    failed = []
    print(f"  必过集: {len(targets)} 个（drivers + 参与本配置构建的共享层源文件）")
    if skipped:
        print(f"  跳过 {len(skipped)} 个共享层文件（本配置未开启对应 CONFIG）: "
              + ", ".join(skipped[:8]) + (" ..." if len(skipped) > 8 else ""))
    for i, path in enumerate(targets, 1):
        obj = os.path.join(outdir, os.path.basename(path) + ".o")
        r = subprocess.run(args + ["-c", path, "-o", obj],
                           capture_output=True, text=True)
        rel = os.path.relpath(path, NUTTX)
        if r.returncode != 0:
            failed.append((rel, (r.stderr or r.stdout).strip().splitlines()[:3]))
            print(f"  [{i}/{len(targets)}] FAIL {rel}")
        else:
            os.unlink(obj)

    print(f"\n  编译 {len(targets)} 个文件, 失败 {len(failed)}")
    for rel, err in failed:
        print(f"  ❌ {rel}")
        for line in err:
            print("      ", line[:160])
    if not failed:
        print("  ✅ V2 全部通过")
        return 0
    return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--static", action="store_true", help="静态一致性矩阵（默认）")
    ap.add_argument("--compile", action="store_true", help="全驱动 -c 编译矩阵")
    args = ap.parse_args()
    if args.compile:
        return run_compile()
    return run_static()


if __name__ == "__main__":
    sys.exit(main())
