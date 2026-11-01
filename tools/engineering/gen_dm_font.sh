#!/usr/bin/env bash
# tools/engineering/gen_dm_font.sh —— 生成 DeskMate 的 CJK 子集字体
#
# 为什么需要子集：LVGL 内置的 SimSun 16 CJK 约 149 KB，全量装载吃 Flash；
# 而 UI 实际只用几百个汉字。本脚本从应用唯一的文案表
# `apps/examples/deskmate/src/ui/dm_i18n.h` 里抽出全部字符串，按字符集生成
# 一个 16px / 4bpp 的子集字体 C 文件（≈40–60 KB）。
#
#   tools/engineering/gen_dm_font.sh              # 生成到应用目录
#   tools/engineering/gen_dm_font.sh --check      # 只列出字符集与统计，不生成
#
# 依赖：node + npx（首次会拉取 lv_font_conv）。离线时改用 LVGL 内置字体：
#   在板级/模拟器 defconfig 里打开 CONFIG_EXAMPLES_DESKMATE_FONT_SIMSUN=y
#   （select LV_FONT_SIMSUN_16_CJK，约 +149 KB）。
#
# 生成后：在应用里把 DM_FONT_BODY/DM_FONT_SMALL 指向 `dm_font_16`，
# 并把 dm_i18n.h 的文案换回中文即可。

set -u -o pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${HERE}/../.." && pwd)"

I18N="${DM_I18N:-${REPO}/app/apps/examples/deskmate/src/ui/dm_i18n.h}"
OUT="${DM_FONT_OUT:-${REPO}/app/apps/examples/deskmate/fonts/dm_font_16.c}"
SIZE=16
BPP=4
SYMBOLS=""

CHECK=0
[ "${1:-}" = "--check" ] && CHECK=1

if [ ! -f "${I18N}" ]; then
  echo "找不到文案表：${I18N}" >&2
  exit 2
fi

# 1. 抽取字符集：把 dm_i18n.h 里的 C 字符串字面量拼起来去重排序。
SYMBOLS="$(python3 - "${I18N}" <<'PY'
import re, sys
src = open(sys.argv[1], encoding='utf-8').read()
# 去掉注释，避免把注释里的示例文字算进去
src = re.sub(r'/\*.*?\*/', '', src, flags=re.S)
src = re.sub(r'//[^\n]*', '', src)
chars = set()
for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', src):
    for ch in lit.encode('utf-8').decode('unicode_escape').encode('latin-1', 'ignore').decode('utf-8', 'ignore'):
        chars.add(ch)
# 只保留非 ASCII（ASCII 由 montserrat 提供），再加上常用标点与空格
keep = {c for c in chars if ord(c) > 0x7f}
keep |= set(' ')
print(''.join(sorted(keep)))
PY
)"

COUNT="$(python3 -c "import sys;print(len(sys.argv[1]))" "${SYMBOLS}")"
echo "[dm-font] 文案表      : ${I18N}"
echo "[dm-font] 抽取字符数  : ${COUNT}"

if [ "${CHECK}" = "1" ]; then
  echo "[dm-font] --check：不生成文件。字符集："
  echo "${SYMBOLS}"
  exit 0
fi

if ! command -v npx >/dev/null 2>&1; then
  echo "[dm-font] 没有 npx，无法生成子集字体。" >&2
  echo "[dm-font] 回退方案：defconfig 里打开 CONFIG_EXAMPLES_DESKMATE_FONT_SIMSUN=y" >&2
  exit 3
fi

mkdir -p "$(dirname "${OUT}")"

echo "[dm-font] 生成 ${SIZE}px / ${BPP}bpp 子集 → ${OUT}"
npx --yes lv_font_conv \
  --font "${DM_TTF:-/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc}" \
  --symbols "${SYMBOLS}" \
  --size "${SIZE}" \
  --bpp "${BPP}" \
  --format lvgl \
  --no-compress \
  --lv-include lvgl.h \
  -o "${OUT}"

echo "[dm-font] 完成：$(wc -c < "${OUT}") 字节"
echo "[dm-font] 下一步：把 DM_FONT_BODY/DM_FONT_SMALL 指向 dm_font_16 并切回中文文案"
