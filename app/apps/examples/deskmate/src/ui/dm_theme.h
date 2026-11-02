/****************************************************************************
 * apps/examples/deskmate/src/ui/dm_theme.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Colours and font choices shared by every DeskMate page.
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

#define DM_COL_BG        0x0f1319
#define DM_COL_CARD      0x1b2230
#define DM_COL_CARD2     0x222c3d
#define DM_COL_ACCENT    0x4c9aff
#define DM_COL_OK        0x48d597
#define DM_COL_WARN      0xf3b23c
#define DM_COL_ERR       0xf2685c
#define DM_COL_TEXT      0xe8eef6
#define DM_COL_MUTED     0x8d9bb0
#define DM_COL_ON_ACCENT 0x08131f

/* 中文子集字体（tools/engineering/gen_dm_font.sh 生成，见 fonts/）。
 * 两档：24px 用于标题/时钟，16px 用于正文与标签。
 * 需要退回内置大字库时：开 CONFIG_EXAMPLES_DESKMATE_FONT_SIMSUN，
 * 并把下面三行换成 &lv_font_simsun_16_cjk。
 */

#if defined(CONFIG_EXAMPLES_DESKMATE_FONT_SIMSUN)
LV_FONT_DECLARE(lv_font_simsun_16_cjk)
#  define DM_FONT_TITLE (&lv_font_simsun_16_cjk)
#  define DM_FONT_BODY  (&lv_font_simsun_16_cjk)
#  define DM_FONT_SMALL (&lv_font_simsun_16_cjk)
#else
LV_FONT_DECLARE(dm_font_24)
LV_FONT_DECLARE(dm_font_16)

#  define DM_FONT_TITLE (&dm_font_24)
#  define DM_FONT_BODY  (&dm_font_16)
#  define DM_FONT_SMALL (&dm_font_16)
#endif

#define DM_MARGIN 16
#define DM_STATUS_H 56
#define DM_TABBAR_H 56

#endif /* __APPS_EXAMPLES_DESKMATE_UI_DM_THEME_H */
