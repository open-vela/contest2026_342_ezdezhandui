#!/usr/bin/env bash
# =============================================================================
# cleanroom_pipeline.sh —— clean-room 全流程（后台无人值守）
#
# 目标：在"评审端状态"的工作区上，从零配置构建 → 烧录 → 抓真机证据。
#
# 步骤
#   1. 快路径：用预置的 esp-hal-3rdparty + FetchContent stamp 直接全新 configure
#      （若 build.sh 因缺 cache 拒绝，则自动退回慢路径）
#   2. 慢路径：删掉构建目录，让 build.sh 自行 clone esp-hal-3rdparty（约 700MB）
#   3. 构建成功后：bash tools/build_flash/usb_stable.sh 烧录
#   4. python3 tools/test/evidence.py … 抓串口 + 逻辑分析仪证据
#   5. 提取三项验收行，写入 /tmp/cleanroom_result.txt
#
# 日志：/tmp/cleanroom_pipeline.log    结果：/tmp/cleanroom_result.txt
# =============================================================================
set -u

WS=/home/ez/share/openvela
REPO=$WS/contest2026_342_ezdezhandui
BUILD=$WS/cmake_out/esp32p4-function-ev-board_nsh
CFG=vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/configs/nsh
LOG=/tmp/cleanroom_pipeline.log
RES=/tmp/cleanroom_result.txt
CACHE=$(ls -d /tmp/broken-build-* 2>/dev/null | head -1)

say() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$LOG"; }

: > "$LOG"
: > "$RES"
say "=== clean-room 流水线开始 ==="
say "工作区: $WS   缓存来源: ${CACHE:-无}"

cd "$WS" || exit 1

# ── 1. 快路径：预置缓存 + 全新 configure ─────────────────────────────────────
if [ -n "${CACHE:-}" ] && [ -d "$CACHE/arch/risc-v/src/common/espressif/esp-hal-3rdparty" ]; then
  say "[1/5] 预置 hal 缓存到全新构建目录"
  rm -rf "$BUILD"
  mkdir -p "$BUILD/arch/risc-v/src/common/espressif" "$BUILD/_deps"
  cp -a "$CACHE/arch/risc-v/src/common/espressif/esp-hal-3rdparty" \
        "$BUILD/arch/risc-v/src/common/espressif/" 2>>"$LOG"
  cp -a "$CACHE/_deps/esp_hal_3rdparty-subbuild" "$BUILD/_deps/" 2>>"$LOG"
  cp -a "$CACHE/_deps/esp_hal_3rdparty-build" "$BUILD/_deps/" 2>>"$LOG"
  say "      hal 树: $(du -sh "$BUILD/arch/risc-v/src/common/espressif/esp-hal-3rdparty" 2>/dev/null | cut -f1)"
fi

say "[2/5] 全新构建（快路径：Configure + ninja）"
if ./build.sh "$CFG" --cmake -j8 >>"$LOG" 2>&1; then
  say "      快路径构建成功"
else
  say "      快路径失败（多半是缺 cache），退回慢路径：删目录让 build.sh 自行 clone"
  rm -rf "$BUILD"
  if ! ./build.sh "$CFG" --cmake -j8 >>"$LOG" 2>&1; then
    say "❌ 慢路径构建也失败，流水线中止（日志见 $LOG）"
    echo "BUILD_FAILED" >> "$RES"
    exit 2
  fi
  say "      慢路径构建成功"
fi

# ── 3. 烧录 ────────────────────────────────────────────────────────────────
say "[3/5] 烧录"
cd "$REPO" || exit 1
if bash tools/build_flash/usb_stable.sh >>"$LOG" 2>&1; then
  say "      烧录完成"
else
  say "❌ 烧录失败"
  echo "FLASH_FAILED" >> "$RES"
  exit 3
fi

# ── 4. 抓证据（串口 + 逻辑分析仪）────────────────────────────────────────────
say "[4/5] 抓取真机证据（约 7 分钟）"
sleep 5
if python3 tools/test/evidence.py --reset --seconds 400 --analyzer 400 --decode \
        --tag cleanroom >>"$LOG" 2>&1; then
  say "      证据抓取完成"
else
  say "⚠️ 证据脚本返回非零，仍继续解析已有日志"
fi

# ── 5. 提取三项验收行 ───────────────────────────────────────────────────────
say "[5/5] 提取验收证据"
D=$(ls -td "$REPO"/logs/*cleanroom* 2>/dev/null | head -1)
{
  echo "证据目录: $D"
  echo "--- 验收行 ---"
  grep -ao 'MCUboot' "$D/boot.txt" 2>/dev/null | head -1
  grep -ao 'NuttShell (NSH)' "$D/boot.txt" 2>/dev/null | head -1
  grep -ao 'connected to "[^"]*"[^,]*' "$D/boot.txt" 2>/dev/null | head -1
  grep -ao 'DhcpDnsStatus iface=[0-9-]* link=1 dhcp=1 ip=[0-9.]*' "$D/boot.txt" 2>/dev/null | head -1
  grep -ao 'wlan0 address [0-9.]* mask [0-9.]* gw [0-9.]*' "$D/boot.txt" 2>/dev/null | head -1
} >> "$RES" 2>&1

if grep -q 'wlan0 address' "$RES" 2>/dev/null; then
  say "✅ clean-room 全流程通过（构建 + 烧录 + 真机三项证据）"
  echo "PASS" >> "$RES"
else
  say "⚠️ 全流程跑完，但未在日志里找到 wlan0 证据行（见 $RES）"
  echo "NO_EVIDENCE" >> "$RES"
fi
say "=== 流水线结束 ==="
