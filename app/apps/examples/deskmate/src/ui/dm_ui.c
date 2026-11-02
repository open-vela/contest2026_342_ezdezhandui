/****************************************************************************
 * apps/examples/deskmate/src/ui/dm_ui.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The DeskMate shell: status bar, page area, tab bar.
 *
 * Everything happens on the LVGL thread (the application's single loop), so no
 * widget is ever touched from another thread.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <lvgl/lvgl.h>

#include "agent/dm_agent.h"
#include "dm_config.h"
#include "dm_log.h"
#include "hal/dm_display.h"
#include "ui/dm_i18n.h"
#include "ui/dm_page.h"
#include "ui/dm_theme.h"
#include "ui/dm_ui.h"
#include "ui/dm_widgets.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define DM_PAGE_COUNT 5
#define DM_TOAST_TICKS 40          /* UI ticks a toast stays visible */

/****************************************************************************
 * Private Data
 ****************************************************************************/

static dm_page_t *g_pages[DM_PAGE_COUNT] =
{
  &dm_page_home,
  &dm_page_camera,
  &dm_page_chat,
  &dm_page_net,
  &dm_page_settings,
};

static int g_tab_index[DM_PAGE_COUNT] = { 0, 1, 2, 3, 4 };

static lv_obj_t *g_screen;
static lv_obj_t *g_content;
static lv_obj_t *g_clock;
static lv_obj_t *g_agent_badge;
static lv_obj_t *g_page_title;
static lv_obj_t *g_tabbar;
static lv_obj_t *g_toast;
static int g_current = -1;
static int g_content_w;
static int g_content_h;
static int g_toast_ticks;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void dm_ui_style_tab(lv_obj_t *btn, bool active)
{
  lv_obj_set_style_bg_color(btn, lv_color_hex(active ? DM_COL_ACCENT :
                                              DM_COL_CARD), 0);
  lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);

  {
    lv_obj_t *label = lv_obj_get_child(btn, 0);

    if (label != NULL)
      {
        lv_obj_set_style_text_color(label, lv_color_hex(active ?
                                                        DM_COL_ON_ACCENT :
                                                        DM_COL_TEXT), 0);
      }
  }
}

static void dm_ui_tab_cb(lv_event_t *event)
{
  int index = *(int *)lv_event_get_user_data(event);

  if (lv_event_get_code(event) != LV_EVENT_CLICKED)
    {
      return;
    }

  (void)dm_ui_goto(index);
}

static const char *dm_ui_agent_label(void)
{
  const char *state = dm_agent_state();

  if (strcmp(state, "off") == 0)
    {
      return DM_I18N_AGENT_OFF;
    }

  if (strcmp(state, "mock") == 0)
    {
      return DM_I18N_AGENT_MOCK;
    }

  if (strcmp(state, "online") == 0)
    {
      return DM_I18N_AGENT_ONLINE;
    }

  return DM_I18N_AGENT_OFFLINE;
}

static void dm_ui_set_clock(void)
{
  time_t now = time(NULL);
  struct tm tm_buf;
  char text[32];

  if (g_clock == NULL || localtime_r(&now, &tm_buf) == NULL)
    {
      return;
    }

  if (strftime(text, sizeof(text), "%H:%M:%S", &tm_buf) > 0)
    {
      lv_label_set_text(g_clock, text);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int dm_ui_create(void)
{
  const struct dm_display_metrics_s *metrics = dm_display_metrics();
  int width;
  int height;
  int i;

  if (metrics == NULL)
    {
      DM_ERR("ui: display not ready");
      return -1;
    }

  width  = metrics->width;
  height = metrics->height;

  g_content_w = width;
  g_content_h = height - DM_STATUS_H - DM_TABBAR_H;
  if (g_content_h < 120)
    {
      g_content_h = height - DM_STATUS_H;
    }

  g_screen = lv_screen_active();
  lv_obj_remove_style_all(g_screen);
  lv_obj_set_style_bg_color(g_screen, lv_color_hex(DM_COL_BG), 0);
  lv_obj_set_style_bg_opa(g_screen, LV_OPA_COVER, 0);

  /* Status bar. */

  {
    lv_obj_t *bar = lv_obj_create(g_screen);

    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, width, DM_STATUS_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(DM_COL_CARD), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);

    {
      lv_obj_t *title = dm_label_at(bar, DM_I18N_APP_TITLE, DM_FONT_BODY,
                                    lv_color_hex(DM_COL_TEXT), DM_MARGIN, 16);

      (void)title;
    }

    g_page_title = dm_label_at(bar, "", DM_FONT_BODY,
                               lv_color_hex(DM_COL_ACCENT), 0, 16);
    lv_obj_align(g_page_title, LV_ALIGN_CENTER, 0, 0);

    g_agent_badge = dm_badge(bar, DM_I18N_AGENT_STATE "：-",
                             lv_color_hex(DM_COL_MUTED));
    lv_obj_align(g_agent_badge, LV_ALIGN_RIGHT_MID, -170, 0);

    g_clock = dm_label_at(bar, "--:--:--", DM_FONT_BODY,
                          lv_color_hex(DM_COL_ACCENT), 0, 16);
    lv_obj_align(g_clock, LV_ALIGN_RIGHT_MID, -DM_MARGIN, 0);
  }

  /* Page area. */

  g_content = lv_obj_create(g_screen);
  lv_obj_remove_style_all(g_content);
  lv_obj_set_pos(g_content, 0, DM_STATUS_H);
  lv_obj_set_size(g_content, width, g_content_h);
  lv_obj_set_style_bg_opa(g_content, LV_OPA_TRANSP, 0);
  lv_obj_clear_flag(g_content, LV_OBJ_FLAG_SCROLLABLE);

  /* Tab bar. */

  g_tabbar = lv_obj_create(g_screen);
  lv_obj_remove_style_all(g_tabbar);
  lv_obj_set_pos(g_tabbar, 0, height - DM_TABBAR_H);
  lv_obj_set_size(g_tabbar, width, DM_TABBAR_H);
  lv_obj_set_style_bg_color(g_tabbar, lv_color_hex(DM_COL_CARD), 0);
  lv_obj_set_style_bg_opa(g_tabbar, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(g_tabbar, 6, 0);
  lv_obj_set_style_pad_column(g_tabbar, 6, 0);
  lv_obj_set_flex_flow(g_tabbar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(g_tabbar, LV_FLEX_ALIGN_SPACE_EVENLY,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(g_tabbar, LV_OBJ_FLAG_SCROLLABLE);

  for (i = 0; i < DM_PAGE_COUNT; i++)
    {
      lv_obj_t *btn = lv_button_create(g_tabbar);

      lv_obj_set_size(btn, (width - 2 * DM_MARGIN) / DM_PAGE_COUNT - 8,
                      DM_TABBAR_H - 12);
      lv_obj_set_style_radius(btn, 8, 0);
      lv_obj_add_event_cb(btn, dm_ui_tab_cb, LV_EVENT_CLICKED, &g_tab_index[i]);
      dm_label(btn, g_pages[i]->title, DM_FONT_SMALL,
               lv_color_hex(DM_COL_TEXT));
      lv_obj_center(lv_obj_get_child(btn, 0));
      dm_ui_style_tab(btn, false);
    }

  /* Toast (hidden until something proactive happens). */

  g_toast = lv_label_create(g_screen);
  lv_obj_set_style_text_font(g_toast, DM_FONT_SMALL, 0);
  lv_obj_set_style_text_color(g_toast, lv_color_hex(DM_COL_ON_ACCENT), 0);
  lv_obj_set_style_bg_color(g_toast, lv_color_hex(DM_COL_WARN), 0);
  lv_obj_set_style_bg_opa(g_toast, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(g_toast, 8, 0);
  lv_obj_set_style_pad_all(g_toast, 8, 0);
  lv_obj_align(g_toast, LV_ALIGN_TOP_MID, 0, DM_STATUS_H + 8);
  lv_obj_add_flag(g_toast, LV_OBJ_FLAG_HIDDEN);

  dm_ui_set_clock();

  if (dm_ui_goto(0) != 0)
    {
      DM_ERR("ui: home page failed to build");
      return -1;
    }

  DM_LOG("ui: shell built (%dx%d, %d pages, content %dx%d)", width, height,
         DM_PAGE_COUNT, g_content_w, g_content_h);
  return 0;
}

int dm_ui_page_count(void)
{
  return DM_PAGE_COUNT;
}

const char *dm_ui_page_id(int index)
{
  if (index < 0 || index >= DM_PAGE_COUNT)
    {
      return NULL;
    }

  return g_pages[index]->id;
}

int dm_ui_page_index(const char *id)
{
  int i;

  if (id == NULL)
    {
      return -1;
    }

  for (i = 0; i < DM_PAGE_COUNT; i++)
    {
      if (strcmp(g_pages[i]->id, id) == 0)
        {
          return i;
        }
    }

  /* Allow the tab index to be given as a string. */

  if (id[0] >= '0' && id[0] <= '9' && id[1] == '\0')
    {
      int index = id[0] - '0';

      return (index < DM_PAGE_COUNT) ? index : -1;
    }

  return -1;
}

int dm_ui_current(void)
{
  return g_current;
}

const char *dm_ui_current_id(void)
{
  return dm_ui_page_id(g_current);
}

int dm_ui_goto(int index)
{
  dm_page_t *page;
  int i;

  if (index < 0 || index >= DM_PAGE_COUNT)
    {
      return -1;
    }

  page = g_pages[index];

  if (!page->built)
    {
      if (page->create(g_content, g_content_w, g_content_h) != 0)
        {
          DM_ERR("ui: page '%s' failed to build", page->id);
          return -1;
        }

      page->root = lv_obj_get_child(g_content,
                                    lv_obj_get_child_count(g_content) - 1);
      page->built = true;
    }

  for (i = 0; i < DM_PAGE_COUNT; i++)
    {
      if (!g_pages[i]->built || g_pages[i]->root == NULL)
        {
          continue;
        }

      if (i == index)
        {
          lv_obj_clear_flag(g_pages[i]->root, LV_OBJ_FLAG_HIDDEN);
        }
      else
        {
          lv_obj_add_flag(g_pages[i]->root, LV_OBJ_FLAG_HIDDEN);
        }
    }

  g_current = index;

  if (g_page_title != NULL)
    {
      lv_label_set_text(g_page_title, page->title);
    }

  for (i = 0; i < DM_PAGE_COUNT; i++)
    {
      lv_obj_t *btn = lv_obj_get_child(g_tabbar, i);

      if (btn != NULL)
        {
          dm_ui_style_tab(btn, i == index);
        }
    }

  if (page->enter != NULL)
    {
      page->enter();
    }

  DM_LOG("ui: page -> %s", page->id);
  return 0;
}

int dm_ui_goto_id(const char *id)
{
  int index = dm_ui_page_index(id);

  if (index < 0)
    {
      DM_WARN("ui: unknown page '%s'", id != NULL ? id : "(null)");
      return -1;
    }

  return dm_ui_goto(index);
}

int dm_ui_tab_target(const char *id, int *x, int *y)
{
  int index = dm_ui_page_index(id);
  lv_obj_t *btn;
  lv_area_t area;

  if (index < 0)
    {
      return -1;
    }

  btn = lv_obj_get_child(g_tabbar, index);
  if (btn == NULL)
    {
      return -1;
    }

  lv_obj_get_coords(btn, &area);

  if (x != NULL)
    {
      *x = (area.x1 + area.x2) / 2;
    }

  if (y != NULL)
    {
      *y = (area.y1 + area.y2) / 2;
    }

  return 0;
}

int dm_ui_object_count(void)
{
  if (g_screen == NULL)
    {
      return 0;
    }

  return (int)lv_obj_get_child_count(g_screen);
}

void dm_ui_toast(const char *text)
{
  if (g_toast == NULL || text == NULL)
    {
      return;
    }

  lv_label_set_text(g_toast, text);
  lv_obj_clear_flag(g_toast, LV_OBJ_FLAG_HIDDEN);
  lv_obj_align(g_toast, LV_ALIGN_TOP_MID, 0, DM_STATUS_H + 8);

  g_toast_ticks = DM_TOAST_TICKS;
}

void dm_ui_tick(void)
{
  char msg[512];

  dm_ui_set_clock();

  if (g_agent_badge != NULL)
    {
      lv_label_set_text_fmt(g_agent_badge, DM_I18N_AGENT_STATE "：%s",
                            dm_ui_agent_label());
    }

  /* Drain whatever the agent produced - LLM replies, offline answers, and
   * proactive events the device raised by itself.  Every message lands in the
   * conversation; a notice is shown when the user is looking elsewhere.
   */

  while (dm_agent_poll(msg, sizeof(msg)) == 1)
    {
      dm_page_chat_append_agent(msg);

      if (strcmp(dm_ui_current_id() != NULL ? dm_ui_current_id() : "",
                 "chat") != 0)
        {
          dm_ui_toast(msg);
        }
    }

  if (g_current >= 0 && g_pages[g_current]->update != NULL)
    {
      g_pages[g_current]->update();
    }

  if (g_toast_ticks > 0 && --g_toast_ticks == 0 && g_toast != NULL)
    {
      lv_obj_add_flag(g_toast, LV_OBJ_FLAG_HIDDEN);
    }
}
