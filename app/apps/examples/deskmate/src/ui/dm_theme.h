/****************************************************************************
 * apps/examples/deskmate/src/ui/dm_theme.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * DeskMate 的配色与字体。
 *
 * 2026-11-02 改为**浅色高对比主题**：真机 7" 1024x600 面板上，之前的深色卡片
 * （#1b2230 on #0f1319）从正常观看距离看就是"一片深蓝、看不到内容"。现在用
 * 浅灰底 + 白卡片 + 深色正文 + 蓝色强调，正文 20px、标题 32px。
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_DESKMATE_UI_DM_THEME_H
#define __APPS_EXAMPLES_DESKMATE_UI_DM_THEME_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <lvgl/lvgl.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* 颜色：浅色底、白卡片、深色正文、蓝色强调 */

#define DM_COL_BG        0xeef2f7
#define DM_COL_CARD      0xffffff
#define DM_COL_CARD2     0xf6f9fc
#define DM_COL_ACCENT    0x1f6feb
#define DM_COL_ACCENT_DK 0x1452b8
#define DM_COL_OK        0x1a9f5a
#define DM_COL_WARN      0xc77700
#define DM_COL_ERR       0xd0342c
#define DM_COL_TEXT      0x16202e
#define DM_COL_MUTED     0x5a6b82
#define DM_COL_ON_ACCENT 0xffffff
#define DM_COL_BORDER    0xd7dee8

/* 字体：自绘中文子集（gen_dm_font.sh 生成），20px 正文 / 32px 标题。
 *
 * 需要退回内置大字库时：开 CONFIG_EXAMPLES_DESKMATE_FONT_SIMSUN，
 * 或用 --sizes 重新生成别的档位。
 */

#if defined(CONFIG_EXAMPLES_DESKMATE_FONT_SIMSUN)
LV_FONT_DECLARE(lv_font_simsun_16_cjk)
#  define DM_FONT_TITLE (&lv_font_simsun_16_cjk)
#  define DM_FONT_BODY  (&lv_font_simsun_16_cjk)
#  define DM_FONT_SMALL (&lv_font_simsun_16_cjk)
#else
LV_FONT_DECLARE(dm_font_32)
LV_FONT_DECLARE(dm_font_20)

#  define DM_FONT_TITLE (&dm_font_32)
#  define DM_FONT_BODY  (&dm_font_20)
#  define DM_FONT_SMALL (&dm_font_20)
#endif

/* 版式（1024x600：状态栏 72 / 标签栏 72 / 边距 20） */

#define DM_MARGIN    20
#define DM_STATUS_H  72
#define DM_TABBAR_H  72

#endif /* __APPS_EXAMPLES_DESKMATE_UI_DM_THEME_H */
