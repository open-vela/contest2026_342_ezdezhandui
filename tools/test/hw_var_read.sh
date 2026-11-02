#!/usr/bin/env bash
# 串口被驱动日志占满时，用 JTAG 直接读应用全局变量（符号地址从 ELF 取）。
#
#   tools/test/hw_var_read.sh g_scan_state g_scan_count g_scan_err
#
# 输出每个符号的地址与 32 位值。需要板子已烧录对应 ELF 的镜像。
set -euo pipefail
HERE=$(cd "$(dirname "$0")/../.." && pwd)          # 交付仓根
WORKSPACE="${DM_WORKSPACE:-$(cd "${HERE}/.." && pwd)}"   # openvela 工作区（构建产物在这里）
ELF="${DM_ELF:-${WORKSPACE}/cmake_out/esp32p4-function-ev-board_nsh/nuttx}"
NM="${WORKSPACE}/prebuilts/gcc/linux-x86_64/riscv-none-elf/bin/riscv-none-elf-nm"
OPENOCD=$(ls -d "${HOME}"/.espressif/tools/openocd-esp32/*/openocd-esp32/bin/openocd 2>/dev/null | head -1)
OCD=$(ls -d "${HOME}"/.espressif/tools/openocd-esp32/*/openocd-esp32/share/openocd/scripts 2>/dev/null | head -1)

[ -n "${OPENOCD}" ] && [ -n "${OCD}" ] || { echo "找不到 OpenOCD" >&2; exit 1; }
[ -f "${ELF}" ] || { echo "找不到 ELF: ${ELF}（用 DM_ELF=<path> 指定）" >&2; exit 1; }

CMDS=()
for sym in "$@"; do
  addr=$("${NM}" "${ELF}" | awk -v s="${sym}" '$3==s {print $1; exit}')
  [ -n "${addr}" ] || { echo "${sym}: 符号未找到" >&2; continue; }
  echo "${sym} @ 0x${addr}"
  CMDS+=(-c "mdw 0x${addr} 1")
done

timeout 120 "${OPENOCD}" -s "${OCD}" -f "${HERE}/board/esp32p4-builtin.cfg" \
  -c "init" -c "halt" "${CMDS[@]}" -c "resume" -c "shutdown" 2>&1 \
  | grep -aE "^0x[0-9a-f]+:"
