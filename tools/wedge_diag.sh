#!/bin/bash
# wedge_diag.sh —— 诊断 "ai_agent 启动后整机失聪" 的现场（JTAG 取证）
#
# 症状（2026-09-17，最终固件，3/3 次稳定复现）：
#   nsh> ai_agent  →  启动日志走到 "[agent] All network services started!" 后
#   ping 100% 丢包、TCP 28789 连不上、串口输入无回显；系统再也不会自愈。
#
# 本脚本不做任何写入（只 halt/读/resume），两次采样间隔 4s 即可判定：
#   * mcycle / minstret 涨不涨   → CPU 到底还在不在执行
#   * g_system_ticks 涨不涨      → NuttX 100Hz 节拍中断是否还活着
#   * SYSTIMER INT_RAW/INT_ST    → 外设是否还在向 CPU 请求中断（挂起没被清）
#   * mstatus/mintthresh/mtvec   → 是否被谁把中断关掉/阈值抬高/向量表改掉
#   * PC 落在哪里                → WFI 空闲 还是 trap 进出路径（中断风暴）
#
# 用法:
#   tools/wedge_diag.sh              # 采集两次（间隔 4s）并打印判读
#   tools/wedge_diag.sh healthy      # 只采一次（对照组：复位后、未启 ai_agent）
#
# 依赖: openocd-esp32（默认 /home/ez/tools/openocd-esp32/bin/openocd，可用 OCD= 覆盖）
#       开发板 USB-JTAG 口空闲（别同时跑 gdb/其它 openocd）
set -u

# openocd 查找顺序：$OCD 环境变量 → PATH → 本机已知路径（换机器时用 OCD= 覆盖）
if [ -z "${OCD:-}" ]; then
    if command -v openocd >/dev/null 2>&1; then OCD="$(command -v openocd)"
    else OCD="/home/ez/tools/openocd-esp32/bin/openocd"; fi
fi
CFG="${CFG:-board/esp32p4-builtin.cfg}"
MODE="${1:-wedge}"

# --- 与 nuttx.map / System.map 对应的符号地址（换固件后按 tools/README_usb.md 重取）---
TICKS=0x4ff589a4          # g_system_ticks
IRQVEC=0x4ff54040         # g_irqvector[18]
HANDLERS=0x4ff4bec8       # s_intr_handlers[core0][32]  (intr_handler_item_t = {fn,arg})
SYST=0x500E2000           # DR_REG_SYSTIMER_BASE
UART0=0x500CA000          # DR_REG_UART0_BASE

EXPOSE='esp32p4.hp.cpu0 riscv expose_csrs 768=mstatus 772=mie 836=mip 773=mtvec 775=mtvt 833=mepc 834=mcause 2816=mcycle 2818=minstret'
FILTER='^(pc |mstatus|mie|mip|mtvec|mtvt|mepc|mcause|mcycle|minstret|0x[0-9a-f]{8}:)'

sample() {
  echo "───── sample $1  $(date +%T)"
  "$OCD" -f "$CFG" -c "$EXPOSE" -c "init" -c "halt" \
    -c "reg pc" -c "reg mstatus" -c "reg mie" -c "reg mip" -c "reg mtvec" -c "reg mtvt" \
    -c "reg mepc" -c "reg mcause" -c "reg mintthresh" -c "reg mcycle" -c "reg minstret" \
    -c "echo {-- g_system_ticks --}"    -c "mdw $TICKS 1" \
    -c "echo {-- UART0 INT_RAW/INT_ENA/STATUS --}" \
    -c "mdw $(printf 0x%X $((UART0 + 0x04))) 1" \
    -c "mdw $(printf 0x%X $((UART0 + 0x0C))) 1" \
    -c "mdw $(printf 0x%X $((UART0 + 0x1C))) 1" \
    -c "echo {-- SYSTIMER INT_ENA/INT_RAW/INT_CLR/INT_ST  TARGET0_CONF  UNIT0_VALUE_LO --}" \
    -c "mdw $(printf 0x%X $((SYST + 0x64))) 4" \
    -c "mdw $(printf 0x%X $((SYST + 0x34))) 1" \
    -c "mdw $(printf 0x%X $((SYST + 0x44))) 1" \
    -c "echo {-- s_intr_handlers core0 (fn,arg)x6 --}" -c "mdw $HANDLERS 12" \
    -c "echo {-- g_irqvector --}" -c "mdw $IRQVEC 36" \
    -c "resume" -c "shutdown" 2>&1 | grep -E "$FILTER"
  echo
}

if [ "$MODE" = "healthy" ]; then
  echo "对照模式：请先复位开发板、且**不要**启动 ai_agent"
  sample healthy
  exit 0
fi

sample A
sleep 4
sample B

cat <<'EOF'
──────── 判读 ────────
 2026-09-17 晚：本症状**已定位并修复** —— riscv_doirq() 的早期引导保护
 `if (g_running_task == NULL) return regs;` 把 up_exit() 故意发出的
 ECALL(SYS_restore_context) 也一起吞掉，于是任何任务退出都卡成 ECALL 死循环
 （修复：该保护只对 irq > RISCV_MAX_EXCEPTION 的硬件中断生效）。
 本脚本保留作**回归检查**：若再出现下面的读数，说明这条路径又断了。
 现场与验证：logs/verify-2026-09-17/wedge_fix.txt、docs/06 §26.7

──────── 读数对照（修复前基线，原始记录 wedge_jtag.txt）────────
 健康态: pc 落在 esp_cpu_wait_for_intr(WFI) ; mstatus.MIE=1 ; g_system_ticks 每次采样都在涨
         SYSTIMER INT_RAW=0（每个 tick 都被 ISR 清掉）
 wedge  : pc 落在 exception_common/riscv_dispatch_irq/return_from_exception（trap 进出路径）
         mcycle/minstret 仍在前进（CPU 在跑）; g_system_ticks 两次采样完全相同（节拍死了）
         SYSTIMER INT_ENA=5 INT_RAW=5 INT_ST=5（TARGET0 节拍闹钟 + TARGET2 esp_timer 都在请求）
         mintthresh/mtvec/mtvt/mie/mip 与健康态一致（不是阈值/向量表的问题）
         s_intr_handlers 与健康态逐字节相同（intno0 仍是 systimer_irq_handler=4002f672）
 结论   : 外设在请求中断、处理器仍注册着，但 CPU 侧没有把这条中断跑完
          ⇒ 当时收敛到"中断投递失效"；后续埋点进一步证明真因是 up_exit 的
          SYS_restore_context ECALL 被 riscv_doirq() 的早期引导保护吞掉（见上）
EOF
