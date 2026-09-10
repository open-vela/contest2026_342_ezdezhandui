#!/bin/bash
# demo_aiagent.sh - 桌伴 DeskMate 一键演示脚本
# 叙事: 烧录最新固件 → 复位稳定 → 启动 ai_agent → 展示 READY + 核心功能输出
#
# 用法:
#   tools/demo_aiagent.sh                 # 用默认最新固件
#   tools/demo_aiagent.sh <firmware.bin>  # 指定固件
#
# 依赖: tools/usb_stable.sh(稳定复位) + tools/verify_aiagent.sh(启动验证)
# 前置: USB 已连(esptool 可用), /dev/ttyACM0 存在

set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FW="${1:-/home/ez/share/openvela/cmake_out/esp32p4-function-ev-board_nsh/nuttx.bin}"

echo "════════════════════════════════════════════"
echo "  桌伴 DeskMate · 一键演示"
echo "════════════════════════════════════════════"
echo "[0/4] 固件: $FW ($(stat -c%s "$FW" 2>/dev/null || echo '?')B)"

# 1. 烧录(幂等,带稳定等待)
echo "[1/4] 烧录固件..."
bash "$SCRIPT_DIR/usb_stable.sh" "$FW" flash || { echo "✗ 烧录失败"; exit 1; }

# 2. 启动验证 ai_agent(最多 4 次尝试)
echo "[2/4] 启动 ai_agent 并验证..."
bash "$SCRIPT_DIR/verify_aiagent.sh" "$FW" || { echo "✗ ai_agent 未 READY"; exit 1; }

# 3. 展示关键输出
echo "[3/4] 演示输出摘要:"
if [ -f /tmp/agent_ready.txt ]; then
    echo "--- 捕获片段(尾部 1200B) ---"
    tail -c 1200 /tmp/agent_ready.txt
    echo ""
    echo "--- 关键标记检查 ---"
    for m in "READY" "vela>" "36 tools" "10 skills"; do
        grep -q "$m" /tmp/agent_ready.txt && echo "  ✓ 含 '$m'" || echo "  - 未含 '$m'(视固件输出而定)"
    done
fi

echo "[4/4] ✅ 演示完成。"
echo "提示: 交互式 CLI 演示请用 serial-shell/serial-monitor 连 ttyACM0, 输入 'ai_agent' 启动, 'vela> ' 下对话。"
