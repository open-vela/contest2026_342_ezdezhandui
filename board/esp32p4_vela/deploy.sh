#!/bin/bash
# ============================================================
# deploy.sh —— 把 board/esp32p4_vela/nuttx/ 文件树部署到 openvela 工作区
#
# 用法:
#   ./deploy.sh                     # 默认铺到 $(pwd)/nuttx
#   ./deploy.sh <工作区>/nuttx      # 指定 nuttx 路径
#
# 之后构建:
#   ./build.sh nuttx/boards/risc-v/esp32p4/esp32p4-function-ev-board/configs/nsh/ --cmake -j8
# ============================================================
set -e

SRC="$(cd "$(dirname "$0")" && pwd)/nuttx"
NUTTX="${1:-$(pwd)/nuttx}"

if [ ! -d "$NUTTX" ]; then
  echo "❌ nuttx 目录不存在: $NUTTX"
  echo "用法: ./deploy.sh <openvela 工作区>/nuttx   （默认当前目录下的 nuttx/）"
  exit 1
fi

echo "📦 部署文件树 → $NUTTX"
rsync -a --exclude=.deleted-files "$SRC/" "$NUTTX/"

# 处理移植中删除的文件（openvela 原 espressif 共享层文件被上游替换）
DELETED_FILE="$SRC/.deleted-files"
if [ -f "$DELETED_FILE" ]; then
  while IFS= read -r f; do
    [ -z "$f" ] && continue
    if [ -e "$NUTTX/$f" ]; then
      rm "$NUTTX/$f"
      echo "  ✂ 删除 $f"
    fi
  done < "$DELETED_FILE"
fi

echo "✅ 部署完成。请确认 HAL 锁定版本 ≥ 8d0a898（无 esp32p4 组件时：
   export ESP_HAL_3RDPARTY_VERSION=8d0a898910084206721a0892ab093021bca1496a）"
echo "➡ 构建: ./build.sh nuttx/boards/risc-v/esp32p4/esp32p4-function-ev-board/configs/nsh/ --cmake -j8"
