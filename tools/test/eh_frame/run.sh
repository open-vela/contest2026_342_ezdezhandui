#!/bin/bash
# run.sh - 在主机上编译并运行 esp-hosted 帧层的往返一致性测试
#
# 为什么值得有这么一个测试：第 2 层（帧层）是纯逻辑，把它的正确性验出来
# **不需要 C6、不需要 SDIO、不需要板子**。这样一来，"帧格式实现对不对"和
# "C6 是否应答"两件事就解耦了 —— 前者随时可以拿到证据。
#
# 编译的是 **上游 eh_frame.c 原文件**（一行未改），只为它补三个最小 shim：
# esp_err.h / linux/types.h / sdkconfig.h。
#
# 用法:
#   tools/test/eh_frame/run.sh [esp-hosted-mcu 路径]
#   默认找 $EH_MCU 或 /tmp/esp-hosted-mcu

set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
EH="${1:-${EH_MCU:-/tmp/esp-hosted-mcu}}"
SRC="$EH/common/eh_frame/src/eh_frame.c"

if [ ! -f "$SRC" ]; then
  echo "找不到上游帧层源码: $SRC" >&2
  echo "请先 clone espressif/esp-hosted-mcu，或把它作为第一个参数传入。" >&2
  exit 1
fi

OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

INC="-I $DIR/shim -I $EH/common/eh_tlv/include -I $EH/common/eh_frame/include -I $EH/common/eh_common/include"

echo "上游: $SRC"
gcc -c -std=gnu11 $INC "$SRC" -o "$OUT/eh_frame.o"
gcc -std=gnu11 $INC "$DIR/eh_frame_roundtrip.c" "$OUT/eh_frame.o" -o "$OUT/rt"
"$OUT/rt"
