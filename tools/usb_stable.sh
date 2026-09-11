#!/bin/bash
# usb_stable.sh - ESP32-P4X 稳定烧录/复位工具（MCUboot 两镜像流程）
#
# ⚠️ 2026-09-11 起烧录流程变更：板子已切到 **MCUboot 二级引导 + flash XIP**
#    必须烧两个镜像，旧的"单镜像写 0x2000"方式已失效：
#      0x02000  MCUboot 引导      nuttx/mcuboot-esp32p4.bin   (24,640 B)
#      0x20000  OTA_0 应用主槽    <build>/nuttx.bin
#
# 端口拓扑（实测）：
#   /dev/ttyACM0  Espressif USB-Serial/JTAG -> 烧录（esptool）+ ROM/MCUboot 日志
#   /dev/ttyUSB0  CP2102 UART 桥            -> **NuttX console (UART0)**
#                                              读串口必须 assert DTR/RTS，否则 0 字节
#
# 用法:
#   tools/usb_stable.sh                        # 默认路径烧录两镜像
#   tools/usb_stable.sh <app.bin>              # 指定应用镜像（引导用默认路径）
#   tools/usb_stable.sh <app.bin> <boot.bin>
#   tools/usb_stable.sh "" "" {chip_id|reset}
#
# 环境变量:
#   OPENVELA_ROOT  工作区根（默认从脚本位置推导，可显式覆盖）

set -u

FLASH_PORT=/dev/ttyACM0
CONSOLE_PORT=/dev/ttyUSB0
ESPTOOL=${ESPTOOL:-$HOME/.local/bin/esptool.py}

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT=${OPENVELA_ROOT:-$(cd "$SCRIPT_DIR/../.." && pwd)}

BUILD_DIR="$ROOT/cmake_out/esp32p4-function-ev-board_nsh"
DEFAULT_FW="$BUILD_DIR/nuttx.bin"
DEFAULT_BOOT="$ROOT/nuttx/mcuboot-esp32p4.bin"

BOOT_OFFSET=0x2000
APP_OFFSET=0x20000

FW="${1:-$DEFAULT_FW}"
BOOT="${2:-$DEFAULT_BOOT}"
ACTION="${3:-flash}"

# ── 等待串口稳定（复位后设备重新枚举）─────────────────────────────
wait_stable() {
    for i in $(seq 1 15); do
        if [ -c "$CONSOLE_PORT" ]; then
            if timeout 5 python3 -c "
import serial
try:
    s = serial.Serial('$CONSOLE_PORT', 115200, timeout=0.2)
    s.dtr = True; s.rts = True
    s.close()
    print('ready')
except Exception:
    pass" 2>/dev/null | grep -q ready; then
                sleep 3
                return 0
            fi
        fi
        sleep 1
    done
    echo "ERROR: $CONSOLE_PORT 未就绪" >&2
    return 1
}

# ── esptool 带重试 ────────────────────────────────────────────────
# 注意：不要写成 `esptool | tee log | grep -q ...`。
# grep -q 一旦匹配到"第一个镜像"的 "Hash of data verified" 就会立即退出，
# tee 收到 SIGPIPE 后终止，进而把正在写第二个镜像的 esptool 一起杀掉
# —— 表现为"烧录返回 OK，但应用槽只写了一半"，MCUboot 报
# "Image in the primary slot is not valid!"（2026-09-11 实际踩到）。
# 正确做法：先把输出落盘，进程结束后再检查日志与退出码。
LOG=/tmp/usb_stable_last.log

esp_run() {
    timeout 300 "$ESPTOOL" --chip esp32p4 --port "$FLASH_PORT" "$@" >"$LOG" 2>&1
    return $?
}

# 校验日志：每个镜像都必须有完整的 "Wrote N bytes ... Hash of data verified"
verify_log() {
    local n_boot n_app
    n_boot=$(grep -c "Wrote 32768 bytes at 0x00002000" "$LOG" 2>/dev/null || echo 0)
    n_app=$(grep -c "Hash of data verified" "$LOG" 2>/dev/null || echo 0)
    n_app=${n_app:-0}
    # flash 动作要求两次 hash 校验（引导 + 应用）
    if [ "${EXPECT_TWO:-0}" = "1" ]; then
        [ "$n_app" -ge 2 ] && grep -q "Hard resetting" "$LOG"
    else
        grep -qE "Hard resetting" "$LOG"
    fi
}

esp_retry() {
    local desc="$1"; shift
    for i in 1 2 3; do
        echo "[$desc] try $i..."
        esp_run "$@"
        local rc=$?
        if [ "$rc" -eq 0 ] && verify_log; then
            echo "[$desc] OK (try $i)"
            wait_stable || true
            return 0
        fi
        if [ "$rc" -ne 0 ]; then
            echo "[$desc] try $i: esptool 失败 (rc=$rc)，日志尾部：" >&2
        else
            echo "[$desc] try $i: esptool 返回 0 但日志不完整（写被中断？），日志尾部：" >&2
        fi
        tail -4 "$LOG" >&2
        sleep 3
    done
    echo "[$desc] FAILED after 3 tries (see $LOG)" >&2
    return 1
}

require_file() {
    if [ ! -f "$1" ]; then
        echo "ERROR: 缺少文件 $1" >&2
        echo "       先构建：./build.sh nuttx/boards/risc-v/esp32p4/esp32p4-function-ev-board/configs/nsh/ --cmake -j8" >&2
        return 1
    fi
    printf '  %-52s %9s B\n' "$1" "$(stat -c%s "$1")"
    return 0
}

case "$ACTION" in
    flash)
        echo "── MCUboot 两镜像烧录 ──"
        require_file "$BOOT" || exit 1
        require_file "$FW"   || exit 1
        echo "  0x2000  <- 引导 (MCUboot)"
        echo "  0x20000 <- 应用 (OTA_0 主槽)"
        # flash 路径要求日志里出现"两次" hash 校验（引导 + 应用）
        EXPECT_TWO=1
        if ! esp_retry "flash" --before default_reset --after hard_reset \
                write_flash --no-compress \
                $BOOT_OFFSET "$BOOT" \
                $APP_OFFSET  "$FW"; then
            echo "❌ 烧录失败：请查看 $LOG" >&2
            exit 1
        fi
        echo "✅ 烧录完成（两镜像均已 hash 校验）：console 在 $CONSOLE_PORT (115200)"
        echo "   复位后应见：MCUboot banner -> NuttShell (NSH) -> nsh>"
        ;;
    chip_id)
        esp_retry "chip_id" --before default_reset chip_id || exit 1
        ;;
    reset)
        esp_retry "reset" --before default_reset --no-stub chip_id || exit 1
        ;;
    *)
        echo "Usage: $0 [app.bin] [mcuboot.bin] {flash|chip_id|reset}" >&2
        exit 1
        ;;
esac
