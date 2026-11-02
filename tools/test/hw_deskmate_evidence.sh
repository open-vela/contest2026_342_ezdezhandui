#!/usr/bin/env bash
# tools/test/hw_deskmate_evidence.sh —— 真机 UI 取证：帧缓冲 → PNG
#
# 板子上跑过一次 deskmate（界面已经画在 /dev/fb0 上）之后，用这个脚本把屏幕
# "拍"下来：串口日志里拿到帧缓冲物理地址 → OpenOCD dump_image → fb2png.py 转 PNG，
# 并打印色数/主色/梯度判据（与模拟器快照同一套判据）。
#
#   tools/test/hw_deskmate_evidence.sh                 # 用启动日志里最后一次的 fb 地址
#   tools/test/hw_deskmate_evidence.sh --addr 0x48000040
#   tools/test/hw_deskmate_evidence.sh --out <目录>
#
# 前提：
#   · console /dev/ttyUSB0（CP2102，115200，需 assert DTR/RTS —— board.py 已处理）
#   · OpenOCD（ESP-IDF 自带：~/.espressif/tools/openocd-esp32/*/openocd-esp32/bin/openocd）
#   · 板子处于运行状态（不是停在 ROM 下载模式）
#
# 产物：<out>/hw_fb.png、<out>/hw_fb.bin（原始 RGB565）、<out>/hw_fb.log（判据）

set -u -o pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${HERE}/../.." && pwd)"
WS="$(cd "${REPO}/.." && pwd)"

STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="${TMPDIR:-/tmp}/deskmate-hw/${STAMP}"
ADDR=""
FBSIZE=1228800          # 1024*600*2
WIDTH=1024
HEIGHT=600

while [ $# -gt 0 ]; do
  case "$1" in
    --addr) shift; ADDR="${1:-}" ;;
    --out)  shift; OUT="${1:-${OUT}}" ;;
    -h|--help) sed -n '2,20p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

log() { printf '\033[1;36m[hw-evidence]\033[0m %s\n' "$*"; }
mkdir -p "${OUT}"

OPENOCD=$(ls -d "${HOME}"/.espressif/tools/openocd-esp32/*/openocd-esp32/bin/openocd 2>/dev/null | head -1)
OCD_SCRIPTS=$(ls -d "${HOME}"/.espressif/tools/openocd-esp32/*/openocd-esp32/share/openocd/scripts 2>/dev/null | head -1)

if [ -z "${OPENOCD}" ] || [ -z "${OCD_SCRIPTS}" ]; then
  echo "找不到 OpenOCD（~/.espressif/tools/openocd-esp32）" >&2
  exit 2
fi

# 1) 拿帧缓冲地址：优先用 --addr，否则从串口日志里找 ========================
if [ -z "${ADDR}" ]; then
  log "no --addr given, searching the console log for the framebuffer address"
  BOOTLOG="${OUT}/boot.log"
  timeout 90 python3 "${HERE}/board.py" watch --timeout 20 > "${BOOTLOG}" 2>&1 || true
  ADDR=$(grep -aoE 'fb0 ready [0-9]+x[0-9]+ RGB565 @ 0x[0-9a-f]+' "${BOOTLOG}" | tail -1 | grep -oE '0x[0-9a-f]+$' || true)
fi

if [ -z "${ADDR}" ]; then
  echo "拿不到帧缓冲地址：请在启动日志里确认 'fb0 ready ... @ 0x...'，或用 --addr 指定" >&2
  exit 3
fi

log "framebuffer @ ${ADDR} (${WIDTH}x${HEIGHT} RGB565, ${FBSIZE} B)"
log "archiving to ${OUT}"

# 2) OpenOCD dump ============================================================
if ! timeout 300 "${OPENOCD}" -s "${OCD_SCRIPTS}" -f board/esp32p4-builtin.cfg \
      -c "init" -c "halt" \
      -c "dump_image ${OUT}/hw_fb.bin ${ADDR} ${FBSIZE}" \
      -c "resume" -c "shutdown" > "${OUT}/openocd.log" 2>&1; then
  echo "OpenOCD dump 失败，见 ${OUT}/openocd.log" >&2
  tail -5 "${OUT}/openocd.log" >&2
  exit 4
fi

if [ ! -s "${OUT}/hw_fb.bin" ]; then
  echo "dump 出来的镜像为空" >&2
  exit 5
fi

# 3) 转 PNG + 判据 ===========================================================
if python3 "${HERE}/fb2png.py" "${OUT}/hw_fb.bin" "${OUT}/hw_fb.png" "${WIDTH}" "${HEIGHT}" \
     > "${OUT}/hw_fb.log" 2>&1; then
  cat "${OUT}/hw_fb.log"
  log "PNG: ${OUT}/hw_fb.png"
else
  echo "fb2png 失败，见 ${OUT}/hw_fb.log" >&2
  exit 6
fi
