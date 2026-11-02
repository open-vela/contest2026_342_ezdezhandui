# 应用层（DeskMate）模拟器快照

由 `tools/test/sim_deskmate.sh` 产出：自检一次运行（`RESULT: PASS`，`DM-SELFTEST RESULT: PASS (18/18)`）
给出三张页面截图，`--tour` 再一次给出五页巡览（9 帧 + GIF）。

| 文件 | 内容 |
|---|---|
| `dm_home.png` / `dm_net.png` / `dm_camera.png` | 自检时的三个页面（1024×600，RAW RGB565 → PNG） |
| `dm_*.bin.txt` | 快照几何（width/height/bpp/stride/backend） |
| `tour/tour_01..09_*.png` | 巡览：首页 → 相机 ×5 → 网络 → 对话 → 设置 |
| `tour/deskmate_tour.gif` | 上面 9 帧拼成的动画（页面停留 0.9–1.1s，相机连拍 0.25s） |

> 2026-11-02 起界面为**中文**（自绘 CJK 子集字体 dm_font_16/24）；本目录的 PNG/GIF 都是中文版。
| `console.log` / `console_tour.log` | guest 控制台（含逐条 `[dm-check] … PASS`） |
| `fb2png.log` | 色数 / 主色占比 / 梯度统计（与真机同一判据：UI 梯度 <8） |
| `result.txt` | 该次运行判定摘要 |

这些快照由应用自己的 `dm_display_snapshot()` 写出：模拟器用内存显示后端，真机用 `/dev/fb0`
（配 JTAG dump）+ 同一个 `tools/test/fb2png.py`——**同一条证据链**。
