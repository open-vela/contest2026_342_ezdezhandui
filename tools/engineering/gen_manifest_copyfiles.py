#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""重新生成作品仓 manifest 的 <copyfile> 清单。

背景
    contest2026_342_ezdezhandui.xml 通过 <copyfile> 把本仓文件树映射回 openvela 工作区位置：

        nuttx/<rel>           -> nuttx/<rel>
        board/esp32p4/<rel>   -> vendor/espressif/boards/esp32p4/<rel>
        app/apps/<rel>        -> apps/<rel>
        app/ai_agent/<rel>    -> packages/ai_agent/<rel>

    文件树由 tools/engineering/export.py 维护，本脚本保证清单与文件树**严格一一对应**：
    新增文件自动补条目、已删除文件自动移除条目、原清单中的"树外条目"（例如
    CSI_INTEGRATOR_HANDOFF.md）原样保留。

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
    #    （已用 repo 源码实测：目录软链与文件软链都报错，见 docs/06 §24.3）
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
