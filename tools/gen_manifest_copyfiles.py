#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""重新生成作品仓 manifest 的 <copyfile> 清单。

背景
    contest2026_342_ezdezhandui.xml 通过 <copyfile> 把本仓
    board/esp32p4_vela/{nuttx,apps,ai_agent}/ 文件树映射回 openvela 工作区位置：

        board/esp32p4_vela/nuttx/<rel>      -> nuttx/<rel>
        board/esp32p4_vela/apps/<rel>        -> apps/<rel>
        board/esp32p4_vela/ai_agent/<rel>    -> packages/ai_agent/<rel>

    文件树由 board/esp32p4_vela/export.sh 维护，本脚本保证清单与文件树**严格一一对应**：
    新增文件自动补条目、已删除文件自动移除条目、原清单中的"树外条目"（例如
    CSI_INTEGRATOR_HANDOFF.md）原样保留。

用法
    python3 tools/gen_manifest_copyfiles.py            # 就地更新 xml
    python3 tools/gen_manifest_copyfiles.py --check    # 只校验，不写入（差异即非零退出）

校验项
    1. 树中每个文件都有 copyfile 条目（除 .deleted-files；含 .gitignore 等点文件）
    2. 每条 copyfile 的 src 在仓内真实存在
    3. dest 全局唯一
"""

import argparse
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
XML = os.path.join(REPO, "contest2026_342_ezdezhandui.xml")
# 作品仓内目录 -> openvela 工作区目标前缀
GROUPS = [
    ("nuttx",          "nuttx"),
    ("board/esp32p4",  "vendor/espressif/boards/esp32p4"),
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
                out.append((sub + "/" + rel, dest_prefix + "/" + rel))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="只校验不写入")
    args = ap.parse_args()

    with open(XML, encoding="utf-8") as f:
        lines = f.read().split("\n")

    first = next(i for i, l in enumerate(lines) if COPYFILE_RE.match(l))
    last = max(i for i, l in enumerate(lines) if COPYFILE_RE.match(l))

    existing = []
    for l in lines[first:last + 1]:
        m = COPYFILE_RE.match(l)
        if m:
            existing.append((m.group(2), m.group(3)))

    # 树外条目（例如仓根文档）原样保留，且在清单末尾
    wanted_dests = {d for _s, d in tree_entries()}
    extras = [(s, d) for s, d in existing
              if not any(s == g or s.startswith(g + "/") for g in GROUP_DIRS)
              and not any(s.startswith(p) for p in LEGACY_PREFIXES)
              and d not in wanted_dests]

    wanted = tree_entries()
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

    if problems:
        print("\n❌ 校验失败:")
        for p in problems:
            print("   " + p)
        return 1

    if args.check:
        if added or removed:
            print("\n❌ 清单与文件树不一致（--check 模式不写入）")
            return 1
        print("\n✅ 清单与文件树一致")
        return 0

    out = lines[:first]
    out += ['    <copyfile src="%s" dest="%s"/>' % (s, d) for s, d in wanted]
    out += ['    <copyfile src="%s" dest="%s"/>' % (s, d) for s, d in extras]
    out += ['  </project>', '</manifest>', '']

    with open(XML, "w", encoding="utf-8") as f:
        f.write("\n".join(out))

    print("\n✅ 已写入 %s（%d 条 copyfile）" % (os.path.relpath(XML, REPO), len(wanted) + len(extras)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
