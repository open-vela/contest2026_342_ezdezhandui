#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""重新生成作品仓 manifest 的 <copyfile> 清单。

背景
    contest2026_342_ezdezhandui.xml 通过 <copyfile> 把本仓文件树映射回 openvela 工作区位置：

        nuttx/<rel>           -> nuttx/<rel>
        board/esp32p4/<rel>   -> vendor/espressif/boards/esp32p4/<rel>
        board/vela/<rel>      -> vendor/openvela/boards/vela/<rel>
        app/apps/<rel>        -> apps/<rel>
        app/ai_agent/<rel>    -> packages/ai_agent/<rel>

    文件树由 tools/engineering/export.py 维护，本脚本保证清单与文件树**严格一一对应**：
    新增文件自动补条目、已删除文件自动移除条目、原清单中的"树外条目"（不落在 4 棵
    文件树内的仓根/文档条目）原样保留。

用法
    python3 tools/engineering/gen_manifest_copyfiles.py            # 就地更新 xml
    python3 tools/engineering/gen_manifest_copyfiles.py --check    # 只校验，不写入（差异即非零退出）

校验项
    1. 树中每个文件都有 copyfile 条目（除 .deleted-files；含 .gitignore 等点文件）
    2. 每条 copyfile 的 src 在仓内真实存在
    3. dest 全局唯一
    4. 不存在"需要部署端删除上游文件"的 .deleted-files
       （manifest 只能 copy/link，无法表达删除 → 那会逼评审多跑一步部署脚本）
"""

import argparse
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
WS = os.environ.get("OPENVELA_WS", "/home/ez/share/openvela")
XML = os.path.join(REPO, "contest2026_342_ezdezhandui.xml")
# 作品仓内目录 -> openvela 工作区目标前缀
# 只读保护区（作品仓内路径）：另一条对话在开发的模拟器 UI 及其 sim/emulator 侧配置。
# 它们**绝不进本仓交付**——copyfile 部署会把旧副本盖回别人的工作区。
PROTECTED_REPO_PATHS = (
    "app/apps/examples/deskmate",
    "nuttx/drivers/net/wifi_sim.c",
    "nuttx/boards/sim/sim/sim/configs/deskmate",
    "board/vela",                 # vendor/openvela（goldfish/vela 板级）
)

GROUPS = [
    ("nuttx",          "nuttx"),
    ("board/esp32p4",  "vendor/espressif/boards/esp32p4"),
    ("board/vela",     "vendor/openvela/boards/vela"),
    ("app/apps",       "apps"),
    ("app/ai_agent",   "packages/ai_agent"),
]

COPYFILE_RE = re.compile(r'^(\s*)<copyfile src="([^"]+)" dest="([^"]+)"\s*/>\s*$')
SRC_PREFIX = ""
GROUP_DIRS = [g[0] for g in GROUPS]

# 旧布局前缀：迁移期必须显式丢弃，否则它们会被当成"树外条目"保留下来，
# 与新布局条目产生同 dest 冲突。
LEGACY_PREFIXES = ["board/esp32p4_vela/"]


def tree_entries():
    """枚举各组文件树 -> [(src, dest)]，顺序稳定。

    符号链接必须单独处理：`os.walk` 默认不跟随指向目录的链接，这类链接
    既不会出现在 filenames 里，也不会被递归进去 —— 于是会被整个漏掉。
    板级的 `common/board` 正是这种链接。
    """
    out = []
    for sub, dest_prefix in GROUPS:
        root = os.path.join(REPO, sub)
        if not os.path.isdir(root):
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames.sort()
            for d in list(dirnames):
                if os.path.islink(os.path.join(dirpath, d)):
                    rel = os.path.relpath(os.path.join(dirpath, d), root)
                    out.append((sub + "/" + rel, dest_prefix + "/" + rel))
                    dirnames.remove(d)          # 不递归进去
            for name in sorted(filenames):
                if name == ".deleted-files":
                    continue
                rel = os.path.relpath(os.path.join(dirpath, name), root)
                src = sub + "/" + rel
                if any(src == q or src.startswith(q + "/")
                       for q in PROTECTED_REPO_PATHS):
                    continue
                out.append((src, dest_prefix + "/" + rel))
    return out


def linkfile_groups(entries, threshold=20):
    """找出可以用**一条 <linkfile>** 代替逐文件 <copyfile> 的自包含子树。

    判定（全部满足，计数一律**递归**）：
      1. 子树在工作区里没有任何 tracked 文件 —— 整棵子树都归本项目（全是新增/改动），
         用 symlink 替换不会遮住上游内容；
      2. 子树里工作区的文件数 == 待交付条目数 —— 否则 symlink 会丢掉文件；
      3. 文件数 >= threshold；
      4. 结构一致：src 与 dest 去掉各自前缀后逐文件相同（纯前缀替换）。
    只保留互不包含的**最大**子树。
    """
    import subprocess

    by_dest = {d: (s_, d) for s_, d in entries}
    dests = list(by_dest)

    # 候选：每条 dest 的所有祖先目录
    cand = set()
    for d in dests:
        parts = d.split("/")
        for i in range(1, len(parts)):
            cand.add("/".join(parts[:i]))

    # 项目根 -> tracked 文件清单（缓存）
    cache = {}

    def split_proj(ws_rel):
        if ws_rel.startswith("vendor/espressif/"):
            return "vendor/espressif", ws_rel[len("vendor/espressif/"):]
        if ws_rel.startswith("packages/ai_agent/"):
            return "packages/ai_agent", ws_rel[len("packages/ai_agent/"):]
        top = ws_rel.split("/", 1)[0]
        return top, ws_rel[len(top) + 1:]

    def tracked_under(ws_dir):
        proj, rel = split_proj(ws_dir)
        if proj not in cache:
            r = subprocess.run(["git", "-C", os.path.join(WS, proj), "ls-files"],
                               capture_output=True, text=True)
            cache[proj] = r.stdout.splitlines()
        return [f for f in cache[proj] if f == rel or f.startswith(rel + "/")]

    def ws_files(ws_dir):
        full = os.path.join(WS, ws_dir)
        if not os.path.isdir(full):
            return []
        out = []
        for root, _, fs in os.walk(full):
            for f in fs:
                out.append(os.path.relpath(os.path.join(root, f), full))
        return out

    ok = []
    for d in sorted(cand, key=lambda x: (x.count("/"), x)):
        items = [(s_, dd) for s_, dd in entries
                 if dd == d or dd.startswith(d + "/")]
        if len(items) < threshold:
            continue
        if tracked_under(d):
            continue
        wf = ws_files(d)
        if len(wf) != len(items):
            continue
        # 结构一致：src/dest 去前缀后逐文件相同
        repo_dir = None
        good = True
        for s_, dd in items:
            rel = dd[len(d) + 1:]
            if not s_.endswith("/" + rel):
                good = False
                break
            r_dir = s_[: len(s_) - len(rel) - 1]
            if repo_dir is None:
                repo_dir = r_dir
            elif repo_dir != r_dir:
                good = False
                break
        if not good or not repo_dir:
            continue
        ok.append((len(items), d, repo_dir))

    ok.sort(key=lambda t: -t[0])
    chosen = []
    for n, d, r in ok:
        if any(d == cd or d.startswith(cd + "/") for _, cd, _r in chosen):
            continue
        chosen.append((n, d, r))
    return [(r, d, n) for n, d, r in chosen]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="只校验不写入")
    ap.add_argument("--linkfile-threshold", type=int, default=20,
                    help="自包含子树文件数达到该值时改用一条 <linkfile>（默认 20）")
    ap.add_argument("--no-linkfile", action="store_true",
                    help="关闭 linkfile 混合策略，全部用 <copyfile>")
    ap.add_argument("--prune", action="store_true",
                    help="删除作品仓内的保护区残留（另一条对话的模拟器 UI）后再生成")
    args = ap.parse_args()

    if args.prune:
        import shutil
        n = 0
        for q in PROTECTED_REPO_PATHS:
            full = os.path.join(REPO, q)
            if os.path.exists(full):
                shutil.rmtree(full) if os.path.isdir(full) else os.remove(full)
                n += 1
        print(f"prune: 已删除作品仓内 {n} 个保护区残留路径")

    with open(XML, encoding="utf-8") as f:
        lines = f.read().split("\n")

    first = next(i for i, l in enumerate(lines) if COPYFILE_RE.match(l))
    LINKFILE_RE = re.compile(r'(\s*)<linkfile src="([^"]+)" dest="([^"]+)"\s*/>')
    last = max(i for i, l in enumerate(lines) if COPYFILE_RE.match(l))

    existing = []
    for l in lines[first:last + 1]:
        m = COPYFILE_RE.match(l)
        if m:
            existing.append((m.group(2), m.group(3)))

    def ours(src):
        return any(src == g or src.startswith(g + "/") for g in GROUP_DIRS)

    old_links = []
    for l in lines[:first]:
        m = LINKFILE_RE.match(l)
        if m and ours(m.group(2)):
            old_links.append((m.group(2), m.group(3)))

    # 树外条目（例如仓根文档）原样保留，且在清单末尾
    wanted_dests = {d for _s, d in tree_entries()}
    extras = [(s, d) for s, d in existing
              if not any(s == g or s.startswith(g + "/") for g in GROUP_DIRS)
              and not any(s.startswith(p) for p in LEGACY_PREFIXES)
              and not any(s == q or s.startswith(q + "/")
                          for q in PROTECTED_REPO_PATHS)
              and d not in wanted_dests]

    wanted = tree_entries()

    # ── 混合策略：自包含子树用一条 <linkfile>，其余逐文件 <copyfile> ──────────
    links = [] if args.no_linkfile else linkfile_groups(wanted, args.linkfile_threshold)
    if links:
        link_srcs = [r for r, _d, _n in links]
        kept = []
        for src, dest in wanted:
            if any(src == r or src.startswith(r + "/") for r in link_srcs):
                continue
            kept.append((src, dest))
        dropped = len(wanted) - len(kept)
        wanted = kept
        print("linkfile 子树 : %d 个（替代 %d 条 copyfile）" % (len(links), dropped))
        for r, d, n in links:
            print("   ~ %s  (%d 文件)  ->  %s" % (r, n, d))

    wanted.sort(key=lambda sd: (sd[1].split("/")[0], sd[0]))

    old_set = {s for s, _ in existing}
    new_set = {s for s, _ in wanted}
    extra_set = {s for s, _ in extras}
    added = sorted(new_set - old_set)
    removed = sorted(old_set - new_set - extra_set)

    problems = []

    # 1. 树覆盖：wanted 已含全部树内文件；检查是否有条目 src 不存在
    for src, dest in wanted + extras:
        if not os.path.lexists(os.path.join(REPO, src)):
            problems.append("src 不存在: " + src)

    # 2. dest 唯一
    dests = [d for _, d in wanted + extras]
    dup = sorted({d for d in dests if dests.count(d) > 1})
    for d in dup:
        problems.append("dest 重复: " + d)

    # 3. 条数与树文件数一致
    n_tree = len(wanted)
    print("文件树条目 : %d" % n_tree)
    print("清单条目   : %d（含树外 %d）" % (len(wanted) + len(extras), len(extras)))
    print("新增       : %d" % len(added))
    for p in added[:25]:
        print("   + " + p)
    if len(added) > 25:
        print("   ... 其余 %d 条" % (len(added) - 25))
    print("移除       : %d" % len(removed))
    for p in removed[:25]:
        print("   - " + p)

    # 3. 不得存在"需要部署端删除上游文件"的清单
    #    repo manifest 只能 copyfile/linkfile，**无法表达删除**：一旦某仓相对
    #    基线删了上游文件，评审端 repo sync 之后该文件仍然在，只有手工部署
    #    （deploy.sh）才会删 —— 那正是我们要消灭的步骤。所以这里直接判失败，
    #    迫使在移植源码里消化冲突（例如改用 <esp_timer.h> 这类非遮蔽 include）。
    for sub, _dest_prefix in GROUPS:
        delfile = os.path.join(REPO, sub, ".deleted-files")
        if not os.path.isfile(delfile):
            continue
        with open(delfile, encoding="utf-8") as fh:
            entries = [x.strip() for x in fh if x.strip()]
        if entries:
            head = ", ".join(entries[:3]) + (" ..." if len(entries) > 3 else "")
            problems.append(
                "删除清单非空（manifest 无法复现删除，评审端会缺这一步）: %s -> %s"
                % (os.path.relpath(delfile, REPO), head)
            )

    # 4. 文件树里不能有**任何软链**。
    #    <copyfile> 的 src 会经 repo 的 _SafeExpandPath() 逐段校验，只要路径上
    #    出现软链（含最后一个组件、含指向普通文件的软链）就直接抛
    #    ManifestInvalidPathError: "<path>: traversing symlinks not allow"，
    #    评审端 repo sync 就少这个路径 → 构建失败。而 <linkfile> 只能把 dest
    #    链到作品仓内的路径，无法还原"链到工作区内的相对目标"。
    #    （已用 repo 源码实测：目录软链与文件软链都报错，见 docs/01_开发流程/开发过程复盘与改进清单.md §24.3）
    for sub, _dest_prefix in GROUPS:
        root = os.path.join(REPO, sub)
        if not os.path.isdir(root):
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            for name in list(dirnames) + list(filenames):
                p = os.path.join(dirpath, name)
                if not os.path.islink(p):
                    continue
                problems.append(
                    "文件树含软链（copyfile src 不允许，repo sync 会抛 "
                    "traversing symlinks not allowed）: %s -> %s"
                    % (os.path.relpath(p, REPO), os.readlink(p))
                )

    if problems:
        print("\n❌ 校验失败:")
        for p in problems:
            print("   " + p)
        return 1

    link_added = [x for x in links if (x[0], x[1]) not in old_links]
    link_removed = [x for x in old_links if x not in [(r, d) for r, d, _n in links]]

    if args.check:
        if added or removed or link_added or link_removed:
            print("\n❌ 清单与文件树不一致（--check 模式不写入）")
            for r, d, n in link_added:
                print("   + linkfile %s -> %s (%d 文件)" % (r, d, n))
            for r, d in link_removed:
                print("   - linkfile %s -> %s" % (r, d))
            return 1
        print("\n✅ 清单与文件树一致（linkfile %d 条）" % len(links))
        return 0

    out = [l for l in lines[:first]
           if not (LINKFILE_RE.match(l) and ours(LINKFILE_RE.match(l).group(2)))]
    out += ['    <linkfile src="%s" dest="%s"/>' % (r, d) for r, d, _n in links]
    out += ['    <copyfile src="%s" dest="%s"/>' % (s, d) for s, d in wanted]
    out += ['    <copyfile src="%s" dest="%s"/>' % (s, d) for s, d in extras]
    out += ['  </project>', '</manifest>', '']

    with open(XML, "w", encoding="utf-8") as f:
        f.write("\n".join(out))

    print("\n✅ 已写入 %s（%d 条 copyfile + %d 条 linkfile）"
          % (os.path.relpath(XML, REPO), len(wanted) + len(extras), len(links)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
