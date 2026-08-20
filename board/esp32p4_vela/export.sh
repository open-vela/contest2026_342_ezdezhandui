#!/bin/bash
# ============================================================
# export.sh —— 把 openvela 工作区 nuttx/ 的 esp32p4 改动同步回作品仓文件树
#
# 开发流程：在工作区 nuttx/ 里改代码 → 编译验证 → ./export.sh 同步回文件树 → commit 作品仓
#
# 用法:
#   ./export.sh                        # 默认读取 $(pwd)/nuttx，基线=上游 dev-ai-contest-2026
#   ./export.sh <工作区>/nuttx         # 指定 nuttx 路径
#   ./export.sh <nuttx> <基线commit>   # 指定 diff 基线（默认上游分支）
#
# 同步范围：基线..HEAD 的全部改动（新增/修改复制，删除记入 .deleted-files）
# ============================================================
set -e

DST="$(cd "$(dirname "$0")" && pwd)/nuttx"
NUTTX="${1:-$(pwd)/nuttx}"
BASE="${2:-refs/remotes/openvela/dev-ai-contest-2026}"

if [ ! -d "$NUTTX/.git" ]; then
  echo "❌ 不是 git 仓: $NUTTX（请在 openvela 工作区运行）"
  exit 1
fi

cd "$NUTTX"
if ! git rev-parse --verify "$BASE" >/dev/null 2>&1; then
  echo "❌ 基线不存在: $BASE（可传第二个参数指定 commit）"
  exit 1
fi

echo "📤 同步 $BASE..HEAD 改动 → $DST"
DELETED="$DST/.deleted-files"
: > "$DELETED"
count=0
while IFS=$'\t' read -r st path; do
  [ -z "$path" ] && continue
  case "$st" in
    D)
      echo "$path" >> "$DELETED"
      rm -f "$DST/$path"
      echo "  ✂ 删除 $path"
      ;;
    A|M|R*|C*)
      mkdir -p "$(dirname "$DST/$path")"
      cp "$NUTTX/$path" "$DST/$path"
      echo "  + $path"
      count=$((count+1))
      ;;
  esac
done < <(git diff --no-renames --name-status "$BASE"..HEAD)

echo "✅ 同步完成：$count 个文件，删除清单已更新（.deleted-files）"
echo "➡ 提交: cd $(dirname "$DST") && git add -A && git commit -m '...'"
