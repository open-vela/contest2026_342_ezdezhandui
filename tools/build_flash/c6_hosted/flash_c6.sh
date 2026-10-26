#!/bin/bash
# flash_c6.sh - 把 ESP-Hosted SDIO 从机固件烧到板载 ESP32-C6
#
# 为什么需要这一步：板载 C6 出厂镜像走的是**串口**传输的 esp-hosted
# （读它的 UART 能看到 `serial : in: 0 rsp: 0 evt: 0`），而 ESP32-P4 与它之间
# 只有 SD2_* 这组 SDIO 连线 —— 所以 P4 做 SDIO 枚举时永远没有设备应答。
# 只有把 C6 换成 SDIO 传输的从机固件，P4 才可能枚举到它。
#
# ⚠️ 前置：这颗 CP2102 的 DTR/RTS 没有接到 C6 的 EN/BOOT，esptool 无法自动
#    进入下载模式（实测 default_reset / no_reset / 手动驱动 DTR·RTS 三种极性
#    均返回 "No serial data received"）。必须人工：
#        按住板上 BOOT（GPIO9）→ 按一下 RESET（或断电重上电）→ 松开 BOOT
#
# 用法:
#   tools/build_flash/c6_hosted/flash_c6.sh [串口]
#
# 固件来源: espressif/esp-hosted-mcu examples/wifi/sta/cp,
#           idf.py set-target esp32c6, CONFIG_EH_TRANSPORT_CP_SDIO=y

set -u

PORT="${1:-/dev/ttyUSB1}"
DIR="$(cd "$(dirname "$0")" && pwd)"
FW="$DIR/eh_cp_wifi_sta_esp32c6_sdio.bin"
ESPTOOL=${ESPTOOL:-$HOME/.local/bin/esptool.py}

if [ ! -f "$FW" ]; then
  echo "找不到固件: $FW" >&2; exit 1
fi

echo "串口 : $PORT"
echo "固件 : $FW ($(stat -c%s "$FW") B)"
echo
echo "如果接下来报 'No serial data received'，说明 C6 不在下载模式："
echo "  按住 BOOT → 按一下 RESET → 松开 BOOT，然后立刻重跑本脚本。"
echo

# 单镜像烧到 0x0（该工程的分区表已包含 bootloader/app 偏移）
"$ESPTOOL" --port "$PORT" --baud 460800 \
  --before no_reset --after hard_reset \
  write_flash 0x0 "$FW"
