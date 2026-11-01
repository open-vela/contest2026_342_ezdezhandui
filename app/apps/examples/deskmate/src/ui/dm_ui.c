/****************************************************************************
 * apps/examples/deskmate/src/ui/dm_ui.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The DeskMate widget tree.  Milestone 1 builds the home page: a status bar, a
 * system card, an interactive tap card that proves touch input reaches the
 * widgets, and a footer.  Later milestones add the camera, chat, network and
 * settings pages on top of the same helpers.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <lvgl/lvgl.h>

#include "dm_config.h"
#include "dm_log.h"
#include "hal/dm_display.h"
#include "ui/dm_i18n.h"
#include "ui/dm_ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

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

#define DM_COL_BG       0x0f1319
#define DM_COL_CARD     0x1b2230
#define DM_COL_CARD2    0x222c3d
#define DM_COL_ACCENT   0x4c9aff
#define DM_COL_TEXT     0xe8eef6
#define DM_COL_MUTED    0x8d9bb0

/****************************************************************************
 * Private Data
 ****************************************************************************/

static lv_obj_t *g_time_label;
static lv_obj_t *g_uptime_label;
static lv_obj_t *g_geo_label;
static lv_obj_t *g_counter_label;
static lv_obj_t *g_tap_button;
static int       g_tap_count;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void dm_style_card(lv_obj_t *obj, lv_color_t bg)
{
  lv_obj_set_style_bg_color(obj, bg, 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(obj, 0, 0);
  lv_obj_set_style_radius(obj, 12, 0);
  lv_obj_set_style_pad_all(obj, 14, 0);
}

static lv_obj_t *dm_label(lv_obj_t *parent, const char *text,
                          const lv_font_t *font, lv_color_t colour)
{
  lv_obj_t *label = lv_label_create(parent);

  lv_label_set_text(label, text);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, colour, 0);

  return label;
}

static void dm_tap_event_cb(lv_event_t *event)
{
  lv_event_code_t code = lv_event_get_code(event);

  if (code != LV_EVENT_CLICKED)
    {
      return;
    }

  g_tap_count++;

  if (g_counter_label != NULL)
    {
      lv_label_set_text_fmt(g_counter_label, "%d %s", g_tap_count,
                            DM_I18N_TAP_COUNT);
    }

  DM_LOG("ui: tap %d handled by the button", g_tap_count);
}

static void dm_refresh_clock(void)
{
  time_t now = time(NULL);
  struct tm tm_buf;
  char text[32];

  if (localtime_r(&now, &tm_buf) == NULL)
    {
      return;
    }

  strftime(text, sizeof(text), "%H:%M:%S", &tm_buf);

  if (g_time_label != NULL)
    {
      lv_label_set_text(g_time_label, text);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int dm_ui_create(void)
{
  const struct dm_display_metrics_s *metrics = dm_display_metrics();
  lv_obj_t *screen;
  lv_obj_t *bar;
  lv_obj_t *card;
  lv_obj_t *title;
  lv_obj_t *value;
  int width;
  int height;
  int margin = 16;
  int bar_h;
  int card_h;
  int y;

  if (metrics == NULL)
    {
      DM_ERR("ui: display not ready");
      return -1;
    }

  width  = metrics->width;
  height = metrics->height;

  screen = lv_screen_active();
  lv_obj_remove_style_all(screen);
  lv_obj_set_style_bg_color(screen, lv_color_hex(DM_COL_BG), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

  /* Status bar ------------------------------------------------------------ */

  bar_h = DM_STATUS_BAR_H;

  bar = lv_obj_create(screen);
  lv_obj_remove_style_all(bar);
  lv_obj_set_pos(bar, 0, 0);
  lv_obj_set_size(bar, width, bar_h);
  lv_obj_set_style_bg_color(bar, lv_color_hex(DM_COL_CARD), 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_left(bar, margin, 0);
  lv_obj_set_style_pad_right(bar, margin, 0);

  title = dm_label(bar, DM_I18N_APP_TITLE, DM_FONT_TITLE,
                   lv_color_hex(DM_COL_TEXT));
  lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

  g_time_label = dm_label(bar, "--:--:--", DM_FONT_BODY,
                          lv_color_hex(DM_COL_ACCENT));
  lv_obj_align(g_time_label, LV_ALIGN_RIGHT_MID, 0, 0);

  /* System card ----------------------------------------------------------- */

  y = bar_h + margin;
  card_h = (height - y - DM_FOOTER_H - 2 * margin) * 3 / 5;
  if (card_h < 120)
    {
      card_h = height - y - DM_FOOTER_H - 2 * margin;
      if (card_h < 80)
        {
          card_h = height - y - margin;
        }
    }

  card = lv_obj_create(screen);
  lv_obj_remove_style_all(card);
  lv_obj_set_pos(card, margin, y);
  lv_obj_set_size(card, width - 2 * margin, card_h);
  dm_style_card(card, lv_color_hex(DM_COL_CARD));
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(card, 6, 0);

  dm_label(card, DM_I18N_SYSTEM_CARD, DM_FONT_BODY, lv_color_hex(DM_COL_ACCENT));

  g_geo_label = dm_label(card, "", DM_FONT_BODY, lv_color_hex(DM_COL_TEXT));
  lv_label_set_text_fmt(g_geo_label, "%s %dx%d RGB565 (%s)",
                        DM_I18N_DISPLAY, metrics->width, metrics->height,
                        metrics->backend);

  value = dm_label(card, "", DM_FONT_BODY, lv_color_hex(DM_COL_MUTED));
  lv_label_set_text_fmt(value, "%s %s", DM_I18N_LVGL, lv_version_info());

  value = dm_label(card, "", DM_FONT_BODY, lv_color_hex(DM_COL_MUTED));
  lv_label_set_text_fmt(value, "%s %s %s", DM_I18N_BUILD, DM_APP_NAME,
                        DM_APP_VERSION);

  g_uptime_label = dm_label(card, "", DM_FONT_BODY, lv_color_hex(DM_COL_MUTED));
  lv_label_set_text(g_uptime_label, DM_I18N_UPTIME ": 0 s");

  /* Touch card ------------------------------------------------------------ */

  y += card_h + margin;
  card_h = height - DM_FOOTER_H - margin - y;
  if (card_h < 96)
    {
      card_h = 96;
    }

  card = lv_obj_create(screen);
  lv_obj_remove_style_all(card);
  lv_obj_set_pos(card, margin, y);
  lv_obj_set_size(card, width - 2 * margin, card_h);
  dm_style_card(card, lv_color_hex(DM_COL_CARD2));

  dm_label(card, DM_I18N_TOUCH_CARD, DM_FONT_BODY,
           lv_color_hex(DM_COL_ACCENT));

  g_tap_button = lv_button_create(card);
  lv_obj_set_size(g_tap_button, 140, 52);
  lv_obj_align(g_tap_button, LV_ALIGN_LEFT_MID, 0, 12);
  lv_obj_set_style_bg_color(g_tap_button, lv_color_hex(DM_COL_ACCENT), 0);
  lv_obj_set_style_radius(g_tap_button, 10, 0);
  lv_obj_add_event_cb(g_tap_button, dm_tap_event_cb, LV_EVENT_CLICKED, NULL);

  {
    lv_obj_t *btn_label = dm_label(g_tap_button, DM_I18N_TAP_BUTTON,
                                   DM_FONT_BODY, lv_color_hex(0x08131f));
    lv_obj_center(btn_label);
  }

  g_counter_label = dm_label(card, "", DM_FONT_BODY, lv_color_hex(DM_COL_TEXT));
  lv_label_set_text_fmt(g_counter_label, "0 %s", DM_I18N_TAP_COUNT);
  lv_obj_align_to(g_counter_label, g_tap_button, LV_ALIGN_OUT_RIGHT_MID, 18, 0);

  {
    lv_obj_t *hint = dm_label(card, DM_I18N_TAP_HINT, LV_FONT_DEFAULT,
                              lv_color_hex(DM_COL_MUTED));
    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  }

  /* Footer ---------------------------------------------------------------- */

  {
    lv_obj_t *footer = dm_label(screen, DM_I18N_HOME "  -  "
                                DM_I18N_APP_SUBTITLE, LV_FONT_DEFAULT,
                                lv_color_hex(DM_COL_MUTED));
    lv_obj_align(footer, LV_ALIGN_BOTTOM_LEFT, margin, -10);
  }

  dm_refresh_clock();

  DM_LOG("ui: home page built (%d top-level objects)",
         (int)lv_obj_get_child_count(screen));
  return 0;
}

void dm_ui_tick(void)
{
  static unsigned int last_second;
  struct timespec ts;
  unsigned int now_s;

  dm_refresh_clock();

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
      return;
    }

  now_s = (unsigned int)ts.tv_sec;
  if (now_s == last_second)
    {
      return;
    }

  last_second = now_s;

  if (g_uptime_label != NULL)
    {
      lv_label_set_text_fmt(g_uptime_label, DM_I18N_UPTIME ": %u s", now_s);
    }
}

int dm_ui_tap_count(void)
{
  return g_tap_count;
}

int dm_ui_tap_target(int *x, int *y)
{
  lv_area_t area;

  if (g_tap_button == NULL)
    {
      return -1;
    }

  lv_obj_get_coords(g_tap_button, &area);

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
  lv_obj_t *screen = lv_screen_active();

  if (screen == NULL)
    {
      return 0;
    }

  return (int)lv_obj_get_child_count(screen);
}
