#!/usr/bin/env bash
# tools/engineering/gen_dm_font.sh —— 生成 DeskMate 的中文子集字体
#
# 为什么需要子集：LVGL 内置的 SimSun 16 CJK 约 149 KB 且只有 16px 一档；UI 实际
# 只用几百个汉字。本脚本扫描应用里的**全部字符串字面量**（文案表 + 页面 + 主程序），
# 按实际字形生成 16px / 24px 两档子集字体，产物入仓，复现者不需要 node。
#
#   tools/engineering/gen_dm_font.sh                      # 生成到作品仓 app 目录
#   tools/engineering/gen_dm_font.sh --app <应用目录>      # 指定 app 目录（工作区用）
#   tools/engineering/gen_dm_font.sh --check              # 只统计字形与体积，不生成
#
# 源字体：单 face 的 CJK TTF（lv_font_conv / fontkit **不支持 TTC 集合**）。
#   默认 /usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf，可用 --ttf / DM_TTF 覆盖。
#   若机器上只有 TTC（如 Noto Sans CJK），先用 fonttools 拆出单 face 再用。
#
# 依赖：node + npx（首次会拉取 lv_font_conv）。离线回退：
#   在 defconfig 打开 CONFIG_EXAMPLES_DESKMATE_FONT_SIMSUN=y（内置 SimSun 16，+149 KB）。

set -u -o pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${HERE}/../.." && pwd)"

APP_DIR="${REPO}/app/apps/examples/deskmate"
# 两个源字体：拉丁字母/数字来自 DejaVuSans（DroidSansFallback 是**纯 CJK 回退字体，
# 不含 ASCII**——单一字体方案会让所有字母/数字缺字，UI 上画出方框）。
TTF_ASCII="${DM_TTF_ASCII:-/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf}"
TTF_CJK="${DM_TTF_CJK:-/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf}"
CJK_COMMON="${DM_CJK_COMMON:-1}"   # 1 = 正文档并入常用汉字表（修运行时中文乱码）
SIZES="16 24"
BPP=4
CHECK=0

while [ $# -gt 0 ]; do
  case "$1" in
    --app)   shift; APP_DIR="${1:-${APP_DIR}}" ;;
    --ttf)   shift; TTF_ASCII="${1:-${TTF_ASCII}}"; TTF_CJK="${1:-${TTF_CJK}}" ;;
    --ttf-ascii) shift; TTF_ASCII="${1:-${TTF_ASCII}}" ;;
    --ttf-cjk)   shift; TTF_CJK="${1:-${TTF_CJK}}" ;;
    --sizes) shift; SIZES="${1:-${SIZES}}" ;;
    --cjk-common) shift; CJK_COMMON="${1:-1}" ;;
    --body-size)  shift; BODY_SIZE="${1:-20}" ;;
    --check) CHECK=1 ;;
    -h|--help) sed -n '2,19p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

I18N="${APP_DIR}/src/ui/dm_i18n.h"
FONT_DIR="${APP_DIR}/fonts"

if [ ! -f "${I18N}" ]; then
  echo "找不到文案表：${I18N}" >&2
  exit 2
fi

for f in "${TTF_ASCII}" "${TTF_CJK}"; do
  if [ ! -f "$f" ]; then
    echo "找不到源字体：$f" >&2
    echo "用 --ttf-ascii / --ttf-cjk 指定；TTC 集合不被 lv_font_conv 支持。" >&2
    exit 3
  fi
done

# 1) 扫描全部源文件里的字符串字面量，取字形并集 -------------------------------
#    · 文案表 dm_i18n.h
#    · 页面与主程序里的 UI 字面量（预设短语等）
#    · 固定并入 ASCII 可见字符与中文常用标点（数字/时间/百分比/单位要用）
#    工程标识（[dm-check] 键名、DM-* 标记、NSH 命令）也会进来，但都是 ASCII，成本可忽略。

SYMBOLS="$(python3 - "${APP_DIR}" <<'PY'
import glob, os, re, sys

app = sys.argv[1]
files = [os.path.join(app, 'deskmate_main.c')]
files += glob.glob(os.path.join(app, 'src', '**', '*.c'), recursive=True)
files += glob.glob(os.path.join(app, 'src', '**', '*.h'), recursive=True)

LIT = re.compile(r'"((?:[^"\\]|\\.)*)"')
chars = set()
for path in files:
    try:
        src = open(path, encoding='utf-8').read()
    except (UnicodeDecodeError, IsADirectoryError):
        continue
    src = re.sub(r'/\*.*?\*/', '', src, flags=re.S)   # 块注释
    src = re.sub(r'//[^\n]*', '', src)               # 行注释
    for lit in LIT.findall(src):
        lit = (lit.replace('\\n', '\n').replace('\\t', '\t')
                  .replace('\\"', '"').replace('\\\\', '\\'))
        for ch in lit:
            if ch not in '\n\t\r\x00':
                chars.add(ch)

chars |= set('，。、：；！？（）【】《》“”‘’—…·％＋－')

# 只输出非 ASCII：ASCII 用 lv_font_conv 的 --range 传入。若把 ASCI（含 \ " $ ` 等）
# 塞进 --symbols，工具会把这些字符当成转义/特殊符号吞掉，导致个别字母缺字
# （实测 'o'、'0' 等 8 个字母没有轮廓，UI 上会画出方框）。
print(''.join(sorted(c for c in chars if ord(c) > 0x7f)))
PY
)"

# 1b) 常用汉字表：运行时文本（LLM 回复 / SSID / 天气 / 用户输入）不在字符串字面量里，
#     只按字面量做子集会把这些字画成方框（真机"中文乱码"的根因）。
#     用 Python 内置 GB2312 编码现算一级汉字（3755 字），无需外部数据文件；
#     只并入【正文档】（BODY_SIZE，默认 20px），标题 32px 仍用字面子集控体积。

COMMON_CJK="$(python3 - <<'PYEOF'
chars = set()
for hi in range(0xb0, 0xd8):          # GB2312 一级汉字区（3755 字）
    for lo in range(0xa1, 0xff):
        try:
            chars.add(bytes((hi, lo)).decode('gb2312'))
        except UnicodeDecodeError:
            continue
print(''.join(sorted(chars)))
PYEOF
)"

BODY_SIZE="${BODY_SIZE:-20}"
if [ "${CJK_COMMON}" = "1" ]; then
  SYMBOLS_BODY="${SYMBOLS}${COMMON_CJK}"
else
  SYMBOLS_BODY="${SYMBOLS}"
fi

COUNT="$(python3 -c "import sys;print(len(sys.argv[1]) + 95)" "${SYMBOLS}")"
CJK_COUNT="$(python3 -c "import sys;print(len(sys.argv[1]))" "${SYMBOLS}")"

echo "[dm-font] app 目录    : ${APP_DIR}"
echo "[dm-font] ASCII 源字体: ${TTF_ASCII}"
echo "[dm-font] CJK   源字体: ${TTF_CJK}"
echo "[dm-font] 字形总数    : ${COUNT}（非 ASCII ${CJK_COUNT} + ASCII 0x20-0x7E）"
echo "[dm-font] 档位        : ${SIZES}px / ${BPP}bpp"

if [ "${CHECK}" = "1" ]; then
  echo "[dm-font] --check：不生成文件。非 ASCII 字形清点："
  echo "${SYMBOLS}"
  exit 0
fi

if ! command -v npx >/dev/null 2>&1; then
  echo "[dm-font] 没有 npx，无法生成子集字体。" >&2
  echo "[dm-font] 回退：defconfig 打开 CONFIG_EXAMPLES_DESKMATE_FONT_SIMSUN=y" >&2
  exit 4
fi

mkdir -p "${FONT_DIR}"

# 2) 逐档生成；命名固定 dm_font_<size>，应用侧按名字声明 -----------------------
for SZ in ${SIZES}; do
  OUT="${FONT_DIR}/dm_font_${SZ}.c"

  if [ "${SZ}" = "${BODY_SIZE}" ]; then
    USE_SYMBOLS="${SYMBOLS_BODY}"
    echo "[dm-font] 生成 ${SZ}px（正文档：并入常用汉字表）→ ${OUT}"
  else
    USE_SYMBOLS="${SYMBOLS}"
    echo "[dm-font] 生成 ${SZ}px（字面子集）→ ${OUT}"
  fi
  if ! npx --yes lv_font_conv \
        --font "${TTF_ASCII}" \
        --range 0x20-0x7e \
        --font "${TTF_CJK}" \
        --symbols "${USE_SYMBOLS}" \
        --size "${SZ}" \
        --bpp "${BPP}" \
        --format lvgl \
        --lv-font-name "dm_font_${SZ}" \
        --no-compress \
        --lv-include lvgl.h \
        -o "${OUT}"; then
    echo "[dm-font] 生成失败（${SZ}px）。若是 TTC 集合，请先拆成单 face TTF。" >&2
    exit 5
  fi
  echo "[dm-font]   $(wc -c < "${OUT}") 字节 C 源"
done

echo "[dm-font] 完成。下一步：dm_theme.h 里把 DM_FONT_TITLE/BODY/SMALL 指到 dm_font_*"
