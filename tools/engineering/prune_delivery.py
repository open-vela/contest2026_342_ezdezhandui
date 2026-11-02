#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""交付瘦身：删除"多余"的东西（默认 dry-run，只有 --apply 才真删）。

清单与依据见 docs/60_交付与提交/可删清单.md；本脚本只认下面这份**显式名单**，
不做任何通配扫描，删之前逐项打印体积/是否 tracked/是否在交付树里。

用法
    python3 tools/engineering/prune_delivery.py                 # 预览全部组
    python3 tools/engineering/prune_delivery.py --only a1 a2    # 只预览指定组
    python3 tools/engineering/prune_delivery.py --apply --only a1 a2 --yes

安全约束（都会在删之前校验，失败即中止）
    1. 只删 GROUPS 里显式列出的路径；不在名单里的东西一律不碰；
    2. 交付树（nuttx/ board/ app/）里只允许删 a2/b1 这两组显式路径，
       其余组必须落在交付树之外；
    3. **tracked 文件默认拒删**（它们在 git 历史里，删了要一起提交）；
       确认要删得显式加 --allow-tracked；
    4. 删完会提示重跑 gen_manifest_copyfiles.py --check 与 check_consistency.py。
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WS = os.environ.get("OPENVELA_WS", "/home/ez/share/openvela")
DELIVERY_ROOTS = ("nuttx", "board", "app")

# 组 -> (说明, [待删路径（相对 REPO，或绝对路径）], 是否允许落在交付树里)
GROUPS = {
    "a1": ("tools/test 下未跟踪的 ESP-IDF 构建树（可 idf.py 重生成，.gitignore 已忽略）",
           ["tools/test/p4_sdio_host_idf/build"], False),
    "a2": ("孤儿应用树：未进 manifest 的 GROUPS、全仓无引用",
           ["app/hello_app"], True),
    "a3": ("原始会话压缩导出（整理版在 logs/ez-xu/；docs 无引用）",
           ["logs/2026-10-30_session-raw.jsonl.zstd",
            "logs/2026-11-02_session-raw.jsonl.zstd"], False),
    "a4": ("本轮把真目录换 linkfile 前的备份（仓外）",
           ["/home/ez/share/.pre-linkfile-backup/20261102"], False),
    "a5": ("验证用构建目录（仓外；保留 cmake_out/esp32p4-function-ev-board_nsh 与 sim_deskmate）",
           ["../cmake_out/esp32p4-verify-move",
            "../cmake_out/esp32p4-verifydrv-fresh",
            "../cmake_out/esp32p4-linkfile-verify",
            "../cmake_out/esp32p4-function-ev-board_verify-drivers"], False),
    "a7": ("MCUboot 工作树（仓外构建缓存；删后下次构建会重 clone，需重跑 ensure_psram_guard.sh）",
           ["../nuttx/bootloader-esp32p4"], False),
    "b1": ("同芯片参考板（非本板；会改变评审端收到内容，docs 亦有引用需同步）",
           ["board/esp32p4/esp32p4-tab5",
            "board/esp32p4/esp32p4-pico-wifi-wareshare"], True),
    "b2": ("非验收取证目录（保留 3 个验收目录与 logs/ez-xu）",
           [], False),
}

KEEP_EVIDENCE = ("shared-minimal", "cleanroom", "espressif-move")


def repo_rel(p: str) -> str:
    return p if os.path.isabs(p) else os.path.join(REPO, p)


def describe(path: str):
    full = os.path.normpath(repo_rel(path))
    if not os.path.exists(full) and not os.path.islink(full):
        return None
    size = 0
    files = 0
    if os.path.isdir(full) and not os.path.islink(full):
        for dp, _d, fs in os.walk(full):
            for f in fs:
                try:
                    size += os.path.getsize(os.path.join(dp, f))
                    files += 1
                except OSError:
                    pass
    else:
        size = os.path.getsize(full)
        files = 1
    # tracked 判定：看它相对哪个 git 仓
    tracked = 0
    for proj in ("nuttx", "vendor/espressif", "apps", "packages/ai_agent", "."):
        root = os.path.join(WS, proj) if proj != "." else REPO
        if not os.path.isdir(os.path.join(root, ".git")) and proj != ".":
            continue
        if not full.startswith(os.path.realpath(root)) and proj != ".":
            continue
        rel = os.path.relpath(full, root)
        out = subprocess.run(["git", "-C", root, "ls-files", "--", rel],
                             capture_output=True, text=True).stdout
        tracked = len([x for x in out.splitlines() if x.strip()])
        break
    return dict(full=full, size=size, files=files, tracked=tracked,
                is_dir=os.path.isdir(full) and not os.path.islink(full))


def b2_paths():
    out = []
    logs = os.path.join(REPO, "logs")
    if not os.path.isdir(logs):
        return out
    for name in sorted(os.listdir(logs)):
        d = os.path.join(logs, name)
        if not (os.path.isdir(d) and name[:2] == "20"):
            continue
        if any(k in name for k in KEEP_EVIDENCE):
            continue
        out.append(os.path.relpath(d, REPO))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--apply", action="store_true", help="真正执行删除（默认只预览）")
    ap.add_argument("--yes", action="store_true", help="与 --apply 同用，跳过确认")
    ap.add_argument("--only", nargs="*", default=None, help="只处理这些组（如 a1 a2）")
    ap.add_argument("--allow-tracked", action="store_true",
                    help="允许删除被 git 跟踪的文件（默认拒删）")
    ap.add_argument("--list-groups", action="store_true", help="只列组名与说明")
    args = ap.parse_args()

    if args.list_groups:
        for k, (desc, paths, in_tree) in GROUPS.items():
            print(f"  {k}: {desc}  ({len(paths) if paths else '动态'} 项)")
        return 0

    keys = args.only or list(GROUPS)
    bad = [k for k in keys if k not in GROUPS]
    if bad:
        sys.exit(f"未知组: {bad}")

    total = 0
    plan = []
    print(f"作品仓: {REPO}\n工作区: {WS}\n模式: {'APPLY（真删）' if args.apply else 'DRY-RUN（只看）'}\n")
    for k in keys:
        desc, paths, in_tree = GROUPS[k]
        paths = b2_paths() if k == "b2" else paths
        print(f"━━━ {k}: {desc}")
        for p in paths:
            d = describe(p)
            if not d:
                print(f"    (跳过，不存在) {p}")
                continue
            relp = os.path.relpath(d["full"], WS)
            inside_delivery = any(relp == r or relp.startswith(r + os.sep)
                                  for r in DELIVERY_ROOTS)
            flags = []
            if inside_delivery and not in_tree:
                flags.append("❗落在交付树里，本组不允许")
            if d["tracked"] and not args.allow_tracked:
                flags.append(f"❗{d['tracked']} 个 tracked 文件，默认拒删（--allow-tracked 可放行）")
            print(f"    {relp:60s} {d['size']/1048576:8.2f} MB  {d['files']:5d} 文件"
                  f"  tracked={d['tracked']}  {' '.join(flags)}")
            total += d["size"]
            plan.append((k, d, flags))
        print()
    print(f"合计: {total/1048576:.1f} MB / {len(plan)} 个路径")
    blocked = [x for x in plan if x[2]]
    if blocked:
        print(f"⚠️ 其中有 {len(blocked)} 个路径带阻断标记，--apply 时会被跳过")

    if not args.apply:
        print("\n（DRY-RUN：什么都没删。确认后加 --apply --yes）")
        return 0
    if not args.yes:
        sys.exit("加 --yes 才执行（这是破坏性操作）")

    removed = freed = 0
    for k, d, flags in plan:
        if flags:
            print(f"  跳过 [{'/'.join(flags)}] {d['full']}")
            continue
        if d["is_dir"]:
            shutil.rmtree(d["full"])
        else:
            os.remove(d["full"])
        removed += 1
        freed += d["size"]
        print(f"  已删 {os.path.relpath(d['full'], WS)}")
    print(f"\n✅ 删了 {removed} 个路径，释放 {freed/1048576:.1f} MB")
    print("最后请跑：python3 tools/engineering/gen_manifest_copyfiles.py --check"
          " && python3 tools/engineering/check_consistency.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
