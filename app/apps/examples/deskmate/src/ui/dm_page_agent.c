/****************************************************************************
 * apps/examples/deskmate/src/ui/dm_page_agent.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 智能体页：把 ai_agent 的能力"嵌"进 DeskMate。
 *
 *   · 左：连接状态（ws/mock/off、在线/离线）、收发计数、**最近一条真实回复**
 *   · 右：配对二维码（LVGL 的 lv_qrcode，和 packages/ai_agent 的
 *     qrcode_display.c 用的是同一套 qrcodegen 算法），以及使用提示
 *
 * 注意：这里**不启用** CONFIG_AI_AGENT_LVGL_UI —— 那套 UI 会自己初始化显示和
 * lv_timer_handler 线程，与 DeskMate 的单线程 LVGL 抢屏。我们只复用它的数据通道
 * （ws 127.0.0.1:28789 / chat_id=deskmate-ui）和二维码算法。
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <string.h>

#include <lvgl/lvgl.h>

#include "agent/dm_agent.h"
#include "dm_config.h"
#include "dm_log.h"
#include "hal/dm_net.h"
#include "ui/dm_i18n.h"
#include "ui/dm_page.h"
#include "ui/dm_theme.h"
#include "ui/dm_widgets.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static lv_obj_t *s_root;
static lv_obj_t *s_state;
static lv_obj_t *s_counts;
static lv_obj_t *s_last;
static lv_obj_t *s_qr;
static bool      s_qr_ok;

/* 自检用：二维码是否真的生成成功（非 static，便于断言与取证）。 */

int g_app_agent_qr_ok;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void dm_page_agent_update(void);

static void dm_page_agent_build_qr(lv_obj_t *parent, const char *url)
{
  int rcssz = 240;

  s_qr = lv_qrcode_create(parent);
  if (s_qr == NULL)
    {
      DM_WARN("agent: cannot create the qr widget");
      return;
    }

  lv_qrcode_set_size(s_qr, rcssz);
  lv_qrcode_set_dark_color(s_qr, lv_color_hex(DM_COL_TEXT));
  lv_qrcode_set_light_color(s_qr, lv_color_hex(0xffffff));
  lv_obj_set_style_border_width(s_qr, 0, 0);

  s_qr_ok = lv_qrcode_update(s_qr, url, strlen(url)) == LV_RESULT_OK;
  g_app_agent_qr_ok = s_qr_ok ? 1 : 0;

  DM_LOG("agent: pairing qr '%s' -> %s", url, s_qr_ok ? "ok" : "failed");
}

static void dm_page_agent_reconnect_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) != LV_EVENT_CLICKED)
    {
      return;
    }

  dm_agent_reconnect();
  DM_LOG("agent: reconnect requested from the page");
  dm_page_agent_update();
}

static int dm_page_agent_create(lv_obj_t *parent, int w, int h)
{
  lv_obj_t *card;
  lv_obj_t *row;
  const struct dm_net_status_s *net;
  char url[96];
  const char *ip = "127.0.0.1";

  s_root = dm_page_root(parent, w, h);

  /* 左：状态 + 最近回复 */

  card = dm_card_stack(s_root, DM_MARGIN, DM_MARGIN, 360, h - 2 * DM_MARGIN,
                       lv_color_hex(DM_COL_CARD));
  dm_label(card, DM_I18N_AGENT_TITLE, DM_FONT_BODY, lv_color_hex(DM_COL_ACCENT));

  dm_label(card, DM_I18N_AGENT_STATE, DM_FONT_SMALL,
           lv_color_hex(DM_COL_MUTED));
  s_state = dm_label(card, "", DM_FONT_SMALL, lv_color_hex(DM_COL_TEXT));

  dm_label(card, DM_I18N_AGENT_COUNTS, DM_FONT_SMALL,
           lv_color_hex(DM_COL_MUTED));
  s_counts = dm_label(card, "", DM_FONT_SMALL, lv_color_hex(DM_COL_TEXT));

  dm_label(card, DM_I18N_AGENT_LAST, DM_FONT_SMALL, lv_color_hex(DM_COL_MUTED));
  s_last = dm_label(card, DM_I18N_AGENT_NONE, DM_FONT_SMALL,
                    lv_color_hex(DM_COL_TEXT));
  lv_label_set_long_mode(s_last, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s_last, lv_pct(100));

  row = lv_obj_create(card);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, lv_pct(100), 48);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  (void)dm_button(row, DM_I18N_AGENT_RECONNECT, 120, 42,
                  dm_page_agent_reconnect_cb, NULL);

  /* 右：配对二维码 */

  card = dm_card_stack(s_root, 400, DM_MARGIN, w - 420, h - 2 * DM_MARGIN,
                       lv_color_hex(DM_COL_CARD));
  dm_label(card, DM_I18N_AGENT_QR, DM_FONT_BODY, lv_color_hex(DM_COL_ACCENT));

  /* 网络状态由 dm_ui_tick 统一刷新（1s 节流），页面只读缓存。 */
  net = dm_net_status();
  if (net != NULL && net->primary[0] != '\0')
    {
      for (int i = 0; i < net->count; i++)
        {
          if (strcmp(net->ifs[i].name, net->primary) == 0 &&
              net->ifs[i].ip[0] != '\0')
            {
              ip = net->ifs[i].ip;
              break;
            }
        }
    }

  snprintf(url, sizeof(url), "http://%s:28789/?chat=deskmate-ui", ip);
  dm_page_agent_build_qr(card, url);

  dm_label(card, url, DM_FONT_SMALL, lv_color_hex(DM_COL_MUTED));
  dm_label(card, DM_I18N_AGENT_QR_HINT, DM_FONT_SMALL,
           lv_color_hex(DM_COL_MUTED));

  dm_page_agent_update();

  DM_LOG("ui: agent page built (qr=%s)", s_qr_ok ? "ok" : "no");
  return 0;
}

static void dm_page_agent_update(void)
{
  const char *state;
  const char *last;

  if (s_root == NULL)
    {
      return;
    }

  state = dm_agent_state();

  if (s_state != NULL)
    {
      if (strcmp(state, "online") == 0)
        {
          dm_label_set(s_state, "ws · %s", DM_I18N_AGENT_ONLINE);
        }
      else if (strcmp(state, "mock") == 0)
        {
          dm_label_set(s_state, "%s", DM_I18N_AGENT_MOCK);
        }
      else if (strcmp(state, "off") == 0)
        {
          dm_label_set(s_state, "off");
        }
      else
        {
          dm_label_set(s_state, "ws · %s", DM_I18N_AGENT_OFFLINE);
        }
    }

  if (s_counts != NULL)
    {
      dm_label_set(s_counts, "↑%u  ↓%u", dm_agent_sent_count(),
                   dm_agent_recv_count());
    }

  if (s_last != NULL)
    {
      last = dm_agent_last_recv();

      if (last != NULL && last[0] != '\0')
        {
          dm_label_set(s_last, "%s", last);
        }
    }
}

static void dm_page_agent_enter(void)
{
  dm_page_agent_update();
}

dm_page_t dm_page_agent =
{
  .id     = "agent",
  .title  = DM_I18N_TAB_AGENT,
  .create = dm_page_agent_create,
  .update = dm_page_agent_update,
  .enter  = dm_page_agent_enter,
  .root   = NULL,
  .built  = false,
};
