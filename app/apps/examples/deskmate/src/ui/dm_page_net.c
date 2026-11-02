/****************************************************************************
 * apps/examples/deskmate/src/ui/dm_page_net.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 网络页：左边是有线/无线接口列表，右边是 WiFi 关联状态与扫描结果。
 *
 * 扫描走板载协处理器（ESP-Hosted），一次要几秒，所以**不在 UI 线程里做**：
 * 点"扫描"起一个 detached 线程，页面的 tick 轮询状态，期间显示“扫描中…”。
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <string.h>

#include <lvgl/lvgl.h>

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
static lv_obj_t *s_if_list;
static lv_obj_t *s_wifi_state;
static lv_obj_t *s_ap_list;
static char s_ap_ssid[DM_NET_MAX_APS][DM_NET_SSID_LEN];
static int  s_ap_count;
static bool s_scanning;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void dm_page_net_update(void);
static void dm_page_net_connect_cb(lv_event_t *event);
static void dm_page_net_fill_aps(void);

static void dm_page_net_scan_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) != LV_EVENT_CLICKED)
    {
      return;
    }

  if (dm_net_wifi_scan_start() != 0)
    {
      if (s_wifi_state != NULL)
        {
          dm_label_set(s_wifi_state, "%s", DM_I18N_NET_NO_WIFI);
        }

      return;
    }

  s_scanning = true;

  if (s_wifi_state != NULL)
    {
      dm_label_set(s_wifi_state, "%s", DM_I18N_NET_SCANNING);
    }

  if (s_ap_list != NULL)
    {
      lv_obj_clean(s_ap_list);
      dm_label(s_ap_list, DM_I18N_NET_SCANNING, DM_FONT_SMALL,
               lv_color_hex(DM_COL_MUTED));
    }
}

static void dm_page_net_fill_aps(void)
{
  struct dm_wifi_ap_s aps[DM_NET_MAX_APS];
  int found;
  int i;

  found = dm_net_wifi_scan_results(aps, DM_NET_MAX_APS);
  s_ap_count = found;

  if (s_ap_list == NULL)
    {
      return;
    }

  lv_obj_clean(s_ap_list);

  if (found <= 0)
    {
      dm_label(s_ap_list, DM_I18N_NET_NO_AP, DM_FONT_SMALL,
               lv_color_hex(DM_COL_MUTED));
      return;
    }

  for (i = 0; i < found; i++)
    {
      char text[96];
      lv_obj_t *row;
      lv_obj_t *btn;

      strlcpy(s_ap_ssid[i], aps[i].ssid, DM_NET_SSID_LEN);

      row = lv_obj_create(s_ap_list);
      lv_obj_remove_style_all(row);
      lv_obj_set_size(row, lv_pct(100), 40);
      lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
      lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

      snprintf(text, sizeof(text), "%s  %d dBm%s", aps[i].ssid, aps[i].rssi,
               aps[i].secure ? "  *" : "");

      dm_label(row, text, DM_FONT_SMALL, lv_color_hex(DM_COL_TEXT));

      btn = dm_button(row, DM_I18N_NET_CONNECT, 110, 32, NULL, NULL);
      lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
      lv_obj_add_event_cb(btn, dm_page_net_connect_cb, LV_EVENT_CLICKED,
                          &s_ap_ssid[i]);
    }
}

static void dm_page_net_connect_cb(lv_event_t *event)
{
  const char *ssid = (const char *)lv_event_get_user_data(event);

  if (lv_event_get_code(event) != LV_EVENT_CLICKED || ssid == NULL)
    {
      return;
    }

  if (dm_net_wifi_connect(ssid, NULL) == 0)
    {
      DM_LOG("net: connect requested for '%s'", ssid);
    }
  else
    {
      DM_WARN("net: connect to '%s' failed", ssid);
    }

  dm_page_net_update();
}

static int dm_page_net_create(lv_obj_t *parent, int w, int h)
{
  lv_obj_t *card;
  int left_w = (w - 3 * DM_MARGIN) / 2;

  s_root = dm_page_root(parent, w, h);

  card = dm_card_stack(s_root, DM_MARGIN, DM_MARGIN, left_w,
                       h - 2 * DM_MARGIN, lv_color_hex(DM_COL_CARD));
  dm_label(card, DM_I18N_NET_IFACES, DM_FONT_BODY, lv_color_hex(DM_COL_ACCENT));

  s_if_list = lv_obj_create(card);
  lv_obj_remove_style_all(s_if_list);
  lv_obj_set_width(s_if_list, lv_pct(100));
  lv_obj_set_flex_grow(s_if_list, 1);
  lv_obj_set_flex_flow(s_if_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_if_list, 8, 0);

  card = dm_card_stack(s_root, 2 * DM_MARGIN + left_w, DM_MARGIN, left_w,
                       h - 2 * DM_MARGIN, lv_color_hex(DM_COL_CARD2));

  dm_label(card, DM_I18N_NET_WIFI, DM_FONT_BODY, lv_color_hex(DM_COL_ACCENT));

  s_wifi_state = dm_label(card, "", DM_FONT_SMALL, lv_color_hex(DM_COL_TEXT));

  (void)dm_button(card, DM_I18N_NET_SCAN, 150, 40, dm_page_net_scan_cb, NULL);

  s_ap_list = lv_obj_create(card);
  lv_obj_remove_style_all(s_ap_list);
  lv_obj_set_width(s_ap_list, lv_pct(100));
  lv_obj_set_flex_grow(s_ap_list, 1);
  lv_obj_set_flex_flow(s_ap_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_ap_list, 6, 0);

  dm_page_net_update();

  DM_LOG("ui: network page built");
  return 0;
}

static void dm_page_net_update(void)
{
  const struct dm_net_status_s *net;
  char ssid[DM_NET_SSID_LEN];
  int rssi = 0;
  int i;

  if (s_root == NULL)
    {
      return;
    }

  /* 扫描进行中：每 tick 问一次状态，完成后填列表。 */

  if (s_scanning)
    {
      int count = 0;
      int err = 0;
      int st = dm_net_wifi_scan_status(&count, &err);

      if (st == 1)
        {
          if (s_wifi_state != NULL)
            {
              dm_label_set(s_wifi_state, "%s", DM_I18N_NET_SCANNING);
            }
        }
      else
        {
          s_scanning = false;
          dm_page_net_fill_aps();

          if (s_wifi_state != NULL && st == -1)
            {
              dm_label_set(s_wifi_state, "%s (%d)", DM_I18N_NET_SCAN_FAIL, err);
            }
        }
    }

  (void)dm_net_refresh();
  net = dm_net_status();

  if (s_if_list != NULL)
    {
      lv_obj_clean(s_if_list);

      if (net == NULL || net->count == 0)
        {
          dm_label(s_if_list, DM_I18N_NET_NONE, DM_FONT_SMALL,
                   lv_color_hex(DM_COL_MUTED));
        }
      else
        {
          for (i = 0; i < net->count; i++)
            {
              const struct dm_net_if_s *e = &net->ifs[i];
              char text[128];
              lv_color_t colour = e->ip[0] != '\0' ? lv_color_hex(DM_COL_OK) :
                                  (e->up ? lv_color_hex(DM_COL_WARN) :
                                   lv_color_hex(DM_COL_MUTED));

              snprintf(text, sizeof(text), "%s %s%s%s",
                       e->name,
                       e->running ? DM_I18N_NET_RUNNING
                                  : (e->up ? DM_I18N_NET_UP : DM_I18N_NET_DOWN),
                       e->ip[0] != '\0' ? "  " : "", e->ip);

              dm_label(s_if_list, text, DM_FONT_SMALL, colour);

              if (e->mask[0] != '\0')
                {
                  snprintf(text, sizeof(text), "  %s %s%s",
                           DM_I18N_NET_MASK, e->mask,
                           e->wireless ? "  " DM_I18N_NET_WIRELESS : "");
                  dm_label(s_if_list, text, DM_FONT_SMALL,
                           lv_color_hex(DM_COL_MUTED));
                }
            }
        }
    }

  if (s_wifi_state != NULL && !s_scanning)
    {
      if (!net->wireless_present)
        {
          dm_label_set(s_wifi_state, "%s", DM_I18N_NET_NO_WIFI);
        }
      else if (dm_net_wifi_link(ssid, sizeof(ssid), &rssi) == 0 &&
               ssid[0] != '\0')
        {
          dm_label_set(s_wifi_state, "%s '%s' (%d dBm)", DM_I18N_NET_LINKED,
                       ssid, rssi);
        }
      else
        {
          dm_label_set(s_wifi_state, "%s: %d dBm", DM_I18N_NET_WIFI, rssi);
        }
    }
}

static void dm_page_net_enter(void)
{
  /* 进页面就自动扫一次：板子串口经常被驱动日志占满，读不到 printf，
   * 扫描结果直接画在屏幕上（这比读串口可靠）。
   */

  dm_page_net_update();

  if (!s_scanning && dm_net_wifi_scan_start() == 0)
    {
      s_scanning = true;

      if (s_wifi_state != NULL)
        {
          dm_label_set(s_wifi_state, "%s", DM_I18N_NET_SCANNING);
        }
    }
}

dm_page_t dm_page_net =
{
  .id     = "net",
  .title  = DM_I18N_TAB_NET,
  .create = dm_page_net_create,
  .update = dm_page_net_update,
  .enter  = dm_page_net_enter,
  .root   = NULL,
  .built  = false,
};
