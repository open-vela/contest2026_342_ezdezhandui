#!/usr/bin/env python3
"""repo_baseline_reset.py —— 把所有 repo 仓库的 HEAD 归位到 manifest 固定节点

背景
----
本工作区是 openvela 的 `repo` 检出。开发过程中 nuttx / apps / packages/ai_agent
等仓库会累积**本地提交**（各自的分支），同时还有**未提交修改**。

这给"导出到作品仓"造成麻烦：导出的 diff 必须同时覆盖
    `git diff <基线>..HEAD`（已提交部分）  +  `git diff HEAD`（未提交部分）
两条路径，容易漏。

本工具把两者**统一成一种形态**：

    HEAD  ->  manifest 固定节点
    本地已有的提交 + 当前未提交修改  ->  全部变成"相对该节点的未提交修改"

于是导出只需要一条 `git diff HEAD`（配合 `git status` 处理新增/删除文件）。

安全性
------
* **默认 dry-run**，只打印将要做什么，不改任何东西；执行需要显式 `--apply`。
* 执行前为每个仓库写一个备份 ref：
      refs/backup/pre-baseline-reset-<UTC 时间戳>
  指向**原 HEAD**。`git reset --mixed` 不会丢弃任何对象，
  所以只要有这个 ref，随时可以 `git reset --hard <backup-ref>` 完整还原。
  还原方法会打印在结尾。
* 未跟踪文件不受影响（reset --mixed 不碰它们）。

用法
----
    tools/engineering/repo_baseline_reset.py                    # 预览（dry-run）
    tools/engineering/repo_baseline_reset.py --apply            # 真正执行
    tools/engineering/repo_baseline_reset.py --only nuttx apps  # 只处理指定 project path
    tools/engineering/repo_baseline_reset.py --only-dirty       # 只处理有本地提交或脏改动的仓库
    tools/engineering/repo_baseline_reset.py --apply --restore refs/backup/pre-baseline-reset-20260916T120000Z

节点来源
--------
按优先级：
  1. `--node <path>=<rev>`  显式指定某个仓库的节点（可重复）
  2. manifest 中该 project 的 `revision` 属性
  3. manifest 的 `<default revision=...>`
其中 SHA 形式直接使用；分支/标签形式解析为 `refs/remotes/<remote>/<rev>`。
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from datetime import datetime, timezone

SHA_RE = re.compile(r"^[0-9a-f]{7,40}$")


# --------------------------------------------------------------------------
# helpers
# --------------------------------------------------------------------------

def run(args, cwd=None, check=True):
    """跑一条命令，返回 (rc, stdout, stderr)。"""
    p = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    if check and p.returncode != 0:
        raise RuntimeError(f"{' '.join(args)} failed: {p.stderr.strip()}")
    return p.returncode, p.stdout.strip(), p.stderr.strip()


def git(repo, *args, check=True):
    return run(["git", "-C", repo, *args], check=check)


def worktree_tree(repo):
    """把"工作区完整状态"（含未跟踪文件）写成一个 tree 对象并返回其 SHA。

    做法：`git add -A` -> `git write-tree` -> `git reset -q`（把索引退回 HEAD，
    工作区不变）。**不产生任何提交**，只借 write-tree 算一个内容哈希。

    这是归位前后"内容完全等价"的证明手段：reset --mixed 只移动 HEAD 指针，
    不该改变工作区内容，所以前后算出的 tree 必须**逐位相同**。
    """
    git(repo, "add", "-A")
    _rc, tree, _e = git(repo, "write-tree")
    git(repo, "reset", "-q")
    return tree


def find_workspace_root(start):
    """向上找 .repo 目录。"""
    cur = os.path.abspath(start)
    while True:
        if os.path.isdir(os.path.join(cur, ".repo")):
            return cur
        parent = os.path.dirname(cur)
        if parent == cur:
            return None
        cur = parent


def load_manifest(ws, manifest_file=None):
    """取合并后的 manifest（含 include/default 展开）。"""
    if manifest_file:
        with open(manifest_file, "r", encoding="utf-8") as fh:
            return ET.fromstring(fh.read())

    rc, out, err = run(["repo", "manifest"], cwd=ws, check=False)
    if rc != 0 or "<manifest" not in out:
        raise RuntimeError(
            "无法获取 manifest：`repo manifest` 失败\n"
            f"  rc={rc}\n  stderr={err[:400]}\n"
            "  可用 --manifest <file> 直接指定 manifest xml"
        )
    return ET.fromstring(out)


def parse_projects(root):
    """从 manifest 解析出 [(path, name, remote, revision), ...]。"""
    remotes = {}
    for r in root.findall("remote"):
        remotes[r.get("name")] = r.get("fetch", "")

    default = root.find("default")
    d_remote = default.get("remote") if default is not None else None
    d_rev = default.get("revision") if default is not None else None

    projects = []
    for p in root.findall("project"):
        name = p.get("name")
        path = p.get("path") or name
        if path is None:
            continue
        remote = p.get("remote") or d_remote
        rev = p.get("revision") or d_rev
        projects.append((path, name, remote, rev))
    return projects, remotes, d_rev


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description="把所有 repo 仓库的 HEAD 归位到 manifest 固定节点",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--workspace", "-w", default=None,
                    help="openvela 工作区根（默认从 cwd 向上找 .repo）")
    ap.add_argument("--manifest", default=None,
                    help="直接用指定的 manifest xml，而不是跑 `repo manifest`")
    ap.add_argument("--apply", action="store_true",
                    help="真正执行（默认只预览）")
    ap.add_argument("--only", nargs="*", default=None,
                    help="只处理这些 project path（如 nuttx apps packages/ai_agent）")
    ap.add_argument("--only-dirty", action="store_true",
                    help="只处理有本地提交或未提交改动的仓库")
    ap.add_argument("--node", action="append", default=[],
                    metavar="PATH=REV",
                    help="显式指定某仓库的基线节点（可重复）")
    ap.add_argument("--restore", default=None, metavar="BACKUP_REF",
                    help="把 HEAD 还原到某个备份 ref（与 --apply 同用）")
    ap.add_argument("--tag", default=None,
                    help="备份 ref 的时间戳后缀（默认当前 UTC）")
    args = ap.parse_args()

    ws = args.workspace or find_workspace_root(os.getcwd())
    if not ws:
        print("❌ 找不到 .repo —— 请在 openvela 工作区内运行，或用 --workspace 指定",
              file=sys.stderr)
        return 2

    overrides = {}
    for item in args.node:
        if "=" not in item:
            print(f"❌ --node 需要 PATH=REV 形式: {item}", file=sys.stderr)
            return 2
        k, v = item.split("=", 1)
        overrides[k] = v

    root = load_manifest(ws, args.manifest)
    projects, _remotes, _d_rev = parse_projects(root)

    # ---- restore 模式 ----
    if args.restore:
        if not args.apply:
            print("❌ --restore 需要同时给 --apply")
            return 2
        print(f"↩️  还原 HEAD -> {args.restore}\n")
        bad = 0
        for path, _n, _r, _rv in projects:
            repo = os.path.join(ws, path)
            if not os.path.isdir(os.path.join(repo, ".git")):
                continue
            rc, _o, _e = git(repo, "rev-parse", "--verify", args.restore, check=False)
            if rc != 0:
                continue
            git(repo, "reset", "--hard", args.restore)
            print(f"  ✅ {path:45s} -> {args.restore}")
        print("\n完成。")
        return 0 if bad == 0 else 1

    tag = args.tag or datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    backup_ref = f"refs/backup/pre-baseline-reset-{tag}"

    only = set(args.only) if args.only else None

    rows = []
    for path, name, remote, rev in projects:
        if only and path not in only:
            continue
        repo = os.path.join(ws, path)
        if not os.path.isdir(os.path.join(repo, ".git")):
            continue

        # 1) 解析基线节点
        r = overrides.get(path, rev)
        if r is None:
            rows.append((path, "-", "-", 0, 0, "skip", "manifest 无 revision", None))
            continue
        if SHA_RE.match(r):
            node_ref = r
        else:
            node_ref = f"refs/remotes/{remote}/{r}"
        rc, node_sha, _e = git(repo, "rev-parse", "--verify", f"{node_ref}^{{commit}}",
                               check=False)
        if rc != 0:
            rows.append((path, "-", r, 0, 0, "skip", f"节点不存在: {node_ref}", None))
            continue

        rc, head_sha, _e = git(repo, "rev-parse", "HEAD", check=False)
        if rc != 0:
            rows.append((path, "-", r, 0, 0, "skip", "无 HEAD", None))
            continue

        # 2) 统计本地提交 / 未提交改动
        rc, ahead_s, _e = git(repo, "rev-list", "--count", f"{node_sha}..HEAD", check=False)
        ahead = int(ahead_s) if rc == 0 and ahead_s.isdigit() else 0
        rc, dirty_s, _e = git(repo, "status", "--porcelain", check=False)
        dirty = len([x for x in dirty_s.splitlines() if x.strip()]) if rc == 0 else 0

        if head_sha == node_sha and dirty == 0:
            action, note = "clean", "-"
        elif head_sha == node_sha:
            action, note = "clean", f"仅未提交 ({dirty})"
        else:
            action, note = "RESET", f"{node_sha[:10]}"

        if args.only_dirty and action == "clean" and dirty == 0:
            continue

        rows.append((path, head_sha[:10], node_sha[:10], ahead, dirty, action, note, node_sha))

    if not rows:
        print("没有需要处理的仓库。")
        return 0

    # ---- 打印计划 ----
    to_reset = [x for x in rows if x[5] == "RESET"]
    w = max(len(x[0]) for x in rows) + 2
    print(f"\n工作区: {ws}")
    print(f"备份 ref: {backup_ref}\n")
    print(f"{'PROJECT':<{w}} {'HEAD':<11} {'NODE':<11} {'本地提交':>8} {'未提交':>7}  动作")
    print("-" * (w + 52))
    for path, h, n, ahead, dirty, action, note, _full in sorted(rows, key=lambda x: x[0]):
        print(f"{path:<{w}} {h:<11} {n:<11} {ahead:>8} {dirty:>7}  {action}"
              + (f"  ({note})" if note != "-" else ""))

    print(f"\n合计 {len(rows)} 个仓库，其中 {len(to_reset)} 个需要归位。")

    if not args.apply:
        print("\n这是 dry-run。确认无误后加 --apply 执行。")
        return 0

    if not to_reset:
        print("\n无需改动。")
        return 0

    # ---- 执行 ----
    print(f"\n开始归位（备份 ref: {backup_ref}）...\n")
    done = 0
    failed = 0
    for path, h, n, ahead, dirty, action, note, full in sorted(to_reset, key=lambda x: x[0]):
        repo = os.path.join(ws, path)
        try:
            # 归位前的"工作区完整内容"哈希
            tree_before = worktree_tree(repo)

            git(repo, "update-ref", backup_ref, "HEAD")
            git(repo, "reset", "--mixed", full)

            # 归位后必须得到**完全相同**的内容哈希
            tree_after = worktree_tree(repo)

            rc, dirty_s, _e = git(repo, "status", "--porcelain", check=False)
            after = len([x for x in dirty_s.splitlines() if x.strip()]) if rc == 0 else -1

            if tree_before != tree_after:
                # 理论上不可能发生：reset --mixed 不碰工作区。
                # 真发生了就立刻用备份 ref 还原，避免留下半坏状态。
                git(repo, "reset", "--hard", backup_ref)
                print(f"  ❌ {path}: 内容哈希不一致！已自动还原\n"
                      f"       before={tree_before}\n       after ={tree_after}")
                failed += 1
                continue

            done += 1
            print(f"  ✅ {path:<45} {h} -> {n}   未提交: {after} 项"
                  f"   [tree {tree_after[:12]} 一致]")
        except RuntimeError as exc:
            print(f"  ❌ {path}: {exc}")
            failed += 1

    print(f"\n完成 {done}/{len(to_reset)} 个仓库" + (f"，失败 {failed}" if failed else "") + "。")
    if failed:
        print("⚠️  有仓库失败，请检查上面的输出；对应仓库可用备份 ref 还原。")
    print(f"\n如需整体还原：\n  git -C <repo> reset --hard {backup_ref}")
    print(f"或：\n  {os.path.relpath(__file__, ws)} --apply --restore {backup_ref}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
