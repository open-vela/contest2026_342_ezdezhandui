#!/bin/bash
# ============================================================
# export.sh —— 把 openvela 工作区 nuttx/、apps/、packages/ai_agent 的
#              esp32p4 改动同步回作品仓文件树（board/esp32p4_vela/{nuttx,apps,ai_agent}）
#
# 开发流程：在工作区改代码 → 编译验证 → ./export.sh 同步回文件树 → commit 作品仓
#
# 用法:
#   ./export.sh [<openvela 工作区根目录>]
#       默认工作区 = $(pwd)（脚本位于 <工作区>/contest2026_342_ezdezhandui/board/esp32p4_vela/ 时）
#       也可显式传工作区根，如 ./export.sh /home/ez/share/openvela
#
# 同步范围：
#   - nuttx/apps：各自 上游 dev-ai-contest-2026..HEAD 的全部提交改动
#   - ai_agent ：工作区未提交 diff（git diff HEAD；含新增/修改/删除）
#     （新增/修改复制；删除记入各自 .deleted-files）
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

# ---- 同步"提交式"仓（git diff BASE..HEAD）----
# sync_committed <repo名> <工作区路径> <目标文件树>
sync_committed() {
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
  sync_diff "$NAME" "$REPO" "$DST" < <(git diff --no-renames --name-status "$BASE"..HEAD)
}

# ---- 同步"工作区 diff"仓（git diff HEAD，未提交）----
# sync_worktree <repo名> <工作区路径> <目标文件树>
sync_worktree() {
  local NAME="$1" REPO="$2" DST="$3"
  echo ""
  echo "━━━ 同步 $NAME (工作区 diff vs HEAD) → $DST ━━━"
  if [ ! -d "$REPO/.git" ]; then
    echo "⚠️ 跳过：$REPO 不是 git 仓"
    return
  fi
  cd "$REPO"
  # 含 untracked? 不加 --no-renames 输出会含状态; 用 --no-renames 统一
  sync_diff "$NAME" "$REPO" "$DST" < <(git status --porcelain 2>/dev/null | sed 's/^\(..\) /\1\t/')
}

# ---- 通用 diff 应用 ----
# 输入行: <状态>\t<path>  (porcelain 两字母: XY; name-status 单字母)
# porcelain 中 X=index 状态 Y=worktree 状态, 空格表示未变; 取非空格字母
sync_diff() {
  local NAME="$1" REPO="$2" DST="$3"
  mkdir -p "$DST"
  DELETED="$DST/.deleted-files"
  # 删除清单先写临时文件，确认本次算出了内容才覆盖既有清单——避免 BASE 取错
  # 或分支处于 detached 时（repo sync 后常见）空 diff 把既有清单静默清空。
  local TMP_DELETED
  TMP_DELETED="$(mktemp)"
  local count=0 del_count=0
  while IFS=$'\t' read -r st path; do
    [ -z "$path" ] && continue
    # 忽略 untracked(??) 与 ignored(!!)
    case "$st" in
      "??"|"!!") continue ;;
    esac
    # 取非空格状态字母（porcelain XY; 空格=该侧未变）
    st="${st// /}"
    st="${st:0:1}"
    # 跳过仓级/构建产物文件
    case "$path" in
      .gitignore|.gitmodules|.built|.depend) continue ;;
    esac
    case "$st" in
      D)
        echo "$path" >> "$TMP_DELETED"
        rm -f "$DST/$path"
        echo "  ✂ 删除 $path"
        del_count=$((del_count+1))
        ;;
      A|M|R|C)
        mkdir -p "$(dirname "$DST/$path")"
        cp "$REPO/$path" "$DST/$path"
        echo "  + $path"
        count=$((count+1))
        ;;
    esac
  done

  # 空 diff：极可能 BASE/分支不对 → 不动文件树也不动既有清单，只告警
  if [ "$count" -eq 0 ] && [ "$del_count" -eq 0 ]; then
    rm -f "$TMP_DELETED"
    echo "  ⚠ $NAME: diff 为空（请检查 $REPO 的分支与 BASE），保留既有 .deleted-files" >&2
    return 0
  fi

  if [ "$del_count" -gt 0 ]; then
    mv "$TMP_DELETED" "$DELETED"
  else
    rm -f "$TMP_DELETED"
    if [ -s "$DELETED" ]; then
      # 删除项已在更早提交里体现：保留既有清单，不误清
      echo "  ⚠ $NAME: 本次无删除项但既有 .deleted-files 非空 → 保留" >&2
    else
      rm -f "$DELETED"
    fi
  fi
  echo "  ✅ $NAME 同步 $count 文件"
}

sync_committed "nuttx" "$WS/nuttx" "$SCRIPT_DIR/nuttx"
sync_committed "apps"  "$WS/apps"  "$SCRIPT_DIR/apps"
sync_worktree "ai_agent" "$WS/packages/ai_agent" "$SCRIPT_DIR/ai_agent"

echo ""
echo "✅ 全部同步完成"
echo "➡ 提交: cd $VELA_DIR && git add -A && git commit -m '...'"
