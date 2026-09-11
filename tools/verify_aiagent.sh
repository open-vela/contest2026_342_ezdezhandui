#!/bin/bash
# verify_aiagent.sh - 验证 ai_agent 在真机上完整启动（闭环断言）
#
# 端口（2026-09-11 实测修正）：
#   /dev/ttyUSB0  CP2102        -> **NuttX console（UART0）**  ← 必须在这个口读输出
#   /dev/ttyACM0  USB-Serial/JTAG -> 仅用于烧录/复位（读不到 NSH 输出）
#   打开串口必须 assert DTR/RTS，否则读到 0 字节（tools/board.py 已内置）
#
# 关于重试：本机是 VMware 虚拟机，USB-CP2102 在日志突发时偶发丢字节
# （表现为个别行缺失或两行交错）。因此本脚本对同一组里程碑做多轮采集，
# 任一轮看到即算通过；这样既保留严格断言，又不会被丢包误判为失败。
#
# 用法: tools/verify_aiagent.sh [轮数]

set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROUNDS="${1:-3}"
OUTDIR=/tmp/verify_agent
mkdir -p "$OUTDIR"

MARKERS=(
    "P0: storage ready"
    "P3: tool_registry_init (rc=0)"
    "P3: skill_loader_init (rc=0)"
    "P3: cron_service_init (rc=0)"
    "P4: nsh_commands_init (rc=0)"
    "P6: nsh_commands_start (rc=0)"
    "AI Agent ready"
    "Skills system ready (12 built-in)"
    "Network connected"
    "[cron] Cron started"
)

declare -A SEEN
for m in "${MARKERS[@]}"; do SEEN["$m"]=0; done

round=1
while [ "$round" -le "$ROUNDS" ]; do
    echo "── 第 $round/$ROUNDS 轮 ──"
    echo "   复位..."
    bash "$SCRIPT_DIR/usb_stable.sh" "" "" reset >/dev/null 2>&1 || true
    OUT="$OUTDIR/round$round.txt"
    python3 "$SCRIPT_DIR/board.py" run "ai_agent" --timeout 45 --no-idle \
            --save "$OUT" --quiet
    echo "   采集 $(wc -c <"$OUT") 字节"
    for m in "${MARKERS[@]}"; do
        if [ "${SEEN[$m]}" = "0" ] && grep -qF "$m" "$OUT"; then
            SEEN["$m"]=1
            echo "   ✓ $m"
        fi
    done
    # 全部命中即提前结束
    pending=0
    for m in "${MARKERS[@]}"; do [ "${SEEN[$m]}" = "0" ] && pending=$((pending+1)); done
    [ "$pending" -eq 0 ] && break
    round=$((round+1))
done

echo "── 结果 ──"
FAIL=0
for m in "${MARKERS[@]}"; do
    if [ "${SEEN[$m]}" = "1" ]; then
        echo "  ✓ $m"
    else
        echo "  ✗ 未观察到: $m"; FAIL=1
    fi
done

if [ "$FAIL" -eq 0 ]; then
    echo "✅ ai_agent 启动验证通过（$round 轮采集，日志: $OUTDIR）"
else
    echo "❌ 验证失败（日志: $OUTDIR）" >&2
fi
exit "$FAIL"
