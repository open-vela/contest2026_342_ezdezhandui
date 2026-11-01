#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""检查 docs/ 下所有 markdown 的相对链接与图片是否可解析（只读）。

退出码：0 = 全部可解析；1 = 有失效链接。
"""

from __future__ import annotations

import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DOCS = os.path.join(REPO, "docs")

LINK = re.compile(r"!?\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
SKIP_PREFIX = ("http://", "https://", "mailto:", "#", "ftp:")


def main():
    broken, checked, files = [], 0, 0
    for root, _, names in os.walk(DOCS):
        for n in names:
            if not n.endswith(".md"):
                continue
            files += 1
            path = os.path.join(root, n)
            text = open(path, encoding="utf-8", errors="replace").read()
            for m in LINK.finditer(text):
                target = m.group(1).strip()
                if not target or target.startswith(SKIP_PREFIX):
                    continue
                target = target.split("#", 1)[0]
                if not target:
                    continue
                checked += 1
                full = os.path.normpath(os.path.join(root, target))
                if not os.path.exists(full):
                    broken.append((os.path.relpath(path, REPO), target))

    print(f"== docs 链接检查 ==")
    print(f"  文件 {files} 个, 相对链接/图片 {checked} 处, 失效 {len(broken)} 处")
    for f, t in broken[:40]:
        print(f"  ❌ {f} -> {t}")
    if len(broken) > 40:
        print(f"  ... 另有 {len(broken) - 40} 处")
    if not broken:
        print("  ✅ 全部可解析")
    return 0 if not broken else 1


if __name__ == "__main__":
    sys.exit(main())
