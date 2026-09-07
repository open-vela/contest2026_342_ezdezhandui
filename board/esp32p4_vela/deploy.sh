#!/bin/bash
# ============================================================
# deploy.sh —— 把 board/esp32p4_vela/{nuttx,apps}/ 文件树部署到 openvela 工作区
#
# 用法:
#   ./deploy.sh                     # 默认铺到 $(pwd)/nuttx 与 $(pwd)/apps
#   ./deploy.sh <openvela 工作区根>  # 指定工作区根目录
#
# 之后构建:
#   ./build.sh nuttx/boards/risc-v/esp32p4/esp32p4-function-ev-board/configs/nsh/ --cmake -j8
# ============================================================
set -e

SRC="$(cd "$(dirname "$0")" && pwd)"
if [ -n "$1" ]; then
  WS="$1"
else
  WS="$(pwd)"
fi

# ---- 部署单个文件树的函数 ----
# deploy_tree <repo名> <源子目录> <目标目录>
deploy_tree() {
  local NAME="$1" SUB="$2" DEST="$3"
  echo "📦 部署 $NAME 文件树 → $DEST"
  if [ ! -d "$DEST" ]; then
    echo "  ❌ 目标不存在: $DEST"
    return 1
  fi
  rsync -a --exclude=.deleted-files "$SRC/$SUB/" "$DEST/"

  # 处理删除文件清单
  local DELETED_FILE="$SRC/$SUB/.deleted-files"
  if [ -f "$DELETED_FILE" ]; then
    while IFS= read -r f; do
      [ -z "$f" ] && continue
      if [ -e "$DEST/$f" ]; then
        rm "$DEST/$f"
        echo "  ✂ 删除 $f"
      fi
    done < "$DELETED_FILE"
  fi
  echo "  ✅ $NAME 部署完成"
}

deploy_tree "nuttx" "nuttx" "$WS/nuttx"
if [ -d "$SRC/apps" ]; then
  deploy_tree "apps" "apps" "$WS/apps"
fi

echo ""
echo "✅ 全部部署完成。请确认 HAL 锁定版本 ≥ 8d0a898（无 esp32p4 组件时："
echo "   export ESP_HAL_3RDPARTY_VERSION=8d0a898910084206721a0892ab093021bca1496a）"
echo "➡ 构建: ./build.sh nuttx/boards/risc-v/esp32p4/esp32p4-function-ev-board/configs/nsh/ --cmake -j8"
