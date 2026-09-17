#!/usr/bin/env bash
# =============================================================================
# camera_diag.sh — 摄像头链路一次性验证（传感器 → MIPI-CSI → 桥 → DW-GDMA）
#
# 目的
#   把「摄像头驱动到底正不正常」变成一条可复现的命令：
#     · 传感器控制面：I2C 探到 chip ID、寄存器表写下去、stream-on 读回 0x01
#     · CSI 主机/桥/DW-GDMA：寄存器是否按 2 lane / RAW10 / 帧长配置并使能
#     · 数据面：桥 FIFO 有没有收到字节、DMA 的 dar 有没有在动、有没有错误事件
#   一次运行即可判定故障在「软件链路」还是「MIPI 物理链路/模组」。
#
# 前置（一次性）
#   寄存器级探针默认关闭，需打开后重编：
#     sed -i 's/#define ESP_CSI_CAPTURE_DIAG 0/#define ESP_CSI_CAPTURE_DIAG 1/' \
#         nuttx/arch/risc-v/src/common/espressif/esp_csi.c
#     ./build.sh esp32p4-function-ev-board:nsh --cmake -j8
#     (验证完记得改回 0 并重编 —— 探针会在每次 start_capture 多花约 1.2s 并刷屏)
#
# 用法
#   tools/camera_diag.sh            # 复位 → 跑 camera 应用 → 抓 CAMDIAG → 给判读
#   tools/camera_diag.sh 60         # 自定义采集秒数
#
# 判读（脚本最后会打印同样的表）
#   RESULT 行出现 "MOVING"                     → 帧在进内存，驱动链路 OK
#   RESULT 行 "STATIC" + buf_depth=0 + 无错误  → 传感器在流、链路配置正确，
#                                                数据没跨过 MIPI 物理链路（模组/排线/焊接）
#   host st_main / brg int_raw 非 0            → 接收侧有错误事件，先查 PHY/lane 配置
#   0x0100 读回不是 0x01                        → 传感器没进流，是传感器控制面问题
# =============================================================================
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
SECS="${1:-35}"
LOG="/tmp/camera_diag_$$.log"

echo "── 摄像头链路验证（采集 ${SECS}s）──────────────────────────"
echo "   console: /dev/ttyUSB0 (115200)   repo: $REPO"

cd "$REPO"
python3 tools/board.py reset --timeout 22 > /tmp/camera_diag_boot.log 2>&1 || true

BOOT_HITS=$(grep -acE "SC2336 chip ID|video0" /tmp/camera_diag_boot.log || true)
echo "   启动：SC2336/video0 相关行 = ${BOOT_HITS}"

python3 tools/board.py run "camera" --timeout "$SECS" --no-idle > "$LOG" 2>&1 || true

echo
echo "── 传感器控制面 ────────────────────────────────────────────"
grep -aE "SC2336 chip ID|Camera registered" /tmp/camera_diag_boot.log | sed 's/^/   /'
grep -aE "SC2336: start_capture ->|ERROR: SC2336" "$LOG" | sed 's/^/   /'

echo
echo "── CSI 主机 / 桥 / DW-GDMA（t0 快照）──────────────────────"
grep -aE "CAMDIAG\[t0\] (host|brg|dma)" "$LOG" | sed 's/^/   /'

echo
echo "── 数据面判定 ──────────────────────────────────────────────"
grep -aE "CAMDIAG\[RESULT\]" "$LOG" | sed 's/^/   /'

echo
echo "── 判读 ────────────────────────────────────────────────────"
if grep -aq "MOVING - FRAMES ARRIVING" "$LOG"; then
  echo "   ✅ 帧在进内存：摄像头驱动链路端到端 OK"
elif grep -aq "STATIC - no frames" "$LOG"; then
  if grep -aq "buf_depth=0" "$LOG" && grep -aq "int_raw=00000000" "$LOG"; then
    echo "   ⚠️  传感器已在流、CSI 主机/桥/DMA 配置正确且零错误，但桥 FIFO 一个字节都没收到"
    echo "      → 数据没有跨过 MIPI D-PHY 物理链路：查排线/连接器/模组（换模组复测）"
  else
    echo "   ⚠️  无帧，且接收侧有错误/缓冲非空信号 → 先查 PHY/lane 配置与电源"
  fi
else
  echo "   ❓ 未采集到 RESULT 行：确认探针已打开并重编（见脚本头部说明），或延长采集秒数"
fi
echo
echo "   原始日志：$LOG"
