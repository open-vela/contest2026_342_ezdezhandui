#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""工作区 ↔ 作品仓 一致性对账（只读，不写任何东西）。

用途
    事故背景：nuttx 的 git 基线比工作区所用的 openvela drop 旧，`git status`
    会把整片上游更新报成"改动"；而作品仓的交付是按 manifest 的 <copyfile> 把
    文件映射回工作区。一旦两边漂移，就会出现"交付缺文件"或"工作区改了没交付"。
    本脚本把这两个方向都算出来：

      1. 交付完整性：manifest 里每条 <copyfile> 的 src（作品仓）与 dest（工作区）
         是否都存在、内容是否一致（按 sha256 比对）；
      2. 未交付改动：各仓 `git status` 里相对基线的改动，是否都在 manifest 覆盖范围内
         （不在 → 部署后不会生效）；
      3. 保护路径：默认把 `apps/examples/deskmate`（另一个对话在开发的模拟器 UI）
         列为只读保护区，只报告它的状态、绝不建议触碰。

退出码：0 = 一致；1 = 有差异（差异即需要人看的东西）。

用法
    python3 tools/engineering/check_consistency.py
    python3 tools/engineering/check_consistency.py --json     # 机器可读
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WS = os.environ.get("OPENVELA_WS", "/home/ez/share/openvela")
XML = os.path.join(REPO, "contest2026_342_ezdezhandui.xml")

# 只读保护区：另一条对话在开发的东西（模拟器 UI），任何同步/部署都不得触碰。
# 注意它不止 apps/ 一处：模拟器要跑起来，nuttx 侧也有它的痕迹
#   · nuttx/drivers/net/wifi_sim.c          ← tracked 且被修改（repo sync nuttx 会回退它！）
#   · nuttx/boards/sim/sim/sim/configs/deskmate/   ← 新增的板级配置
PROTECTED = [
    "apps/examples/deskmate",
    "nuttx/drivers/net/wifi_sim.c",
    "nuttx/boards/sim/sim/sim/configs/deskmate",
    # 模拟器（goldfish/vela）侧的板级配置由另一条对话维护，不归本流程管
    "vendor/openvela",
]

# 有意不交付的公共层文件（见 gen_manifest_copyfiles.py 的 SHARED_MINIMAL /
# NEVER_DELIVER）：它们既不被编译、也不被 include，评审端用基线版本即可。
# 这里从"未交付改动"里剔除，避免把有意设计报成问题。
import re as _re


def _shared_minimal():
    gen = os.path.join(REPO, "tools/engineering/gen_manifest_copyfiles.py")
    try:
        text = open(gen, encoding="utf-8").read()
    except OSError:
        return set(), set()
    m = _re.search(r"SHARED_MINIMAL = \{(.*?)\}", text, _re.S)
    keep = set(_re.findall(r'"([^"]+)"', m.group(1))) if m else set()
    n = _re.search(r"NEVER_DELIVER = \{(.*?)\}", text, _re.S)
    never = set(_re.findall(r'"([^"]+)"', n.group(1))) if n else set()
    return keep, never


SHARED_KEEP, SHARED_NEVER = _shared_minimal()

# 构建/工具噪声：既不该进交付，也不值得报成"未交付改动"
NOISE_PREFIXES = (
    "build-esp32p4-bootloader/", "build-", "cmake_out/", "out/",
    "chips/esp32p4/",          # vendor 侧构建脚手架（依赖树/生成物）
)
NOISE_NAMES = {
    ".depend", ".gitignore", ".gitmodules", "Make.dep",
    ".ninja_deps", ".ninja_log", "CMakeCache.txt", ".built",
}

# 会被本工具检查改动的仓（项目路径 -> git 仓根）
REPOS = ["nuttx", "vendor/espressif", "apps", "packages/ai_agent"]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def load_copyfiles():
    """读 manifest 的 <copyfile src dest>，返回 [(src_in_repo, dest_in_ws)]。"""
    if not os.path.exists(XML):
        sys.exit(f"缺少 manifest: {XML}")
    root = ET.parse(XML).getroot()
    pairs = []
    for el in root.iter("copyfile"):
        src = el.get("src")
        dest = el.get("dest")
        if src and dest:
            pairs.append((src, dest))
    # <linkfile> 也可用于整棵子树（symlink）：展开成逐文件比较，
    # 只处理 src 落在本仓内的那些（AI skills 项目的 linkfile 不属于本仓）
    for el in root.iter("linkfile"):
        src = el.get("src")
        dest = el.get("dest")
        if not src or not dest:
            continue
        full = os.path.join(REPO, src)
        if not os.path.isdir(full):
            continue
        for dirpath, _, files in os.walk(full):
            for f in files:
                rel = os.path.relpath(os.path.join(dirpath, f), full)
                pairs.append((os.path.join(src, rel),
                              os.path.join(dest, rel).replace(os.sep, "/")))
    return pairs


def check_delivery(pairs):
    """交付完整性：src/dest 存在性与内容一致性。"""
    missing_repo, missing_ws, differ = [], [], []
    for src, dest in pairs:
        rs = os.path.join(REPO, src)
        wd = os.path.join(WS, dest)
        if not os.path.exists(rs):
            missing_repo.append(src)
            continue
        if not os.path.exists(wd):
            missing_ws.append(dest)
            continue
        if under_protected(dest):
            continue
        try:
            if sha256(rs) != sha256(wd):
                differ.append(dest)
        except OSError:
            differ.append(dest)
    return missing_repo, missing_ws, differ


def git_changed(proj):
    """(changed, deleted)，相对各仓 git 基线。"""
    root = os.path.join(WS, proj)
    if not os.path.isdir(os.path.join(root, ".git")):
        return [], []
    r = subprocess.run(["git", "-C", root, "status", "--porcelain",
                        "--untracked-files=all"],
                       capture_output=True, text=True)
    changed, deleted = [], []
    for line in r.stdout.splitlines():
        if len(line) < 4:
            continue
        xy, path = line[:2], line[3:]
        if " -> " in path:
            path = path.split(" -> ", 1)[1]
        if xy == "!!":
            continue
        if "D" in xy:
            deleted.append(path)
        else:
            changed.append(path)
    return changed, deleted


def under_protected(path):
    return any(path == p or path.startswith(p + "/") for p in PROTECTED)


SHARED_PREFIX = "nuttx/arch/risc-v/src/common/espressif/"


def is_intentional_undelivered(proj, rel):
    """公共层里"有意不交付"的文件（最小集之外的直属文件 + NEVER_DELIVER）。"""
    full = proj + "/" + rel
    if full in SHARED_NEVER:
        return True
    if full.startswith(SHARED_PREFIX):
        base = full[len(SHARED_PREFIX):]
        if "/" not in base and base not in SHARED_KEEP:
            return True
    return False


# 构建期符号链接（板级 include 重定向），由构建创建、不属于交付内容
BUILD_ARTIFACTS = (
    "boards/esp32p4/common/board",
)


def is_noise(rel):
    if rel in BUILD_ARTIFACTS:
        return True
    if any(rel.startswith(p) for p in NOISE_PREFIXES):
        return True
    base = os.path.basename(rel)
    if base in NOISE_NAMES:
        return True
    return "/CMakeFiles/" in rel


def touches_protected(proj, rel):
    full = proj + "/" + rel if not proj.endswith("/") else proj + rel
    return under_protected(full)


def check_undelivered(pairs):
    """各仓改动里，哪些没有对应的 copyfile（部署后不会生效）。"""
    # dest（工作区路径）-> 是否在 manifest 覆盖内
    covered = {dest.rstrip("/") for _, dest in pairs}

    def is_covered(proj, rel):
        full = os.path.join(proj, rel)
        if full in covered:
            return True
        # 目录形式的条目（copyfile 指向目录）也视为覆盖
        for c in covered:
            if full.startswith(c + "/"):
                return True
        return False

    report = {}
    for proj in REPOS:
        changed, deleted = git_changed(proj)
        uncovered, protected_dirty, noise = [], [], 0
        for p in changed:
            if is_noise(p):
                noise += 1
                continue
            if is_intentional_undelivered(proj, p):
                noise += 1
                continue
            if touches_protected(proj, p):
                protected_dirty.append(p)
                continue
            if not is_covered(proj, p):
                uncovered.append(p)
        report[proj] = {
            "changed": len(changed),
            "deleted": len(deleted),
            "noise": noise,
            "protected_dirty": protected_dirty,
            "uncovered": uncovered,
        }
    return report


def check_protected():
    out = {}
    for p in PROTECTED:
        full = os.path.join(WS, p)
        if not os.path.exists(full):
            out[p] = {"present": False}
            continue
        if os.path.isfile(full):
            out[p] = {"present": True, "files": 1, "digest": sha256(full)[:16]}
            continue
        n = 0
        digest = hashlib.sha256()
        for root, _, files in os.walk(full):
            for f in sorted(files):
                fp = os.path.join(root, f)
                n += 1
                digest.update(os.path.relpath(fp, full).encode())
                try:
                    digest.update(sha256(fp).encode())
                except OSError:
                    pass
        out[p] = {"present": True, "files": n, "digest": digest.hexdigest()[:16]}
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--json", action="store_true", help="输出 JSON")
    args = ap.parse_args()

    pairs = load_copyfiles()
    missing_repo, missing_ws, differ = check_delivery(pairs)
    undelivered = check_undelivered(pairs)
    protected = check_protected()

    ok = not (missing_repo or missing_ws or differ) and \
        not any(v["uncovered"] for v in undelivered.values())

    if args.json:
        print(json.dumps({
            "manifest_entries": len(pairs),
            "missing_in_repo": missing_repo,
            "missing_in_workspace": missing_ws,
            "content_differs": differ,
            "repos": undelivered,
            "protected": protected,
            "ok": ok,
        }, ensure_ascii=False, indent=2))
        return 0 if ok else 1

    print("== 工作区 ↔ 作品仓 一致性对账 ==")
    print(f"  工作区: {WS}")
    print(f"  作品仓: {REPO}")
    print(f"  manifest 条目: {len(pairs)}")
    print()
    print(f"  1) 作品仓缺文件（manifest 指向但仓里没有）: {len(missing_repo)}")
    for p in missing_repo[:8]:
        print("     -", p)
    print(f"  2) 工作区缺文件（部署后会创建）: {len(missing_ws)}")
    for p in missing_ws[:8]:
        print("     -", p)
    print(f"  3) 内容不一致（交付的是旧版本）: {len(differ)}")
    for p in differ[:8]:
        print("     -", p)
    print()
    print("  4) 各仓改动的交付覆盖情况（已过滤构建噪声）:")
    for proj, v in undelivered.items():
        status = "OK" if not v["uncovered"] else f"未交付 {len(v['uncovered'])} 个"
        print(f"     {proj}: 改动 {v['changed']}（噪声 {v['noise']}）, 删除 {v['deleted']}, {status}")
        for p in v["uncovered"][:6]:
            print("        ·", p)
        if v["protected_dirty"]:
            print(f"        🔒 其中 {len(v['protected_dirty'])} 个属于只读保护区（不交付也不得同步覆盖）:")
            for p in v["protected_dirty"][:6]:
                print("           ", p)
    print()
    print("  5) 只读保护区（不得被同步/部署触碰）:")
    for p, v in protected.items():
        if v.get("present"):
            print(f"     {p}: {v['files']} 个文件, 指纹 {v['digest']}")
        else:
            print(f"     {p}: 不存在")
    print()
    print("  ✅ 一致" if ok else "  ❌ 有差异（见上）")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
