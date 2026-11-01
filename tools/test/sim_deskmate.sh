#!/usr/bin/env bash
# tools/test/sim_deskmate.sh —— DeskMate 应用层：NuttX 模拟器一键闭环
#
# 一条命令完成「配置 → 构建 → 无头跑 UI 与逻辑 → 快照 → 断言 → 归档证据」，
# 全程不需要 X11、不需要摄像头、不需要 root。
#
#   tools/test/sim_deskmate.sh                 # 构建 + 运行 + 断言
#   tools/test/sim_deskmate.sh --no-build      # 复用已有构建
#   tools/test/sim_deskmate.sh --keep-running  # 不设超时（调试用）
#
# 产物：logs/sim-deskmate/<时间戳>/ 下
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
RUN_DIR="${REPO}/logs/sim-deskmate/${STAMP}"
# 交换目录用短路径：NSH 命令行长度有限（本配置已抬到 512，但短路径让默认配置
# 也能跑），跑完把产物拷回 RUN_DIR。
HOST_DIR="${TMPDIR:-/tmp}/dm-sim-${STAMP}"
TIMEOUT_SECS=180

DO_BUILD=1
KEEP_RUNNING=0

for arg in "$@"; do
  case "${arg}" in
    --no-build)     DO_BUILD=0 ;;
    --keep-running) KEEP_RUNNING=1; TIMEOUT_SECS=0 ;;
    -h|--help)
      sed -n '2,20p' "${BASH_SOURCE[0]}"
      exit 0 ;;
    *) echo "unknown option: ${arg}" >&2; exit 2 ;;
  esac
done

mkdir -p "${HOST_DIR}"

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
CMDS=$'deskmate --selftest --hostfs '"${HOST_DIR}"$' --snap /host/deskmate/dm_home.bin\n'

log "running ${SIM_BIN} (timeout ${TIMEOUT_SECS}s)"
if [ "${TIMEOUT_SECS}" = "0" ]; then
  printf '%s' "${CMDS}" | (cd "${RUN_DIR}" && "${SIM_BIN}") > "${RUN_DIR}/console.log" 2>&1
  RC=$?
else
  printf '%s' "${CMDS}" | timeout -k 5 "${TIMEOUT_SECS}" \
    bash -c "cd '${RUN_DIR}' && exec '${SIM_BIN}'" > "${RUN_DIR}/console.log" 2>&1
  RC=$?
fi

log "simulator finished (rc=${RC})"

# --------------------------------------------------------------------------
# 3. 断言
# --------------------------------------------------------------------------
PASS_COUNT=$(grep -c '\[dm-check\].* PASS' "${RUN_DIR}/console.log" || true)
FAIL_COUNT=$(grep -c '\[dm-check\].* FAIL' "${RUN_DIR}/console.log" || true)
SELFTEST_LINE=$(grep -m1 'DM-SELFTEST RESULT:' "${RUN_DIR}/console.log" || true)
READY_LINE=$(grep -m1 'DM-READY' "${RUN_DIR}/console.log" || true)
SNAP_BIN="${HOST_DIR}/deskmate/dm_home.bin"
SNAP_TXT="${SNAP_BIN}.txt"
SNAP_PNG="${RUN_DIR}/dm_home.png"

STATUS=PASS
REASON=""

[ -n "${READY_LINE}" ] || { STATUS=FAIL; REASON="no DM-READY line (UI never came up)"; }
echo "${SELFTEST_LINE}" | grep -q 'PASS' || { STATUS=FAIL; REASON="self-test did not pass"; }
[ "${FAIL_COUNT}" = "0" ] || { STATUS=FAIL; REASON="${FAIL_COUNT} check(s) failed"; }
[ -f "${SNAP_BIN}" ] || { STATUS=FAIL; REASON="snapshot missing"; }

# --------------------------------------------------------------------------
# 4. 快照 → PNG（复用真机同一条证据链）
# --------------------------------------------------------------------------
if [ -f "${SNAP_BIN}" ]; then
  SNAP_W=1024
  SNAP_H=600
  if [ -f "${SNAP_TXT}" ]; then
    SNAP_W=$(sed -n 's/^width=//p'  "${SNAP_TXT}" | head -1)
    SNAP_H=$(sed -n 's/^height=//p' "${SNAP_TXT}" | head -1)
  fi
  SNAP_W=${SNAP_W:-1024}
  SNAP_H=${SNAP_H:-600}

  if python3 "${HERE}/fb2png.py" "${SNAP_BIN}" "${SNAP_PNG}" \
       "${SNAP_W}" "${SNAP_H}" > "${RUN_DIR}/fb2png.log" 2>&1; then
    log "snapshot converted: ${SNAP_PNG}"
  else
    STATUS=FAIL
    REASON="${REASON:+${REASON}; }fb2png failed (see fb2png.log)"
  fi
else
  log "no snapshot to convert"
fi

# --------------------------------------------------------------------------
# 5. 汇总
# --------------------------------------------------------------------------
{
  echo "RESULT: ${STATUS}"
  echo "reason: ${REASON:-all assertions passed}"
  echo "ready:  ${READY_LINE}"
  echo "checks: ${PASS_COUNT} pass / ${FAIL_COUNT} fail"
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
