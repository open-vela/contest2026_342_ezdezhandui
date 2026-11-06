/****************************************************************************
 * apps/examples/deskmate/src/ui/dm_page_home.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Home page: clock, build/display identity, a capability summary of the four
 * board peripherals, and a tap target the self-test uses to prove that touch
 * input reaches the widgets.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <time.h>

#include <lvgl/lvgl.h>

#include "agent/dm_agent.h"
#include "dm_config.h"
#include "dm_log.h"
#include "hal/dm_camera.h"
#include "hal/dm_display.h"
#include "hal/dm_net.h"
#include "ui/dm_i18n.h"
#include "ui/dm_page.h"
#include "ui/dm_theme.h"
#include "ui/dm_widgets.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static lv_obj_t *s_root;
static lv_obj_t *s_clock;
static lv_obj_t *s_uptime;
static lv_obj_t *s_cap_camera;
static lv_obj_t *s_cap_net;
static lv_obj_t *s_cap_agent;
static lv_obj_t *s_taps;
static lv_obj_t *s_tap_button;
static int       s_tap_count;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void dm_home_update(void);

static void dm_home_tap_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) != LV_EVENT_CLICKED)
    {
      return;
    }

  s_tap_count++;
  dm_label_set(s_taps, "%d %s", s_tap_count, DM_I18N_TAP_COUNT);
  DM_LOG("ui: tap %d handled by the home button", s_tap_count);
}

static int dm_home_create(lv_obj_t *parent, int w, int h)
{
  const struct dm_display_metrics_s *metrics = dm_display_metrics();
  lv_obj_t *card;
  lv_obj_t *label;
  int card_w = (w - 3 * DM_MARGIN) / 2;
  int top_h = (h - 3 * DM_MARGIN) * 3 / 5;

  s_root = dm_page_root(parent, w, h);

  /* Left column: system identity. */

  card = dm_card_stack(s_root, DM_MARGIN, DM_MARGIN, card_w, top_h,
                       lv_color_hex(DM_COL_CARD));

  dm_label(card, DM_I18N_SYSTEM_CARD, DM_FONT_BODY,
           lv_color_hex(DM_COL_ACCENT));

  label = dm_label(card, "", DM_FONT_SMALL, lv_color_hex(DM_COL_TEXT));
  dm_label_set(label, "%s %s %s", DM_I18N_BUILD, DM_APP_NAME, DM_APP_VERSION);

  label = dm_label(card, "", DM_FONT_SMALL, lv_color_hex(DM_COL_MUTED));
  dm_label_set(label, "%s %s", DM_I18N_LVGL, lv_version_info());

  label = dm_label(card, "", DM_FONT_SMALL, lv_color_hex(DM_COL_MUTED));
  dm_label_set(label, "%s %dx%d %dbpp (%s)", DM_I18N_DISPLAY,
               metrics != NULL ? metrics->width : 0,
               metrics != NULL ? metrics->height : 0,
               metrics != NULL ? metrics->bpp : 0,
               metrics != NULL ? metrics->backend : "?");

  s_clock = dm_label(card, "--:--:--", DM_FONT_TITLE,
                     lv_color_hex(DM_COL_TEXT));

  s_uptime = dm_label(card, DM_I18N_UPTIME "：0 " DM_I18N_SECONDS, DM_FONT_SMALL,
                      lv_color_hex(DM_COL_MUTED));

  /* Right column: capability summary. */

  card = dm_card_stack(s_root, 2 * DM_MARGIN + card_w, DM_MARGIN, card_w, top_h,
                       lv_color_hex(DM_COL_CARD));

  dm_label(card, DM_I18N_STATE_CARD, DM_FONT_BODY, lv_color_hex(DM_COL_ACCENT));

  s_cap_camera = dm_label(card, "", DM_FONT_SMALL, lv_color_hex(DM_COL_TEXT));
  s_cap_net    = dm_label(card, "", DM_FONT_SMALL, lv_color_hex(DM_COL_TEXT));
  s_cap_agent  = dm_label(card, "", DM_FONT_SMALL, lv_color_hex(DM_COL_TEXT));

  dm_home_update();

  /* Bottom card: touch. */

  card = dm_card(s_root, DM_MARGIN, 2 * DM_MARGIN + top_h,
                 w - 2 * DM_MARGIN, h - 3 * DM_MARGIN - top_h,
                 lv_color_hex(DM_COL_CARD));

  dm_label_at(card, DM_I18N_TOUCH_CARD, DM_FONT_BODY,
              lv_color_hex(DM_COL_ACCENT), 0, 0);

  s_tap_button = dm_button(card, DM_I18N_TAP_BUTTON, 150, 52,
                           dm_home_tap_cb, NULL);
  lv_obj_align(s_tap_button, LV_ALIGN_LEFT_MID, 0, 12);

  s_taps = dm_label_at(card, "0 " DM_I18N_TAP_COUNT, DM_FONT_BODY,
                       lv_color_hex(DM_COL_TEXT), 170, 0);
  lv_obj_align_to(s_taps, s_tap_button, LV_ALIGN_OUT_RIGHT_MID, 18, 0);

  {
    lv_obj_t *hint = dm_label(card, DM_I18N_TAP_HINT, DM_FONT_SMALL,
                              lv_color_hex(DM_COL_MUTED));

    lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  }

  DM_LOG("ui: home page built (%d objects)",
         (int)lv_obj_get_child_count(s_root));
  return 0;
}

static void dm_home_update(void)
{
  const struct dm_net_status_s *net;
  time_t now = time(NULL);
  struct tm tm_buf;
  char text[32];
  char desc[192];

  if (s_root == NULL)
    {
      return;
    }

  /* 走 dm_label_set()：它自带"文字没变就一个字都别写"的护栏。
   *
   * 这里原本是裸的 lv_label_set_text()，于是每轮 dm_ui_tick() 都无条件把首页
   * 时钟标脏。真机 JTAG 实测（2026-11-06，home 页 12s 窗口）：脏区
   * 13,603 px/帧、**提交数 ≡ 循环数**（20.8 次/秒）—— 整屏 20 次/秒地在为一块
   * 只按秒跳动的表针重绘+回写。顶栏时钟（dm_ui.c:146）、agent 徽标
   * （dm_ui.c:608）、相机页统计标签（dm_page_camera.c:375）都早就有这道护栏，
   * 只有首页这一处漏了。
   */

  if (localtime_r(&now, &tm_buf) != NULL &&
      strftime(text, sizeof(text), "%H:%M:%S", &tm_buf) > 0 && s_clock != NULL)
    {
      dm_label_set(s_clock, "%s", text);
    }

  if (s_uptime != NULL)
    {
      struct timespec ts;

      if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        {
          dm_label_set(s_uptime, DM_I18N_UPTIME "：%u " DM_I18N_SECONDS,
                       (unsigned)ts.tv_sec);
        }
    }

  if (s_cap_camera != NULL)
    {
      dm_label_set(s_cap_camera, "%s: %s", DM_I18N_CAMERA_STATE,
                   dm_camera_ready() ? dm_camera_backend() : DM_I18N_NOT_OPENED);
    }

  if (s_cap_net != NULL)
    {
  /* 网络状态由 dm_ui_tick 统一刷新（1s 节流），页面只读缓存。 */
      net = dm_net_status();
      (void)dm_net_describe(desc, sizeof(desc));
      dm_label_set(s_cap_net, "%s: %s", DM_I18N_NET_STATE,
                   (net != NULL) ? desc : "?");
    }

  if (s_cap_agent != NULL)
    {
      dm_label_set(s_cap_agent, "%s: %s (%u/%u)", DM_I18N_AGENT_STATE,
                   dm_agent_state(), dm_agent_sent_count(),
                   dm_agent_recv_count());
    }
}

static void dm_home_enter(void)
{
  dm_home_update();
}

int dm_page_home_tap_target(int *x, int *y)
{
  lv_area_t area;

  if (s_tap_button == NULL)
    {
      return -1;
    }

  lv_obj_get_coords(s_tap_button, &area);

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

int dm_page_home_tap_count(void)
{
  return s_tap_count;
}

dm_page_t dm_page_home =
{
  .id     = "home",
  .title  = DM_I18N_TAB_HOME,
  .create = dm_home_create,
  .update = dm_home_update,
  .enter  = dm_home_enter,
  .root   = NULL,
  .built  = false,
};
