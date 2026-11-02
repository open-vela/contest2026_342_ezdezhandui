# 应用层（DeskMate）真机快照

ESP32-P4 真机屏幕上实际画出来的画面，由 **JTAG 读帧缓冲**得到（不是模拟器渲染）：

| 文件 | 内容 |
|---|---|
| `hw_home_1024x600.png` | 真机 `/dev/fb0`（0x48000040）dump → PNG：中文首页 |
| `openocd_dump.log` | OpenOCD `dump_image` 日志（实测 5.97 KiB/s，1.2 MB 约 200 秒） |
| `console_selftest.log` | 真机 `deskmate --agent mock --selftest` 串口输出 |

判据（与模拟器同一套）：色数 138、主色 51.9%、平均亮度梯度 **1.20**（UI 判据 <8，噪声约 60+）。

复现：

```bash
tools/build_flash/usb_stable.sh                                  # 烧录
python3 tools/test/board.py run "deskmate --agent mock --exit-after 60"
tools/test/hw_deskmate_evidence.sh --addr 0x48000040             # JTAG 取帧 → PNG
```
