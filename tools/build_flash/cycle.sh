#!/bin/bash
# cycle.sh — 一条命令跑完目标要求的"标准循环"
#
#     改  -> 人工编辑源码（本脚本不替你做）
#     建  -> build.sh
#     烧  -> usb_stable.sh（板子不在时跳过）
#     验  -> 复位 + 音频自检（+ 可选逻辑分析仪采集）
#     导  -> export.py --apply --manifest + 一致性检查
#     提  -> git commit（给出明确的提交信息）
#
# 用法：
#   tools/build_flash/cycle.sh                       # 建+验+导，不提交
#   tools/build_flash/cycle.sh -m "fix(x): ..."      # 全部走完并提交
#   tools/build_flash/cycle.sh --skip-flash          # 跳过烧录/真机验证
#   tools/build_flash/cycle.sh --analyzer            # 验的时候顺带抓 SDIO 波形
#
# 设计原则：每一步都打印它实际做了什么；**任何一步失败立即停**，
# 不掩盖错误。板子不可达是"跳过"而不是"失败"，但会明确写出来。

set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
WS="${OPENVELA:-/home/ez/share/openvela}"
BOARD_CFG="vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/configs/nsh"

MSG=""
SKIP_FLASH=0
ANALYZER=0
while [ $# -gt 0 ]; do
  case "$1" in
    -m) MSG="$2"; shift 2 ;;
    --skip-flash) SKIP_FLASH=1; shift ;;
    --analyzer) ANALYZER=1; shift ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "未知参数: $1" >&2; exit 2 ;;
  esac
done

step() { printf '\n\033[1m== %s ==\033[0m\n' "$*"; }
die()  { printf '\033[31m失败: %s\033[0m\n' "$*" >&2; exit 1; }

step "建 (build)"
( cd "$WS" && ./build.sh "$BOARD_CFG" --cmake -j8 > /tmp/cycle_build.log 2>&1 )
rc=$?
if [ $rc -ne 0 ]; then
  # The MCUboot/esp-hal ExternalProject steps need network; on this offline box
  # they are neutralised in the build dir. Re-apply and retry once.
  echo "首次构建失败，施加离线补丁后重试……"
  "$HERE/offline_build_fix.sh" >/dev/null 2>&1 || true
  ( cd "$WS" && ./build.sh "$BOARD_CFG" --cmake -j8 > /tmp/cycle_build.log 2>&1 )
  rc=$?
fi
[ $rc -eq 0 ] || die "构建失败，见 /tmp/cycle_build.log"
grep -q '^#### build completed successfully' /tmp/cycle_build.log \
  || die "构建没有报告成功，见 /tmp/cycle_build.log"
echo "构建成功: $(stat -c%s "$WS/cmake_out/esp32p4-function-ev-board_nsh/nuttx.bin") B"

if [ $SKIP_FLASH -eq 1 ]; then
  step "烧/验 (flash/verify) —— 已按 --skip-flash 跳过"
elif [ ! -e /dev/ttyUSB0 ]; then
  step "烧/验 (flash/verify) —— 板子不可达，跳过"
  echo "  没有 /dev/ttyUSB0；插好板子或去掉 --skip-flash 重跑。"
else
  step "烧 (flash)"
  flash_out=$( cd "$REPO" && tools/build_flash/usb_stable.sh 2>&1 )
  echo "$flash_out" | tail -2
  # Run it once and judge from the captured text: substituting the script into a
  # grep process-substitution would flash the board a second time.
  echo "$flash_out" | grep -qi "烧录完成" || die "烧录未报告成功"

  step "验 (verify)"
  python3 "$REPO/tools/test/board.py" reset --timeout 25 --quiet >/dev/null 2>&1
  out=$(python3 "$REPO/tools/test/board.py" run "audiotest -t 1" --timeout 45 2>&1)
  echo "$out" | grep -E "queued|done" | tail -2
  echo "$out" | grep -q "audiotest: done" || die "音频自检没有走到 done"

  if [ $ANALYZER -eq 1 ]; then
    step "验 (analyzer) —— 抓 SDIO 波形"
    python3 "$REPO/tools/test/pxview/watch.py" 1 4000000 1000000 2>&1 | tail -5
  fi
fi

step "导 (export)"
( cd "$REPO" && python3 tools/engineering/export.py --apply --manifest 2>&1 | tail -3 )
( cd "$REPO" && python3 tools/engineering/gen_manifest_copyfiles.py --check 2>&1 | tail -2 )
( cd "$REPO" && python3 tools/engineering/gen_manifest_copyfiles.py --check >/dev/null 2>&1 ) \
  || die "清单与文件树不一致"

step "提 (commit)"
if [ -z "$MSG" ]; then
  echo "未提供 -m，跳过提交。改动如下："
  git -C "$REPO" status --short
  exit 0
fi
git -C "$REPO" add -A
git -C "$REPO" -c user.name="${GIT_USER:-ez}" -c user.email="${GIT_EMAIL:-ez@localhost}" \
  commit -q -m "$MSG"
echo "已提交: $(git -C "$REPO" log --oneline -1)"
