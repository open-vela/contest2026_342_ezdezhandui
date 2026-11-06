/****************************************************************************
 * apps/examples/deskmate/src/ui/dm_widgets.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Small widget helpers shared by the DeskMate pages.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <lvgl/lvgl.h>

#include "ui/dm_theme.h"
#include "ui/dm_widgets.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

lv_obj_t *dm_card(lv_obj_t *parent, int x, int y, int w, int h,
                  lv_color_t bg)
{
  lv_obj_t *card = lv_obj_create(parent);

  lv_obj_remove_style_all(card);
  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, w, h);
  lv_obj_set_style_bg_color(card, bg, 0);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(card, 14, 0);
  lv_obj_set_style_border_width(card, 1, 0);
  lv_obj_set_style_border_color(card, lv_color_hex(DM_COL_BORDER), 0);
  lv_obj_set_style_pad_all(card, 18, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  return card;
}

lv_obj_t *dm_card_stack(lv_obj_t *parent, int x, int y, int w, int h,
                        lv_color_t bg)
{
  lv_obj_t *card = dm_card(parent, x, y, w, h, bg);

  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(card, 10, 0);

  return card;
}

lv_obj_t *dm_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                   lv_color_t colour)
{
  lv_obj_t *label = lv_label_create(parent);

  lv_label_set_text(label, text != NULL ? text : "");
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, colour, 0);

  return label;
}

lv_obj_t *dm_label_at(lv_obj_t *parent, const char *text,
                      const lv_font_t *font, lv_color_t colour,
                      int x, int y)
{
  lv_obj_t *label = dm_label(parent, text, font, colour);

  lv_obj_set_pos(label, x, y);

  return label;
}

void dm_label_set(lv_obj_t *label, const char *fmt, ...)
{
  char text[256];
  va_list ap;
  const char *cur;

  if (label == NULL || fmt == NULL)
    {
      return;
    }

  va_start(ap, fmt);
  vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);

  /* **文字没变就一个字都别写。**
   *
   * lv_label_set_text() 是无条件失效的：即使新字符串与当前内容完全相同，它
   * 也会把 label 标脏。于是每轮 dm_ui_tick() 都会产生脏区 ⇒ LVGL 每轮都要真
   * 重绘 ⇒ fbdev 还要按 area->h * stride 逐行回写 PSRAM。静态页面上这一条就
   * 足以把主循环从 ~2ms 拖到 ~20ms，并且让"提交数 ≡ 循环数"（真机实测：
   * 15.03 帧/s、>100ms 帧间隔 1.9 次/s）。
   *
   * 顶栏时钟（dm_ui.c）和 agent 徽标早就各自加了 strcmp 缓存，注释里也记着
   * 实测后果（单次刷写 206k 像素、忙时 140ms）——这里把同一个护栏下沉到公共
   * 入口，所有页面一次性受益，免得再漏。
   *
   * 与当前文本比对（而不是缓存上一次入参）：即使别处用
   * lv_label_set_text_fmt() 直接改过这个 label，这里也不会误判。
   */

  cur = lv_label_get_text(label);

  if (cur != NULL && strcmp(cur, text) == 0)
    {
      return;
    }

  lv_label_set_text(label, text);
}

lv_obj_t *dm_button(lv_obj_t *parent, const char *text, int w, int h,
                    lv_event_cb_t cb, void *user_data)
{
  lv_obj_t *btn = lv_button_create(parent);

  lv_obj_set_size(btn, w, h);
  lv_obj_set_style_bg_color(btn, lv_color_hex(DM_COL_ACCENT), 0);
  lv_obj_set_style_radius(btn, 10, 0);

  if (cb != NULL)
    {
      lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }

  {
    lv_obj_t *label = dm_label(btn, text, DM_FONT_BODY,
                               lv_color_hex(DM_COL_ON_ACCENT));

    lv_obj_center(label);
  }

  return btn;
}

lv_obj_t *dm_page_root(lv_obj_t *parent, int w, int h)
{
  lv_obj_t *root = lv_obj_create(parent);

  lv_obj_remove_style_all(root);
  lv_obj_set_pos(root, 0, 0);
  lv_obj_set_size(root, w, h);
  lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
  lv_obj_set_style_pad_all(root, DM_MARGIN, 0);
  lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

  return root;
}

lv_obj_t *dm_badge(lv_obj_t *parent, const char *text, lv_color_t colour)
{
  lv_obj_t *badge = lv_label_create(parent);

  lv_label_set_text(badge, text != NULL ? text : "");
  lv_obj_set_style_text_font(badge, DM_FONT_SMALL, 0);
  lv_obj_set_style_text_color(badge, colour, 0);
  lv_obj_set_style_bg_color(badge, colour, 0);
  lv_obj_set_style_bg_opa(badge, LV_OPA_20, 0);
  lv_obj_set_style_radius(badge, 8, 0);
  lv_obj_set_style_pad_hor(badge, 10, 0);
  lv_obj_set_style_pad_ver(badge, 4, 0);

  return badge;
}
