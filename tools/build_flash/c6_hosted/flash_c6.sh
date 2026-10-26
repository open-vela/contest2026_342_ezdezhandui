#!/bin/bash
# flash_c6.sh — 把 ESP-Hosted SDIO 从机固件烧到板载 ESP32-C6
#
# 为什么需要：板载 C6 出厂镜像走的是**串口**传输的 esp-hosted，而 P4 与它之间
# 只有 SDIO 连线（P4 侧 GPIO14-19 = SD2_D0-D3/CLK/CMD，已由原理图确认）。
# 只有把 C6 换成 SDIO 传输的从机固件，P4 才可能枚举到它。
#
# ⚠️ 偏移很重要：ESP-IDF 生成的**不是**单镜像。真实布局见构建目录的
#    flasher_args.json —— app 在 0x10000，不是 0x0。
#    （本脚本早期版本把 app 单镜像写到 0x0，那是错的。）
#
# ⚠️ 前置：这颗 CP2102 的 DTR/RTS 没有接到 C6 的 EN/BOOT，esptool 无法自动
#    进入下载模式（实测 default_reset / no_reset / 手动驱动 DTR·RTS 三种极性
#    均返回 "No serial data received"）。必须人工：
#        按住板上 BOOT（GPIO9）→ 按一下 RESET（或断电重上电）→ 松开 BOOT
#
# 用法:
#   tools/build_flash/c6_hosted/flash_c6.sh [串口] [--debug]
#     --debug  烧带 DEBUG 日志的版本（用来观察从机的 SDIO 行为）

set -u

PORT="/dev/ttyUSB1"
DEBUG=0
for a in "$@"; do
  case "$a" in
    --debug) DEBUG=1 ;;
    *) PORT="$a" ;;
  esac
done

DIR="$(cd "$(dirname "$0")" && pwd)"
FW="$DIR/eh_cp_wifi_sta_esp32c6_sdio.bin"
[ "$DEBUG" = 1 ] && FW="$DIR/eh_cp_wifi_sta_debug.bin"

ESPTOOL=${ESPTOOL:-$HOME/.local/bin/esptool.py}
BOOTLOADER="$DIR/bootloader.bin"
PARTTABLE="$DIR/partition-table.bin"
OTADATA="$DIR/ota_data_initial.bin"

for f in "$FW" "$BOOTLOADER" "$PARTTABLE" "$OTADATA"; do
  [ -f "$f" ] || { echo "缺少镜像: $f" >&2; exit 1; }
done

echo "串口   : $PORT"
echo "app    : $(basename "$FW") ($(stat -c%s "$FW") B)$([ "$DEBUG" = 1 ] && echo '  [DEBUG 日志版]')"
echo
echo "若报 'No serial data received'，说明 C6 不在下载模式："
echo "  按住 BOOT → 按一下 RESET → 松开 BOOT，然后立刻重跑本脚本。"
echo

"$ESPTOOL" --chip esp32c6 --port "$PORT" --baud 460800 \
  --before no_reset --after hard_reset \
  write_flash \
    0x0     "$BOOTLOADER" \
    0x8000  "$PARTTABLE" \
    0xd000  "$OTADATA" \
    0x10000 "$FW"
