#!/usr/bin/env bash
# tools/test/sim_deskmate.sh —— DeskMate 应用层：NuttX 模拟器一键闭环
#
# 一条命令完成「配置 → 构建 → 无头跑 UI 与逻辑 → 快照 → 断言 → 归档证据」，
# 全程不需要 X11、不需要摄像头、不需要 root。
#
#   tools/test/sim_deskmate.sh                 # 构建 + 运行 + 断言
#   tools/test/sim_deskmate.sh --tour         # 巡览演示：逐页截图 + 拼 GIF（无断言）
#   tools/test/sim_deskmate.sh --no-build      # 复用已有构建
#   tools/test/sim_deskmate.sh --keep-running  # 不设超时（调试用）
#   tools/test/sim_deskmate.sh --out <dir>     # 证据归档目录（默认 /tmp/deskmate-runs）
#
# 产物：<out>/<时间戳>/ 下
#   console.log          guest 控制台全文
#   dm_home.bin          UI 帧快照（RAW RGB565，来自应用内 hostfs 落盘）
#   dm_home.bin.txt      快照几何描述（宽/高/bpp/stride/后端）
#   dm_home.png          转换出的 PNG（tools/test/fb2png.py）
#   result.txt           本次判定摘要
#
# 退出码：0 = RESULT: PASS；非 0 = 失败（细节见 result.txt / console.log）

set -u -o pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${HERE}/../.." && pwd)"
WS="$(cd "${REPO}/.." && pwd)"

CONFIG_DIR="nuttx/boards/sim/sim/sim/configs/deskmate"
BUILD_DIR="${WS}/cmake_out/sim_deskmate"
SIM_BIN="${BUILD_DIR}/nuttx"
STAMP="$(date +%Y%m%d-%H%M%S)"
# 证据目录默认放在 /tmp：本仓的 logs/ 是未跟踪目录，同工作区的其它会话跑
# `git clean -xdf` 会把它连根清掉（实测踩过）。要入库时显式 `--out logs/sim-deskmate`。
OUT_BASE="${TMPDIR:-/tmp}/deskmate-runs"
RUN_DIR=""
# 交换目录用短路径：NSH 命令行长度有限（本配置已抬到 512，但短路径让默认配置
# 也能跑），跑完把产物拷回 RUN_DIR。
HOST_DIR="${TMPDIR:-/tmp}/dm-sim-${STAMP}"
TIMEOUT_SECS=180

DO_BUILD=1
KEEP_RUNNING=0
MODE="selftest"          # selftest（断言）| tour（巡览演示 + GIF）

while [ $# -gt 0 ]; do
  case "$1" in
    --no-build)     DO_BUILD=0 ;;
    --tour)         MODE="tour" ;;
    --keep-running) KEEP_RUNNING=1; TIMEOUT_SECS=0 ;;
    --out)          shift; OUT_BASE="${1:-${OUT_BASE}}" ;;
    -h|--help)
      sed -n '2,22p' "${BASH_SOURCE[0]}"
      exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

RUN_DIR="${OUT_BASE%/}/${STAMP}"
mkdir -p "${RUN_DIR}" "${HOST_DIR}"

log()  { printf '\033[1;36m[sim-deskmate]\033[0m %s\n' "$*"; }
fail() { printf '\033[1;31m[sim-deskmate]\033[0m %s\n' "$*"; }

# --------------------------------------------------------------------------
# 0. 宿主环境适配（无需 root，不改上游源码）
# --------------------------------------------------------------------------
# ① libunwind：arch/sim 的链接规则无条件加 -lunwind，而不少发行版只装了
#    libunwind.so.<N>（运行时包），没有开发用的 libunwind.so 符号链接。
#    这里造一个私有符号链接目录并用 LIBRARY_PATH 交给 gcc，等价于装 dev 包。
SHIM_DIR="${WS}/cmake_out/.libshim"
unwind_shim() {
  if ldconfig -p 2>/dev/null | grep -q 'libunwind\.so\b'; then
    return 0                                    # 有 dev 包，什么都不用做
  fi
  local real
  real=$(ls /usr/lib/*/libunwind.so.* /lib/*/libunwind.so.* 2>/dev/null | head -1)
  if [ -z "${real}" ]; then
    return 0
  fi
  mkdir -p "${SHIM_DIR}"
  ln -sf "${real}" "${SHIM_DIR}/libunwind.so"
  export LIBRARY_PATH="${SHIM_DIR}${LIBRARY_PATH:+:${LIBRARY_PATH}}"
  log "libunwind dev symlink missing; using ${SHIM_DIR} (-> ${real})"
}

# ② 陈旧的内联生成头：apps/builtin/builtin_list.c 用引号包含 "builtin_list.h"，
#    会优先命中 apps/builtin/ 下上一次「别的配置」构建留下的旧副本，从而引用到
#    本配置根本没编的应用（undefined reference to xxx_main）。删掉它，
#    编译期就会用构建目录里本配置新生成的那份。
drop_stale_builtin_header() {
  local f="${WS}/apps/builtin/builtin_list.h"
  if [ -f "${f}" ] && ! git -C "${WS}/apps" ls-files --error-unmatch \
       builtin/builtin_list.h >/dev/null 2>&1; then
    log "removing stale in-tree apps/builtin/builtin_list.h (untracked leftover)"
    rm -f "${f}" "${f%.h}_proto.h" 2>/dev/null || true
  fi
}

unwind_shim
drop_stale_builtin_header

# --------------------------------------------------------------------------
# 1. 构建
# --------------------------------------------------------------------------
if [ "${DO_BUILD}" = "1" ]; then
  log "building ${CONFIG_DIR} (this takes a few minutes on a cold tree)"
  if ! (cd "${WS}" && ./build.sh "${CONFIG_DIR}" --cmake -j8) \
        > "${RUN_DIR}/build.log" 2>&1; then
    fail "build FAILED — see ${RUN_DIR}/build.log"
    echo "RESULT: FAIL (build)" > "${RUN_DIR}/result.txt"
    exit 1
  fi
  log "build ok ($(grep -c . "${RUN_DIR}/build.log") log lines)"
else
  log "skipping build (--no-build)"
  : > "${RUN_DIR}/build.log"
fi

if [ ! -x "${SIM_BIN}" ]; then
  fail "simulator binary missing: ${SIM_BIN}"
  echo "RESULT: FAIL (no binary)" > "${RUN_DIR}/result.txt"
  exit 1
fi

# --------------------------------------------------------------------------
# 2. 运行
# --------------------------------------------------------------------------
# 先喂命令、再给足时间；应用跑完自检会自己退出，模拟器本体随后被超时收掉。
# 快照走应用内默认路径 /host/deskmate/dm_home.bin，命令行只带 hostfs。
CMDS=$'deskmate --'"${MODE}"$' --hostfs '"${HOST_DIR}"$'\n'

log "running ${SIM_BIN} (mode ${MODE}, deadline ${TIMEOUT_SECS}s)"

# 应用跑完自检/巡览会自己退出；模拟器本体停在 NSH，所以这里不等它，而是
# 「后台起 + 轮询日志里的收尾标记 + 收到就收掉」。比死等 timeout 快得多
# （一条命令从 ~3 分钟降到 ~20 秒）。
FIFO="${RUN_DIR}/.stdin"
rm -f "${FIFO}"
mkfifo "${FIFO}"

( printf '%s' "${CMDS}" > "${FIFO}" ) &
WRITER=$!

( cd "${RUN_DIR}" && exec "${SIM_BIN}" ) < "${FIFO}" > "${RUN_DIR}/console.log" 2>&1 &
SIM_PID=$!

DEADLINE=$(( $(date +%s) + TIMEOUT_SECS ))
DONE_MARK="DM-SELFTEST RESULT:"
[ "${MODE}" = "tour" ] && DONE_MARK="DM-TOUR DONE"

while [ "$(date +%s)" -lt "${DEADLINE}" ]; do
  if grep -aq "${DONE_MARK}" "${RUN_DIR}/console.log" 2>/dev/null; then
    # 给应用一点时间打印收尾行（DM-EXIT）再收掉模拟器
    sleep 1
    break
  fi
  kill -0 "${SIM_PID}" 2>/dev/null || break
  sleep 1
done

if grep -aq "${DONE_MARK}" "${RUN_DIR}/console.log" 2>/dev/null; then
  log "run finished ($(grep -am1 "${DONE_MARK}" "${RUN_DIR}/console.log" | tr -d '\r'))"
else
  log "deadline/exit reached without the completion marker"
fi

kill "${SIM_PID}" 2>/dev/null || true
wait "${SIM_PID}" 2>/dev/null || true
kill "${WRITER}" 2>/dev/null || true
wait "${WRITER}" 2>/dev/null || true
rm -f "${FIFO}"
RC=0

log "simulator finished (rc=${RC})"

# --------------------------------------------------------------------------
# 3. 断言
# --------------------------------------------------------------------------
PASS_COUNT=$(grep -c '\[dm-check\].* PASS' "${RUN_DIR}/console.log" || true)
FAIL_COUNT=$(grep -c '\[dm-check\].* FAIL' "${RUN_DIR}/console.log" || true)
SELFTEST_LINE=$(grep -m1 'DM-SELFTEST RESULT:' "${RUN_DIR}/console.log" || true)
READY_LINE=$(grep -m1 'DM-READY' "${RUN_DIR}/console.log" || true)
SNAP_BIN="${HOST_DIR}/deskmate/dm_home.bin"   # selftest 才保证存在
SNAP_TXT="${SNAP_BIN}.txt"
SNAP_PNG="${RUN_DIR}/dm_home.png"

STATUS=PASS
REASON=""

[ -n "${READY_LINE}" ] || { STATUS=FAIL; REASON="no DM-READY line (UI never came up)"; }

if [ "${MODE}" = "tour" ]; then
  TOUR_LINE=$(grep -m1 'DM-TOUR DONE' "${RUN_DIR}/console.log" || true)
  [ -n "${TOUR_LINE}" ] || { STATUS=FAIL; REASON="tour did not finish"; }
else
  echo "${SELFTEST_LINE}" | grep -q 'PASS' || { STATUS=FAIL; REASON="self-test did not pass"; }
  [ "${FAIL_COUNT}" = "0" ] || { STATUS=FAIL; REASON="${FAIL_COUNT} check(s) failed"; }
  [ -f "${SNAP_BIN}" ] || { STATUS=FAIL; REASON="snapshot missing"; }
fi

# hostfs 必须挂到我们给的那个目录：命令行被截断时挂载点会不同，快照就会丢。
grep -q "hostfs: ${HOST_DIR} mounted at /host" "${RUN_DIR}/console.log" \
  || { STATUS=FAIL; REASON="hostfs mount path mismatch (command line truncated?)"; }

# --------------------------------------------------------------------------
# 4. 快照 → PNG（复用真机同一条证据链）
# --------------------------------------------------------------------------
# 自检会把每个页面各写一份 RAW RGB565 快照（dm_home / dm_camera / dm_net …），
# 这里把它们全部转成 PNG 并连同原始帧一起归档。
SNAP_DIR="${HOST_DIR}/deskmate"
SNAP_COUNT=0
PNG_COUNT=0

if [ -d "${SNAP_DIR}" ]; then
  for bin in "${SNAP_DIR}"/*.bin; do
    [ -f "${bin}" ] || continue
    SNAP_COUNT=$((SNAP_COUNT + 1))

    name="$(basename "${bin}" .bin)"
    txt="${bin}.txt"
    w=1024
    h=600
    if [ -f "${txt}" ]; then
      w=$(sed -n 's/^width=//p'  "${txt}" | head -1); w=${w:-1024}
      h=$(sed -n 's/^height=//p' "${txt}" | head -1); h=${h:-600}
    fi

    if python3 "${HERE}/fb2png.py" "${bin}" "${RUN_DIR}/${name}.png" "${w}" "${h}" \
         >> "${RUN_DIR}/fb2png.log" 2>&1; then
      PNG_COUNT=$((PNG_COUNT + 1))
      cp -f "${bin}" "${RUN_DIR}/${name}.bin" 2>/dev/null || true
      [ -f "${txt}" ] && cp -f "${txt}" "${RUN_DIR}/${name}.bin.txt" 2>/dev/null || true
      log "snapshot: ${name}.png (${w}x${h})"
    else
      STATUS=FAIL
      REASON="${REASON:+${REASON}; }fb2png failed for ${name}"
    fi
  done
fi

if [ "${SNAP_COUNT}" = "0" ]; then
  STATUS=FAIL
  REASON="${REASON:+${REASON}; }no snapshot produced by the self-test"
fi

log "snapshots: ${PNG_COUNT}/${SNAP_COUNT} converted"

if [ "${MODE}" = "tour" ]; then
  if python3 "${HERE}/dm_tour_gif.py" "${RUN_DIR}" >> "${RUN_DIR}/fb2png.log" 2>&1; then
    log "animation: ${RUN_DIR}/deskmate_tour.gif"
  else
    log "animation skipped (no tour_*.png)"
  fi
fi

# 5. 汇总
# --------------------------------------------------------------------------
{
  echo "RESULT: ${STATUS}"
  echo "reason: ${REASON:-all assertions passed}"
  echo "ready:  ${READY_LINE}"
  echo "checks: ${PASS_COUNT} pass / ${FAIL_COUNT} fail"
  echo "mode:   ${MODE}"
  echo "result: ${SELFTEST_LINE}"
  echo "artifact_dir: ${RUN_DIR}"
} > "${RUN_DIR}/result.txt"

cat "${RUN_DIR}/result.txt"

if [ "${STATUS}" = "PASS" ]; then
  log "RESULT: PASS"
  exit 0
else
  fail "RESULT: FAIL — ${REASON}"
  exit 1
fi
