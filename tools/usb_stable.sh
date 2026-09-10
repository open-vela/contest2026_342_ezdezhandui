#!/bin/bash
# stable_usb.sh - 稳定的 ESP32-P4 USB 交互工具
# 根因: esptool --before default_reset 每次硬件复位→设备重枚举(短暂消失)
# 解决: 每次 USB 操作后等待设备稳定(udev settle + sleep),并带重试
PORT=/dev/ttyACM0
FW=${1:-/home/ez/share/openvela/cmake_out/esp32p4-function-ev-board_nsh/nuttx.bin}

# 等待设备稳定(复位后设备重枚举,需等 ttyACM0 出现 + 可打开 + 缓冲)
wait_stable() {
    for i in $(seq 1 15); do
        if ls "$PORT" >/dev/null 2>&1 && [ -c "$PORT" ]; then
            # 用 python 测试串口能否打开(设备完全就绪)
            if timeout 5 python3 -c "
import serial
try:
    ser = serial.Serial('$PORT', 115200, timeout=0.2)
    ser.close()
    print('ready')
except Exception:
    pass" 2>/dev/null | grep -q ready; then
                sleep 3
                return 0
            fi
        fi
        sleep 1
    done
    echo "ERROR: $PORT 未就绪" >&2
    return 1
}

# esptool 带重试
esp_retry() {
    local desc="$1"; shift
    for i in 1 2 3; do
        echo "[$desc] try $i..."
        if timeout 90 ~/.local/bin/esptool.py --chip esp32p4 --port "$PORT" "$@" 2>&1 | grep -q "Hard resetting"; then
            echo "[$desc] OK (try $i)"
            wait_stable
            return 0
        fi
        sleep 3
    done
    echo "[$desc] FAILED after 3 tries" >&2
    return 1
}

case "${2:-flash}" in
    flash)
        esp_retry "flash" --before default_reset write_flash 0x2000 "$FW"
        ;;
    chip_id)
        esp_retry "chip_id" --before default_reset chip_id
        ;;
    reset)
        esp_retry "reset" --before default_reset --after hard_reset chip_id
        ;;
    *)
        echo "Usage: $0 [firmware.bin] {flash|chip_id|reset}" >&2
        exit 1
        ;;
esac
