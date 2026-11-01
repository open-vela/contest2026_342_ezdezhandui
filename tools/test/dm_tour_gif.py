#!/usr/bin/env python3
"""dm_tour_gif.py —— 把 DeskMate 巡览快照拼成一帧一帧的 GIF

输入：目录里由 `deskmate --tour` 产出、并用 fb2png.py 转好的
`tour_*.png`（1024x600）。输出：`deskmate_tour.gif`（缩放后带停留时长）。

用法
    python3 dm_tour_gif.py <run_dir> [--width 640] [--out <path>]

顺序按文件名排序（tour_01…tour_09），停留时长：页面 900ms、相机连拍 250ms，
让"切页"看得出来、"画面在动"也看得出来。
"""

import argparse
import glob
import os
import sys

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    sys.exit("需要 Pillow：pip install pillow")


def hold_ms(name: str) -> int:
    """停留时长：相机连拍短、页面长。"""
    if "camera" in name:
        return 250
    if "chat" in name:
        return 1100
    return 950


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("run_dir")
    ap.add_argument("--width", type=int, default=640)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    paths = sorted(glob.glob(os.path.join(args.run_dir, "tour_*.png")))
    if not paths:
        print(f"{args.run_dir}: 没有 tour_*.png，跳过 GIF")
        return 1

    frames = []
    durations = []
    for p in paths:
        im = Image.open(p).convert("RGB")
        h = max(1, round(im.height * args.width / im.width))
        frames.append(im.resize((args.width, h), Image.LANCZOS))
        durations.append(hold_ms(os.path.basename(p)))

    out = args.out or os.path.join(args.run_dir, "deskmate_tour.gif")
    frames[0].save(out, save_all=True, append_images=frames[1:],
                   duration=durations, loop=0, optimize=True)
    print(f"GIF: {out} ({os.path.getsize(out)} B, {len(frames)} 帧)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
