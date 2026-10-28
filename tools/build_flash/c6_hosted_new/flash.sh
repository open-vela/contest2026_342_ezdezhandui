#!/bin/bash
# 烧录"自建"的 C6 固件（esp-hosted v3.0.10）。
#
# ⚠️ 前置：这颗 CP2102 的 DTR/RTS 没有接到 C6 的 EN/BOOT，esptool 无法自动
#    进入下载模式，也没法从 P4 侧驱动 —— 原理图里 C6_EN 接 GPIO54（P4 可控），
#    但 C6 的 BOOT（IO9）没有连到任何 P4 GPIO。
#    所以必须人工：按住板上 BOOT（GPIO9）→ 按一下 RESET → 松开 BOOT，
#    然后立刻运行本脚本。
#
# 偏移按 flasher_args 的方式：app 在 0x10000，不是 0x0。
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
PORT="${1:-/dev/ttyUSB1}"
ESPTOOL="${ESPTOOL:-$HOME/.local/bin/esptool.py}"

for f in bootloader.bin partition-table.bin ota_data_initial.bin \
         eh_cp_network_split_station.bin; do
  [ -f "$DIR/$f" ] || { echo "缺少 $f" >&2; exit 1; }
done

echo "若下面的连接失败：按住 BOOT → 按 RESET → 松开 BOOT，再重跑本脚本。"
"$ESPTOOL" --chip esp32c6 -p "$PORT" -b 460800 \
  --before no_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 4MB \
    0x0     "$DIR/bootloader.bin" \
    0x8000  "$DIR/partition-table.bin" \
    0xd000  "$DIR/ota_data_initial.bin" \
    0x10000 "$DIR/eh_cp_network_split_station.bin"
