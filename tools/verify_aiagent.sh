#!/bin/bash
# verify_aiagent.sh - 稳定验证 ai_agent 启动
# 用 usb_stable 复位确保设备稳定,然后启动 ai_agent 持续读取(带重试)
PORT=/dev/ttyACM0
FW=${1:-/home/ez/share/openvela/cmake_out/esp32p4-function-ev-board_nsh/nuttx.bin}
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "[1/3] 复位确保设备稳定..."
bash "$SCRIPT_DIR/usb_stable.sh" "$FW" reset || exit 1

echo "[2/3] 启动 ai_agent 并读取(最多 4 次尝试)..."
for attempt in 1 2 3 4; do
    echo "--- attempt $attempt ---"
    timeout 30 python3 << 'EOF'
import serial, time
PORT = '/dev/ttyACM0'
try:
    ser = serial.Serial(PORT, 115200, timeout=0.3)
    time.sleep(1.5)
    ser.reset_input_buffer()
    ser.write(b'\r\n')
    time.sleep(1.5)
    ser.reset_input_buffer()
    ser.write(b'ai_agent\r\n')
    data = b''
    t0 = time.time()
    while time.time() - t0 < 18:
        d = ser.read(2048)
        if d: data += d
        else: time.sleep(0.3)
    txt = data.decode(errors='replace')
    print(f"CAPTURED {len(txt)} bytes")
    print(txt[-2500:])
    if 'READY' in txt or 'vela>' in txt:
        print(">>> AI_AGENT READY!")
        open('/tmp/agent_ready.txt','w').write(txt)
        import sys; sys.exit(0)
    ser.close()
except Exception as e:
    print("ERR:", e)
EOF
    rc=$?
    if [ $rc -eq 0 ] && grep -q "READY" /tmp/agent_ready.txt 2>/dev/null; then
        echo "[3/3] ✅ ai_agent READY"
        exit 0
    fi
    echo "未捕获 READY,复位重试..."
    bash "$SCRIPT_DIR/usb_stable.sh" "$FW" reset
done
echo "[3/3] 多次尝试未确认 READY(可能 USB 窗口不足)"
