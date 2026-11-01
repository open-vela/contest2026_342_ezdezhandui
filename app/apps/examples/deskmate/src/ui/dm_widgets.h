/****************************************************************************
 * apps/examples/deskmate/src/ui/dm_widgets.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Small widget helpers shared by the DeskMate pages.
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_DESKMATE_UI_DM_WIDGETS_H
#define __APPS_EXAMPLES_DESKMATE_UI_DM_WIDGETS_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <lvgl/lvgl.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* A rounded, filled container (a "card"). */

lv_obj_t *dm_card(lv_obj_t *parent, int x, int y, int w, int h,
                  lv_color_t bg);

/* A card that stacks its children vertically with a small gap. */

lv_obj_t *dm_card_stack(lv_obj_t *parent, int x, int y, int w, int h,
                        lv_color_t bg);

/* Labels: the simple form appends to a flex parent, the positioned form
 * places the label explicitly.
 */

lv_obj_t *dm_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                   lv_color_t colour);
lv_obj_t *dm_label_at(lv_obj_t *parent, const char *text,
                      const lv_font_t *font, lv_color_t colour,
                      int x, int y);

/* printf into an existing label. */

void dm_label_set(lv_obj_t *label, const char *fmt, ...);

/* A push button with a centred label. */

lv_obj_t *dm_button(lv_obj_t *parent, const char *text, int w, int h,
                    lv_event_cb_t cb, void *user_data);

/* A transparent, full-size container used as a page root. */

lv_obj_t *dm_page_root(lv_obj_t *parent, int w, int h);

/* A small coloured status dot + text, e.g. "eth0 UP". */

lv_obj_t *dm_badge(lv_obj_t *parent, const char *text, lv_color_t colour);

#endif /* __APPS_EXAMPLES_DESKMATE_UI_DM_WIDGETS_H */
