#!/bin/bash
# ============================================================
# export.sh —— 把 openvela 工作区 nuttx/ 与 apps/ 的 esp32p4 改动
#              同步回作品仓文件树（board/esp32p4_vela/{nuttx,apps}）
#
# 开发流程：在工作区改代码 → 编译验证 → ./export.sh 同步回文件树 → commit 作品仓
#
# 用法:
#   ./export.sh [<openvela 工作区根目录>]
#       默认工作区 = $(pwd)（脚本位于 <工作区>/contest2026_342_ezdezhandui/board/esp32p4_vela/ 时）
#       也可显式传工作区根，如 ./export.sh /home/ez/share/openvela
#
# 同步范围：nuttx/apps 各自 上游 dev-ai-contest-2026..HEAD 的全部改动
#           （新增/修改复制；删除记入各自 .deleted-files）
# ============================================================
set -e

# ---- 定位工作区根 ----
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"          # .../board/esp32p4_vela
VELA_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"      # 作品仓根
if [ -n "$1" ]; then
  WS="$1"
else
  # 默认：脚本位于 <WS>/contest2026_342_ezdezhandui/board/esp32p4_vela
  WS="$(dirname "$(dirname "$(dirname "$SCRIPT_DIR")")")"
fi
echo "📍 工作区: $WS"

# ---- 同步一个仓的函数 ----
# sync_repo <repo名:nuttx|apps> <工作区路径> <目标文件树>
sync_repo() {
  local NAME="$1" REPO="$2" DST="$3"
  local BASE="refs/remotes/openvela/dev-ai-contest-2026"
  echo ""
  echo "━━━ 同步 $NAME ($BASE..HEAD) → $DST ━━━"
  if [ ! -d "$REPO/.git" ]; then
    echo "⚠️ 跳过：$REPO 不是 git 仓"
    return
  fi
  cd "$REPO"
  if ! git rev-parse --verify "$BASE" >/dev/null 2>&1; then
    echo "❌ 基线不存在: $BASE（先 git fetch openvela dev-ai-contest-2026）"
    return
  fi
  DELETED="$DST/.deleted-files"
  : > "$DELETED"
  local count=0
  local del_count=0
  while IFS=$'\t' read -r st path; do
    [ -z "$path" ] && continue
    # 跳过仓级文件（非源码改动）
    case "$path" in
      .gitignore|.gitmodules) continue ;;
    esac
    case "$st" in
      D)
        echo "$path" >> "$DELETED"
        rm -f "$DST/$path"
        echo "  ✂ 删除 $path"
        del_count=$((del_count+1))
        ;;
      A|M|R*|C*)
        mkdir -p "$(dirname "$DST/$path")"
        cp "$REPO/$path" "$DST/$path"
        echo "  + $path"
        count=$((count+1))
        ;;
    esac
  done < <(git diff --no-renames --name-status "$BASE"..HEAD)
  # 无删除文件时不保留空清单
  if [ "$del_count" -eq 0 ]; then
    rm -f "$DELETED"
  fi
  echo "  ✅ $NAME 同步 $count 文件"
}

sync_repo "nuttx" "$WS/nuttx" "$SCRIPT_DIR/nuttx"
sync_repo "apps"  "$WS/apps"  "$SCRIPT_DIR/apps"

echo ""
echo "✅ 全部同步完成"
echo "➡ 提交: cd $VELA_DIR && git add -A && git commit -m '...'"
