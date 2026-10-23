#!/usr/bin/env bash
# =============================================================================
# camera_verify.sh — 摄像头驱动一键复现：编译 → 烧录 → 枚举 → 抓帧 → 判定
#
# 目标板：ESP32-P4X-C5-Function-EV-Board（openvela / NuttX）
# 传感器：SC2336，SCCB 0x30，2-lane MIPI CSI-2，RAW8 1280x720 @30fps（2 lane / 336 Mbps）
#
# 用法：
#   tools/test/camera_verify.sh                 # 编译 + 烧录 + 复位 + 全部验证
#   tools/test/camera_verify.sh --no-flash      # 只编译 + 复位验证（板内已是新固件时）
#   tools/test/camera_verify.sh --build-only    # 只编译并检查告警
#   tools/test/camera_verify.sh --frames 3      # 抓 3 帧（默认 2）
#   tools/test/camera_verify.sh --out /tmp/cam  # 指定日志目录
#
# 前置：
#   /dev/ttyUSB0  console（CP2102, 115200，须 assert DTR/RTS；board.py 已处理）
#   /dev/ttyACM0  烧录（esptool）
#   ~/.local/bin/esptool.py 或 PATH 里的 esptool.py
#
# 判定（全部满足才算 PASS）：
#   1. 编译 exit 0；摄像头相关源文件（sc2336.c / v4l2ctl.c / camera_main.c）0 warning
#   2. 启动日志：SC2336 chip ID: 0xcb3a；Camera registered on /dev/video0
#   3. v4l2ctl list-devices：FOUND /dev/video0
#   4. v4l2ctl --info：DRIVER / CAPS 正常
#   5. v4l2ctl --list-formats-ext：FORMAT、SIZE 1280x720、INTERVAL 1/30
#   6. v4l2ctl --capture N：RESULT PASS，且至少一帧 nonzero > 0
# =============================================================================
set -uo pipefail

WS=${WS:-/home/ez/share/openvela}
REPO=${REPO:-$WS/contest2026_342_ezdezhandui}
CFG=vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/configs/nsh
CMAKE_DIR="$WS/cmake_out/esp32p4-function-ev-board_nsh"
BOARD="$REPO/tools/test/board.py"
FLASH="$REPO/tools/build_flash/usb_stable.sh"

DO_BUILD=1
DO_FLASH=1
DO_RESET=1
FRAMES=2
WAIT=20
OUT=${OUT:-/tmp/camera_verify.$(date +%H%M%S)}

while [ $# -gt 0 ]; do
  case "$1" in
    --no-build)    DO_BUILD=0 ;;
    --no-flash)    DO_FLASH=0 ;;
    --no-reset)    DO_RESET=0 ;;
    --build-only)  DO_FLASH=0; DO_RESET=0 ;;
    --frames)      FRAMES=${2:-2}; shift ;;
    --timeout)     WAIT=${2:-20}; shift ;;
    --out)         OUT=${2:-$OUT}; shift ;;
    -h|--help)     sed -n '2,30p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

mkdir -p "$OUT"
PASS=0
FAIL=0

ok()   { PASS=$((PASS + 1)); printf '  \033[32mPASS\033[0m %s\n' "$*"; }
bad()  { FAIL=$((FAIL + 1)); printf '  \033[31mFAIL\033[0m %s\n' "$*"; }
info() { printf '── %s\n' "$*"; }

expect_grep() { # <file> <ERE> <label>
  if grep -aqE "$2" "$1"; then ok "$3"; else bad "$3 (未匹配 /$2/ in $1)"; fi
}

# ── 0. 环境检查 ─────────────────────────────────────────────────────────────
info "环境"
[ -f "$BOARD" ] || { echo "缺少 $BOARD" >&2; exit 2; }
[ -d "$WS" ]    || { echo "缺少工作区 $WS" >&2; exit 2; }
if [ "$DO_FLASH" = 1 ] && [ ! -x "$FLASH" ]; then
  echo "缺少可执行烧录脚本 $FLASH" >&2; exit 2
fi
if [ ! -c /dev/ttyUSB0 ]; then
  echo "WARN: /dev/ttyUSB0 不存在，枚举/抓帧步骤将失败" >&2
fi
echo "  工作区 : $WS"
echo "  日志   : $OUT"
echo "  抓帧   : $FRAMES 帧，单次等待 ${WAIT}s"

# ── 1. 编译 ────────────────────────────────────────────────────────────────
if [ "$DO_BUILD" = 1 ]; then
  info "1/5 编译"

  # 增量工作区可能沿用旧 .config：defconfig 新增的 v4l2ctl 不会自动生效。
  # 检测到缺项就按 defconfig 重置一次配置，避免"编译通过但固件里没有 v4l2ctl"。
  if [ -f "$CMAKE_DIR/.config" ] && \
     ! grep -q '^CONFIG_SYSTEM_V4L2CTL=y' "$CMAKE_DIR/.config"; then
    echo "  .config 缺少 CONFIG_SYSTEM_V4L2CTL，按 defconfig 重置配置"
    cmake --build "$CMAKE_DIR" --target resetconfig >"$OUT/resetconfig.log" 2>&1 \
      || { bad "resetconfig 失败"; tail -5 "$OUT/resetconfig.log"; }
  fi

  ( cd "$WS" && ./build.sh "$CFG" --cmake -j8 ) >"$OUT/build.log" 2>&1
  rc=$?
  if [ $rc -eq 0 ]; then ok "build.sh exit 0"; else bad "build.sh exit $rc"; fi

  if grep -aqE '(^|: )error:' "$OUT/build.log"; then
    bad "编译日志出现 error:"
    grep -aE '(^|: )error:' "$OUT/build.log" | head -5
  else
    ok "编译日志无 error:"
  fi

  cam_warn=$(grep -aE 'warning:' "$OUT/build.log" \
             | grep -aE 'sc2336\.c|v4l2ctl\.c|camera_main\.c' | wc -l)
  if [ "$cam_warn" -eq 0 ]; then
    ok "摄像头相关源文件 0 warning"
  else
    bad "摄像头相关源文件 warning 数 = $cam_warn"
    grep -aE 'warning:' "$OUT/build.log" \
      | grep -aE 'sc2336\.c|v4l2ctl\.c|camera_main\.c' | head -5
  fi

  if [ -f "$CMAKE_DIR/nuttx.bin" ]; then
    ok "产物 nuttx.bin = $(stat -c%s "$CMAKE_DIR/nuttx.bin") B"
  else
    bad "产物 nuttx.bin 不存在"
  fi

  if [ "$DO_FLASH" = 0 ] && [ "$DO_RESET" = 0 ]; then
    echo
    echo "结果：PASS=$PASS FAIL=$FAIL（--build-only）"
    [ "$FAIL" -eq 0 ] && exit 0 || exit 1
  fi
fi

# ── 2. 烧录 ────────────────────────────────────────────────────────────────
if [ "$DO_FLASH" = 1 ]; then
  info "2/5 烧录（MCUboot 两镜像）"
  "$FLASH" >"$OUT/flash.log" 2>&1
  rc=$?
  if [ $rc -eq 0 ] && grep -aq '烧录完成' "$OUT/flash.log"; then
    ok "两镜像烧录完成"
  else
    bad "烧录失败（rc=$rc），末尾："
    tail -6 "$OUT/flash.log"
  fi
else
  info "2/5 跳过烧录（--no-flash）"
fi

# ── 3. 复位 + 启动日志 ─────────────────────────────────────────────────────
if [ "$DO_RESET" = 1 ]; then
  info "3/5 复位并抓启动日志"
  python3 "$BOARD" reset --timeout 25 --save "$OUT/boot.log" >/dev/null 2>&1
  rc=$?
  [ $rc -eq 0 ] && ok "复位/启动捕获完成" || bad "复位捕获失败 rc=$rc"

  expect_grep "$OUT/boot.log" 'SC2336 chip ID: 0xcb3a' \
              "传感器 ID 0xcb3a"
  expect_grep "$OUT/boot.log" 'Camera registered on /dev/video0' \
              "/dev/video0 注册"
else
  info "3/5 跳过复位（--no-reset）"
fi

# ── 4. 枚举 ────────────────────────────────────────────────────────────────
info "4/5 枚举（NuttX 无 v4l2-ctl，用 v4l2ctl）"

python3 "$BOARD" run "v4l2ctl list-devices" --timeout 15 \
        --save "$OUT/list.log" >/dev/null 2>&1
expect_grep "$OUT/list.log" 'FOUND /dev/video0' "list-devices 找到 /dev/video0"

python3 "$BOARD" run "v4l2ctl -d /dev/video0 --info" --timeout 15 \
        --save "$OUT/info.log" >/dev/null 2>&1
expect_grep "$OUT/info.log" 'V4L2CTL: DRIVER' "QUERYCAP driver"
expect_grep "$OUT/info.log" 'V4L2CTL: CAPS'   "QUERYCAP capabilities"

python3 "$BOARD" run "v4l2ctl -d /dev/video0 --list-formats-ext" \
        --timeout 15 --save "$OUT/formats.log" >/dev/null 2>&1
expect_grep "$OUT/formats.log" 'V4L2CTL: FORMAT'          "ENUM_FMT"
expect_grep "$OUT/formats.log" 'SIZE 1280x720'           "ENUM_FRAMESIZES 1280x720"
expect_grep "$OUT/formats.log" 'INTERVAL 1/30'            "ENUM_FRAMEINTERVALS 1/30"

# ── 5. 抓帧 ────────────────────────────────────────────────────────────────
info "5/5 抓帧（v4l2ctl --capture $FRAMES）"
python3 "$BOARD" run "v4l2ctl -d /dev/video0 --capture $FRAMES --timeout $WAIT" \
        --timeout $((WAIT + 15)) --no-idle --save "$OUT/capture.log" >/dev/null 2>&1

expect_grep "$OUT/capture.log" 'V4L2CTL: STREAMON ok' "STREAMON"
expect_grep "$OUT/capture.log" 'V4L2CTL: FRAME 0'     "收到第 1 帧"
expect_grep "$OUT/capture.log" "V4L2CTL: RESULT PASS frames=$FRAMES" \
            "抓帧 $FRAMES/$FRAMES"

# 帧内容非全零（全零 = 没有真正写入内存）
if grep -aqE 'V4L2CTL: FRAME 0 .*nonzero=[1-9][0-9]*' "$OUT/capture.log"; then
  ok "第一帧 nonzero > 0（缓冲区有真实数据）"
else
  bad "第一帧 nonzero = 0（缓冲区未被写入）"
fi

# ── 汇总 ───────────────────────────────────────────────────────────────────
echo
echo "============================================================"
if [ "$FAIL" -eq 0 ]; then
  echo "RESULT: PASS（$PASS 项断言全过）"
else
  echo "RESULT: FAIL（PASS=$PASS FAIL=$FAIL）"
  echo
  echo "排查入口："
  echo "  boot    : $OUT/boot.log"
  echo "  list    : $OUT/list.log"
  echo "  info    : $OUT/info.log"
  echo "  formats : $OUT/formats.log"
  echo "  capture : $OUT/capture.log"
  echo "  抓不到帧时先看 capture.log 里的 S_FMT/STREAMON/FRAME 行；"
  echo "  需要寄存器级判据时打开 ESP_CSI_CAPTURE_DIAG 后跑 tools/test/camera_diag.sh。"
fi
echo "============================================================"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
