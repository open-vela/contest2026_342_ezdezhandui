#!/usr/bin/env bash
# =============================================================================
# sync_and_restore.sh —— 受控的 openvela 同步（默认 dry-run）
#
# 背景
#   nuttx 的 git 基线比工作区所用的 openvela drop 旧：`repo sync` 会把 90 个
#   tracked 改动**回退到 manifest revision**（≈ 降级 openvela），而本项目的
#   untracked 新文件（esp32p4/esp_hosted、esp32p4/drivers、板级 defconfig…）
#   虽然不会被删，但共享层被回退后构建会失败。
#
#   因此同步必须：先备份 tracked 改动 + 保护区内容 → 只同步白名单项目 →
#   用作品仓 <copyfile> 把我们的文件恢复回来 → 校验保护区逐字节一致 → 重建。
#
# 只同步白名单项目（apps 永不进入同步范围，模拟器 UI 的另一个对话不受影响）。
# =============================================================================
set -euo pipefail

WS="${OPENVELA_WS:-/home/ez/share/openvela}"
REPO_DIR="$WS/contest2026_342_ezdezhandui"
STAMP="$(date +%Y%m%d-%H%M%S)"
GUARD="/tmp/sync-guard/$STAMP"

# 只同步这两项；apps 永不同步（deskmate 在 apps 里）
SYNC_PROJECTS=( nuttx "vendor/espressif" )

# 只读保护区：另一条对话在开发模拟器 UI，任何同步/部署都不得触碰
PROTECTED=(
  "apps/examples/deskmate"
  "nuttx/drivers/net/wifi_sim.c"
  "nuttx/boards/sim/sim/sim/configs/deskmate"
)

APPLY=0
for a in "$@"; do
  case "$a" in
    --apply) APPLY=1 ;;
    -h|--help) sed -n '2,22p' "$0"; exit 0 ;;
    *) echo "未知参数: $a"; exit 2 ;;
  esac
done

say() { printf '%s\n' "$*"; }

# 保护区内容指纹
fingerprint() {
  local p="$1"
  if [ ! -e "$WS/$p" ]; then echo "MISSING"; return; fi
  if [ -f "$WS/$p" ]; then sha256sum "$WS/$p" | awk '{print $1}'; return; fi
  ( cd "$WS" && find "$p" -type f | sort | xargs -r sha256sum | sha256sum | awk '{print $1}' )
}

say "== 受控同步（$([ $APPLY -eq 1 ] && echo APPLY || echo DRY-RUN)）=="
say "   工作区: $WS"
say "   白名单项目: ${SYNC_PROJECTS[*]}"
say "   保护路径: ${PROTECTED[*]}"
say "   备份目录: $GUARD"
say ""

# 1) 断言：apps 不能有 tracked 改动（deskmate 全是 untracked，正常）
if [ -n "$(git -C "$WS/apps" status --porcelain --untracked-files=no 2>/dev/null)" ]; then
  say "⚠️  apps 仓有 tracked 改动：同步不会碰它（不在白名单），但请留意："
  git -C "$WS/apps" status --porcelain --untracked-files=no | head -5
fi

# 2) 记录保护区指纹
mkdir -p "$GUARD"
: > "$GUARD/protected.before"
for p in "${PROTECTED[@]}"; do
  printf '%s  %s\n' "$(fingerprint "$p")" "$p" >> "$GUARD/protected.before"
done
say "   [1/6] 保护区指纹已记录："
sed 's/^/         /' "$GUARD/protected.before"

# 3) 备份 tracked 改动 + untracked 清单
for proj in "${SYNC_PROJECTS[@]}"; do
  git -C "$WS/$proj" diff > "$GUARD/${proj//\//_}.tracked.patch" 2>/dev/null || true
  git -C "$WS/$proj" status --porcelain --untracked-files=all > "$GUARD/${proj//\//_}.status" 2>/dev/null || true
done
say "   [2/6] tracked 改动已备份到 $GUARD（*.tracked.patch）"

if [ $APPLY -eq 0 ]; then
  say ""
  say "   [3/6] 将执行: repo sync ${SYNC_PROJECTS[*]}   （dry-run，未执行）"
  say "   [4/6] 将用作品仓 copyfile 恢复我们的文件"
  say "   [5/6] 将逐字节校验保护区与同步前一致"
  say "   [6/6] 将执行构建 + 8 域回归"
  say ""
  say "   ✅ dry-run 结束；确认后用 --apply 真正执行"
  exit 0
fi

# 4) 真正的同步（只同步白名单项目；apps 不动）
say "   [3/6] repo sync ${SYNC_PROJECTS[*]}"
( cd "$WS" && repo sync -j4 "${SYNC_PROJECTS[@]}" )

# 5) 用作品仓恢复（按 manifest 的 copyfile 反向映射）
say "   [4/6] 部署作品仓文件树回工作区"
python3 - "$REPO_DIR" "$WS" <<'PY'
import os, sys, shutil, xml.etree.ElementTree as ET
repo, ws = sys.argv[1], sys.argv[2]
root = ET.parse(os.path.join(repo, "contest2026_342_ezdezhandui.xml")).getroot()
n = 0
for el in root.iter("copyfile"):
    src, dest = el.get("src"), el.get("dest")
    if not src or not dest:
        continue
    s, d = os.path.join(repo, src), os.path.join(ws, dest)
    if not os.path.isfile(s):
        continue
    os.makedirs(os.path.dirname(d), exist_ok=True)
    shutil.copy2(s, d)
    n += 1
print(f"        部署 {n} 个文件")
PY

# 6) 校验保护区
say "   [5/6] 保护区一致性校验"
: > "$GUARD/protected.after"
for p in "${PROTECTED[@]}"; do
  printf '%s  %s\n' "$(fingerprint "$p")" "$p" >> "$GUARD/protected.after"
done
if diff -u "$GUARD/protected.before" "$GUARD/protected.after"; then
  say "         ✅ 保护区逐字节未变"
else
  say "         ❌ 保护区被改动！请立刻用 $GUARD 回滚并检查同步白名单"
  exit 1
fi

# 7) 重建 + 回归提示
say "   [6/6] 重建主板"
( cd "$WS" && ./build.sh vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/configs/nsh --cmake -j8 )
say ""
say "   ✅ 同步 + 恢复 + 重建完成。下一步：8 域回归"
say "      cd $REPO_DIR && python3 tools/test/evidence.py --reset --seconds 400 --analyzer 400 --decode --tag post-sync"
