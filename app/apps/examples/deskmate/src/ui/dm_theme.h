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

#if defined(LV_FONT_MONTSERRAT_28) && LV_FONT_MONTSERRAT_28
#  define DM_FONT_TITLE (&lv_font_montserrat_28)
#else
#  define DM_FONT_TITLE LV_FONT_DEFAULT
#endif

#if defined(LV_FONT_MONTSERRAT_20) && LV_FONT_MONTSERRAT_20
#  define DM_FONT_BODY (&lv_font_montserrat_20)
#else
#  define DM_FONT_BODY LV_FONT_DEFAULT
#endif

#define DM_FONT_SMALL LV_FONT_DEFAULT

#define DM_MARGIN 16
#define DM_STATUS_H 56
#define DM_TABBAR_H 56

#endif /* __APPS_EXAMPLES_DESKMATE_UI_DM_THEME_H */
