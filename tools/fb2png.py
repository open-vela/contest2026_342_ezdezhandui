#!/usr/bin/env python3
"""fb2png.py —— 把 JTAG dump 出来的帧缓冲（RAW RGB565）转成 PNG，并打印画面统计。

用途：板子没有截屏接口时，用 JTAG 读 /dev/fb0 的物理地址，把「屏幕上到底画了什么」
变成一张可查看的 PNG。配合 `lvgldemo` 可以验证 LVGL 渲染链路。

用法
    # 1) JTAG dump（0x48278d80 来自启动日志 "/dev/fb0 ready ... @ 0x48278d80"）
    openocd -f board/esp32p4-builtin.cfg \
      -c "init" -c "halt" -c "dump_image /tmp/fb.bin 0x48278d80 1228800" -c "resume" -c "shutdown"
    # 2) 转 PNG（默认 1024x600，小端 RGB565）
    python3 tools/fb2png.py /tmp/fb.bin /tmp/lvgl_demo.png [宽 高]

说明
    · JTAG 读速率实测约 4 KB/s，1.2MB 需要 ~5 分钟；halt 期间画面冻结，内容不受影响。
    · 输出末尾会打印「不同颜色数 / 主色占比 / 水平垂直梯度」——用它区分
      「渲染出的 UI」（色数千级、梯度 <8）与「花屏/噪声」（色数万级、梯度 60+）。
"""

import sys
from collections import Counter

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    sys.exit("需要 Pillow：pip install pillow")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)

    path, out = sys.argv[1], sys.argv[2]
    w = int(sys.argv[3]) if len(sys.argv) > 3 else 1024
    h = int(sys.argv[4]) if len(sys.argv) > 4 else 600

    raw = open(path, "rb").read()
    n = min(len(raw) // 2, w * h)
    rows = n // w
    px = bytearray(rows * w * 3)
    for i in range(rows * w):
        v = raw[2 * i] | (raw[2 * i + 1] << 8)          # RGB565 little-endian
        px[3 * i] = ((v >> 11) & 0x1F) << 3
        px[3 * i + 1] = ((v >> 5) & 0x3F) << 2
        px[3 * i + 2] = (v & 0x1F) << 3

    img = Image.frombytes("RGB", (w, rows), bytes(px))
    img.save(out)

    # 统计：色数 / 主色 / 梯度（判 UI vs 噪声）
    step = 2
    cols = Counter()
    lums = []
    for y in range(0, rows, step):
        row = []
        for x in range(0, w, step):
            r = px[(y * w + x) * 3]
            g = px[(y * w + x) * 3 + 1]
            b = px[(y * w + x) * 3 + 2]
            cols[(r, g, b)] += 1
            row.append((r * 299 + g * 587 + b * 114) // 1000)
        lums.append(row)

    total = sum(cols.values())
    gh = sum(abs(r[i + 1] - r[i]) for r in lums for i in range(len(r) - 1)) / max(1, total - 1)
    gv = sum(abs(lums[j + 1][i] - lums[j][i])
             for j in range(len(lums) - 1) for i in range(len(lums[0]))) / max(1, total - 1)

    print(f"{path}: {len(raw)} B → {out} ({w}x{rows})")
    print(f"  采样像素 {total}，不同颜色 {len(cols)}"
          f"（UI 千级；随机噪声 万级+）")
    print("  主色:", ", ".join(f"{c} {n/total*100:.1f}%" for c, n in cols.most_common(3)))
    print(f"  水平梯度 {gh:.2f} / 垂直梯度 {gv:.2f}（UI 通常 <8；噪声 ≈60+）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
