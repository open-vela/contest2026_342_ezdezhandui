#!/usr/bin/env python3
"""export.py —— 把 openvela 工作区的改动导出到作品仓文件树

与旧 export.sh 的区别
---------------------
旧脚本区分两条路径：
    sync_committed  `git diff BASE..HEAD`   （本地提交部分）
    sync_worktree   `git status --porcelain`（未提交部分，**且跳过 untracked**）
这有两个问题：
  1. 必须保证 BASE 分支存在且正确，否则静默漏掉整块；
  2. **untracked (`??`) 文件被直接跳过** —— 而新增的驱动/源文件恰恰都是 untracked，
     于是"新增文件导不出去"。

现在（在执行过 `tools/engineering/repo_baseline_reset.py` 之后）所有仓库的 HEAD 都停在
manifest 固定节点，本地提交与未提交修改**统一成未提交修改**，因此只需要一条路径：

    git status --porcelain --untracked-files=all

untracked 文件 = 相对基线**新增**的文件，必须导出。

映射关系
--------
每个逻辑仓导出到作品仓的哪个子树，由下面的 SYNC_MAP 定义：
    (project_path, 目标子树, 说明)
映射到作品仓根的 4 棵文件树（nuttx/ board/esp32p4/ app/apps/ app/ai_agent/），
与 manifest 的 <copyfile> 一一对应。

用法
----
    tools/engineering/export.py                 # 预览（dry-run）
    tools/engineering/export.py --apply         # 真正写入作品仓文件树
    tools/engineering/export.py --apply --manifest   # 同时刷新 manifest 的 copyfile 条目
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys

# --------------------------------------------------------------------------
# 配置：工作区 project path -> 作品仓内的目标目录（相对作品仓根）
#
# 映照关系（与 manifest <copyfile> 一一对应）：
#     nuttx/**                          -> nuttx/**             （内核/arch/驱动）
#     vendor/espressif/boards/esp32p4/** -> board/esp32p4/**     （板级源码 + defconfig）
#     apps/**                            -> app/apps/**          （应用：camera 示例 / lvgl / netinit / mbedtls）
#     packages/ai_agent/**               -> app/ai_agent/**      （ai_agent 应用）
# --------------------------------------------------------------------------
SYNC_MAP = [
    ("nuttx",             "nuttx",           "NuttX 内核 / arch / 驱动"),
    ("vendor/espressif",  "board/esp32p4",   "板级源码 + defconfig（openvela vendor 风格）"),
    ("apps",              "app/apps",        "apps 应用（camera 示例、lvgl 端口、netinit…）"),
    ("packages/ai_agent", "app/ai_agent",    "ai_agent 应用"),
]

# 从 vendor/espressif 里只取板级子树，避免把 esp32s3 等无关板子一起带走
SUBTREE_FILTER = {
    "vendor/espressif": "boards/esp32p4/",
}

# 这些文件名属于"仓级/构建级"，不进作品仓
SKIP_NAMES = {".gitignore", ".gitmodules", ".built", ".depend", "Make.dep"}
SKIP_PREFIXES = ("build-esp32p4", "cmake_out/", "out/")


def run(args, cwd=None, check=True):
    p = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    if check and p.returncode != 0:
        raise RuntimeError(f"{' '.join(args)} failed: {p.stderr.strip()}")
    return p.returncode, p.stdout, p.stderr


def find_workspace_root(start):
    cur = os.path.abspath(start)
    while True:
        if os.path.isdir(os.path.join(cur, ".repo")):
            return cur
        parent = os.path.dirname(cur)
        if parent == cur:
            return None
        cur = parent


def repo_root():
    """作品仓根目录（本脚本位于 <repo>/tools/engineering/）。"""
    return os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def collect(repo_path):
    """收集一个仓里相对基线的全部改动。

    返回 (changed, deleted)：
      changed = [(rel_path, kind), ...]  kind in {"add","mod"}
      deleted = [rel_path, ...]
    """
    rc, out, err = run(["git", "-C", repo_path, "status", "--porcelain",
                        "--untracked-files=all"], check=False)
    if rc != 0:
        raise RuntimeError(f"git status 失败: {err.strip()}")

    changed, deleted = [], []
    for line in out.splitlines():
        if len(line) < 4:
            continue
        xy, path = line[:2], line[3:]
        if xy == "!!":
            continue
        # 处理重命名 "R  old -> new"
        if " -> " in path:
            path = path.split(" -> ", 1)[1]

        base = os.path.basename(path)
        if base in SKIP_NAMES:
            continue
        if any(path.startswith(p) for p in SKIP_PREFIXES):
            continue

        # 目录条目 = 嵌套 git 仓（构建期 clone 的依赖，如 arch/…/esp32p4/esp-hal-3rdparty、
        # arch/…/espressif/bootloader）。git 把整棵树当**一个** untracked 条目报出来，
        # 而它有几 GB、manifest <copyfile> 也只能表达文件 —— 绝不能导出。
        # 正常情况这些目录被移植自带的 .gitignore 挡住；这里再兜一层，
        # 避免"ignore 规则被 repo 清掉 / 新依赖没加规则"时把整棵依赖树灌进作品仓。
        # （2026-09-18 实测踩过：旧布局把 5 个 .gitignore 当 copyfile 投递，9/17 重构丢弃后
        #  repo 的 copy-link-files.json 在 repo sync 时把它们从工作区删掉，2GB 依赖树随即
        #  变成"未跟踪改动"。恢复规则 + 本守卫双保险。）
        if os.path.isdir(os.path.join(repo_path, path)):
            print(f"    ⏭  跳过目录条目（疑似嵌套仓/依赖树，不导出）: {path}")
            continue

        if xy == "??":
            changed.append((path, "add"))
        elif "D" in xy:
            deleted.append(path)
        elif "A" in xy:
            changed.append((path, "add"))
        else:
            changed.append((path, "mod"))
    return changed, deleted


def main():
    ap = argparse.ArgumentParser(description="导出工作区改动到作品仓文件树")
    ap.add_argument("--workspace", "-w", default=None)
    ap.add_argument("--apply", action="store_true", help="真正写入（默认预览）")
    ap.add_argument("--manifest", action="store_true",
                    help="同时刷新 manifest 的 <copyfile> 条目")
    ap.add_argument("--only", nargs="*", default=None,
                    help="只导出这些 project path")
    args = ap.parse_args()

    ws = args.workspace or find_workspace_root(os.getcwd())
    if not ws:
        print("❌ 找不到 .repo，请用 --workspace 指定", file=sys.stderr)
        return 2

    root = repo_root()
    only = set(args.only) if args.only else None

    print(f"工作区 : {ws}")
    print(f"作品仓 : {root}\n")

    total_add = total_mod = total_del = 0
    written = []          # 供 manifest 生成用: (src_rel, dest_rel)

    for proj, dest_rel, desc in SYNC_MAP:
        if only and proj not in only:
            continue
        src_repo = os.path.join(ws, proj)
        if not os.path.isdir(os.path.join(src_repo, ".git")):
            print(f"⚠️  跳过 {proj}：不是 git 仓")
            continue

        dst_tree = os.path.join(root, dest_rel)
        # 子路径过滤：只导出该前缀下的改动（如 vendor/espressif 只取 boards/esp32p4/）
        filt = SUBTREE_FILTER.get(proj)
        try:
            changed, deleted = collect(src_repo)
        except RuntimeError as exc:
            print(f"❌ {proj}: {exc}")
            continue

        if filt:
            changed = [(p, k) for p, k in changed if p.startswith(filt)]
            deleted = [p for p in deleted if p.startswith(filt)]

        # 源仓相对路径 -> 目标树相对路径。
        # SUBTREE_FILTER 剥掉前缀时（源 `boards/esp32p4/x` → 目标 `x`），
        # 拷贝、删除、剪枝三处都必须用剥过的路径，否则会出现嵌套目录。
        def _dest_rel(rel):
            if filt and rel.startswith(filt):
                return rel[len(filt):]
            return rel

        print(f"━━━ {proj}  ({desc})")
        print(f"    目标: {dest_rel}")
        print(f"    改动 {len(changed)} 个文件，删除 {len(deleted)} 个")

        if not changed and not deleted:
            print("    （无改动）\n")
            continue

        for rel, kind in sorted(changed):
            s = os.path.join(src_repo, rel)
            d = os.path.join(dst_tree, _dest_rel(rel))
            written.append((os.path.join(dest_rel, _dest_rel(rel)), rel))
            if kind == "add":
                total_add += 1
            else:
                total_mod += 1
            if args.apply:
                os.makedirs(os.path.dirname(d), exist_ok=True)
                if os.path.islink(s):
                    # NuttX 的 board/include/board.h 之类会以符号链接形式存在，
                    # 直接复制会跟随链接（目标可能是目录）→ 必须原样重建链接。
                    # ⚠️ 但 repo 的 <copyfile> 不接受**任何**软链（_SafeExpandPath
                    # 逐段校验，报 "traversing symlinks not allow"）→ 树里出现软链
                    # 就等于评审端缺这个路径，必须改写为普通文件/目录。
                    print(f"    ⚠️ {_dest_rel(rel)} 是软链 —— manifest <copyfile> 不接受"
                          f"任何软链（repo 报 traversing symlinks not allow），"
                          f"评审端将缺失该路径；请改为普通文件/目录在源码层消化"
                          f"（参见 docs/06 §二十四）")
                    if os.path.lexists(d):
                        os.remove(d)
                    os.symlink(os.readlink(s), d)
                else:
                    if os.path.lexists(d):
                        os.remove(d)
                    shutil.copy2(s, d)

        # 剪枝：目标树必须**恰好等于**当前改动集。
        # 否则"文件从本仓搬走/不再改动"时，旧副本会永久残留在作品仓里
        # （搬走的文件在源仓是 untracked，git 不会报删除，靠 deleted 列表抓不到）。
        keep = {_dest_rel(rel) for rel, _k in changed}
        pruned = []
        if args.apply and os.path.isdir(dst_tree):
            for dirpath, _dirs, files in os.walk(dst_tree):
                for fn in files:
                    full = os.path.join(dirpath, fn)
                    rel = os.path.relpath(full, dst_tree)
                    if rel == ".deleted-files" or rel in keep:
                        continue
                    pruned.append(rel)
                    os.remove(full)
            # 清掉空目录
            for dirpath, dirs, files in os.walk(dst_tree, topdown=False):
                if not os.listdir(dirpath) and dirpath != dst_tree:
                    os.rmdir(dirpath)
        if pruned:
            print(f"    🧹 剪枝 {len(pruned)} 个已不在改动集中的旧文件")
            for rel in pruned[:5]:
                print(f"       - {rel}")
            if len(pruned) > 5:
                print(f"       … 另有 {len(pruned)-5} 个")

        # 删除清单：本仓相对基线删除的上游文件（manifest 无法表达删除，只有部署端
        # 能还原）。**按当前状态重算，不累加历史** —— 否则源仓停止删除后旧清单会
        # 永久残留，评审端就会去删一个不该删的文件。空清单直接移除文件本身，使
        # "交付物不需要部署步骤" 成为可校验的不变量。
        if args.apply:
            delfile = os.path.join(dst_tree, ".deleted-files")
            if deleted:
                with open(delfile, "w", encoding="utf-8") as fh:
                    fh.write("\n".join(sorted(set(deleted))) + "\n")
            elif os.path.isfile(delfile):
                os.remove(delfile)
                print("    🧹 移除已失效的 .deleted-files（本仓当前无删除文件）")

        if deleted:
            total_del += len(deleted)
            if args.apply:
                for rel in deleted:
                    d = os.path.join(dst_tree, _dest_rel(rel))
                    if os.path.isfile(d):
                        os.remove(d)

        print()

    print(f"合计: 新增 {total_add}，修改 {total_mod}，删除 {total_del}")
    if not args.apply:
        print("\n这是 dry-run。确认无误后加 --apply 执行。")
        return 0

    print("\n✅ 导出完成")

    if args.manifest:
        gen = os.path.join(root, "tools", "engineering", "gen_manifest_copyfiles.py")
        if os.path.isfile(gen):
            print("\n刷新 manifest copyfile 条目 ...")
            rc, out, err = run([sys.executable, gen], cwd=root, check=False)
            print(out.strip() or err.strip())
        else:
            print("⚠️  未找到 tools/engineering/gen_manifest_copyfiles.py")

    print("\n➡ 提交作品仓：")
    print(f"   cd {root} && git add -A && git commit -m '...'")
    return 0


if __name__ == "__main__":
    sys.exit(main())
