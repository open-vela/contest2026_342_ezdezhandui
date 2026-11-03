/****************************************************************************
 * apps/examples/deskmate/src/ui/dm_page_net.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 网络页（类 PC 设置）：
 *   · 左：接口列表（eth0 / wlan0 的状态、地址、掩码）
 *   · 右上：以太网——自动(DHCP) / 手动(静态 IP) 切换；手动时可输入
 *     地址/掩码/网关；应用后 5 秒内未确认则自动回滚（和 PC 一样）
 *   · 右下：无线——扫描列表 + 选网输密码（弹出键盘）+ 断开；热点如实显示
 *     "从机固件不支持"（平台限制，见 dm_net.h 的实测记录）
 *
 * 扫描与网络查询都不在 UI 线程里阻塞：扫描走 dm_net 的后台线程，
 * 页面只在 tick 里读状态。
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
static lv_obj_t *s_eth_state;
static lv_obj_t *s_field_addr;
static lv_obj_t *s_field_mask;
static lv_obj_t *s_field_gw;

static char s_ap_ssid[DM_NET_MAX_APS][DM_NET_SSID_LEN];
static int  s_ap_count;
static bool s_scanning;
static uint32_t s_last_refresh_ms;

/* 以太网手动配置（未点"应用"前只是草稿） */

static char s_draft_addr[20];
static char s_draft_mask[20];
static char s_draft_gw[20];
static bool s_manual;
static char s_applied_addr[20];
static char s_applied_mask[20];
static char s_applied_gw[20];

/* 回滚：应用静态地址后 5 秒内未点"保留"，就回到应用前的配置 */

static lv_timer_t *s_rollback_timer;
static lv_obj_t   *s_confirm_modal;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void dm_page_net_update(void);
static void dm_page_net_fill_aps(void);
static void dm_page_net_close_modal(void);
static void dm_page_net_eth_refresh_fields(void);

/* ── 通用输入弹窗（无线密码 / IP 字段都用它）────────────────────────────── */

typedef void (*dm_input_done_t)(const char *text, void *arg);

static dm_input_done_t s_input_done;
static void            *s_input_arg;

static void dm_modal_ok_cb(lv_event_t *event)
{
  lv_obj_t *ta = (lv_obj_t *)lv_event_get_user_data(event);
  const char *text = ta != NULL ? lv_textarea_get_text(ta) : "";

  if (s_input_done != NULL)
    {
      s_input_done(text, s_input_arg);
    }

  s_input_done = NULL;
  s_input_arg  = NULL;
  dm_page_net_close_modal();
}

static void dm_modal_cancel_cb(lv_event_t *event)
{
  (void)event;

  s_input_done = NULL;
  s_input_arg  = NULL;
  dm_page_net_close_modal();
}

static void dm_modal_kb_cb(lv_event_t *event)
{
  lv_event_code_t code = lv_event_get_code(event);

  if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
    {
      lv_obj_t *ta = (lv_obj_t *)lv_event_get_user_data(event);

      if (code == LV_EVENT_READY && s_input_done != NULL)
        {
          s_input_done(ta != NULL ? lv_textarea_get_text(ta) : "", s_input_arg);
        }

      s_input_done = NULL;
      s_input_arg  = NULL;
      dm_page_net_close_modal();
    }
}

static void dm_page_net_close_modal(void)
{
  lv_obj_t *top = lv_layer_top();

  if (s_confirm_modal != NULL)
    {
      lv_obj_delete(s_confirm_modal);
      s_confirm_modal = NULL;
    }

  /* 弹窗都建在 top layer 上的同一个容器里，清掉即可。 */

  lv_obj_clean(top);
  lv_obj_clear_flag(top, LV_OBJ_FLAG_CLICKABLE);
}

static void dm_page_net_input_modal(const char *title, const char *initial,
                                    bool numeric, dm_input_done_t done,
                                    void *arg)
{
  lv_obj_t *overlay;
  lv_obj_t *card;
  lv_obj_t *ta;
  lv_obj_t *kb;

  dm_page_net_close_modal();

  s_input_done = done;
  s_input_arg  = arg;

  overlay = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(overlay);
  lv_obj_set_size(overlay, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_40, 0);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

  card = dm_card_stack(overlay, 220, 60, 584, 400, lv_color_hex(DM_COL_CARD));
  dm_label(card, title, DM_FONT_BODY, lv_color_hex(DM_COL_TEXT));

  ta = lv_textarea_create(card);
  lv_textarea_set_one_line(ta, true);
  lv_textarea_set_text(ta, initial != NULL ? initial : "");
  lv_obj_set_width(ta, lv_pct(100));
  lv_obj_set_style_text_font(ta, DM_FONT_BODY, 0);

  {
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 52);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);

    (void)dm_button(row, DM_I18N_NET_CANCEL, 120, 44, dm_modal_cancel_cb, NULL);
    (void)dm_button(row, DM_I18N_NET_APPLY, 120, 44, dm_modal_ok_cb, ta);
  }

  kb = lv_keyboard_create(card);
  lv_obj_set_width(kb, lv_pct(100));
  lv_obj_set_flex_grow(kb, 1);
  lv_keyboard_set_mode(kb, numeric ? LV_KEYBOARD_MODE_NUMBER :
                                     LV_KEYBOARD_MODE_TEXT_LOWER);
  lv_keyboard_set_textarea(kb, ta);
  lv_obj_add_event_cb(kb, dm_modal_kb_cb, LV_EVENT_READY, ta);
  lv_obj_add_event_cb(kb, dm_modal_kb_cb, LV_EVENT_CANCEL, ta);
}

/* ── 以太网：手动字段 ─────────────────────────────────────────────────── */

static void dm_page_net_field_done(const char *text, void *arg)
{
  char *dst = (char *)arg;

  strlcpy(dst, text, 20);

  if (dst == s_draft_addr)
    {
      dm_label_set(s_field_addr, "%s", s_draft_addr);
    }
  else if (dst == s_draft_mask)
    {
      dm_label_set(s_field_mask, "%s", s_draft_mask);
    }
  else if (dst == s_draft_gw)
    {
      dm_label_set(s_field_gw, "%s", s_draft_gw);
    }
}

static void dm_page_net_field_cb(lv_event_t *event)
{
  char *dst = (char *)lv_event_get_user_data(event);

  dm_page_net_input_modal(DM_I18N_NET_ADDR, dst, true,
                          dm_page_net_field_done, dst);
}

static lv_obj_t *dm_page_net_field(lv_obj_t *parent, const char *caption,
                                   lv_obj_t **value_label, char *dst)
{
  lv_obj_t *col = lv_obj_create(parent);
  lv_obj_t *btn;

  lv_obj_remove_style_all(col);
  lv_obj_set_size(col, lv_pct(30), 74);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(col, 4, 0);

  dm_label(col, caption, DM_FONT_SMALL, lv_color_hex(DM_COL_MUTED));

  btn = lv_button_create(col);
  lv_obj_set_size(btn, lv_pct(100), 40);
  lv_obj_set_style_bg_color(btn, lv_color_hex(DM_COL_CARD2), 0);
  lv_obj_set_style_radius(btn, 8, 0);
  lv_obj_add_event_cb(btn, dm_page_net_field_cb, LV_EVENT_CLICKED, dst);

  *value_label = dm_label(btn, dst, DM_FONT_SMALL, lv_color_hex(DM_COL_TEXT));
  lv_obj_center(*value_label);
  return col;
}

/* ── 以太网：模式与应用 ───────────────────────────────────────────────── */

static void dm_page_net_rollback_now(void)
{
  if (s_applied_addr[0] != '\0')
    {
      (void)dm_net_if_set_static("eth0", s_applied_addr, s_applied_mask,
                                 s_applied_gw);
    }

  if (s_eth_state != NULL)
    {
      dm_label_set(s_eth_state, "%s", DM_I18N_NET_ROLLBACK);
    }

  DM_LOG("net: static config rolled back");
}

static void dm_rollback_timer_cb(lv_timer_t *timer)
{
  (void)timer;

  dm_page_net_close_modal();
  dm_page_net_rollback_now();

  if (s_rollback_timer != NULL)
    {
      lv_timer_delete(s_rollback_timer);
      s_rollback_timer = NULL;
    }
}

static void dm_keep_cb(lv_event_t *event)
{
  (void)event;

  dm_page_net_close_modal();

  if (s_rollback_timer != NULL)
    {
      lv_timer_delete(s_rollback_timer);
      s_rollback_timer = NULL;
    }

  strlcpy(s_applied_addr, s_draft_addr, sizeof(s_applied_addr));
  strlcpy(s_applied_mask, s_draft_mask, sizeof(s_applied_mask));
  strlcpy(s_applied_gw, s_draft_gw, sizeof(s_applied_gw));

  if (s_eth_state != NULL)
    {
      dm_label_set(s_eth_state, "%s", DM_I18N_NET_STATIC_OK);
    }
}

static void dm_page_net_apply_eth(void)
{
  int ret;

  if (!s_manual)
    {
      if (s_eth_state != NULL)
        {
          dm_label_set(s_eth_state, "%s", DM_I18N_NET_DHCP_WAIT);
        }

      /* DHCP 会阻塞几秒——放后台线程，页面 tick 里刷新状态即可。 */

      dm_net_if_set_dhcp_async("eth0");
      return;
    }

  if (!dm_net_ipv4_valid(s_draft_addr) || !dm_net_ipv4_valid(s_draft_mask) ||
      (s_draft_gw[0] != '\0' && !dm_net_ipv4_valid(s_draft_gw)))
    {
      if (s_eth_state != NULL)
        {
          dm_label_set(s_eth_state, "%s", DM_I18N_NET_BAD_INPUT);
        }

      return;
    }

  ret = dm_net_if_set_static("eth0", s_draft_addr, s_draft_mask, s_draft_gw);

  if (s_eth_state != NULL)
    {
      dm_label_set(s_eth_state, "%s %s", ret == 0 ? DM_I18N_NET_STATIC_OK :
                   DM_I18N_NET_BAD_INPUT, s_draft_addr);
    }

  if (ret != 0)
    {
      return;
    }

  /* 5 秒未确认自动回滚（类 PC 的"保留设置"）。 */

  {
    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    lv_obj_t *card;
    lv_obj_t *row;

    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_40, 0);

    card = dm_card_stack(overlay, 300, 180, 424, 200,
                         lv_color_hex(DM_COL_CARD));
    dm_label(card, DM_I18N_NET_STATIC_OK, DM_FONT_BODY,
             lv_color_hex(DM_COL_TEXT));
    dm_label(card, DM_I18N_NET_ROLLBACK, DM_FONT_SMALL,
             lv_color_hex(DM_COL_MUTED));

    row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 52);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    (void)dm_button(row, DM_I18N_NET_KEEP, 160, 44, dm_keep_cb, NULL);

    s_rollback_timer = lv_timer_create(dm_rollback_timer_cb, 5000, NULL);
    lv_timer_set_repeat_count(s_rollback_timer, 1);
  }
}

static void dm_page_net_apply_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) == LV_EVENT_CLICKED)
    {
      dm_page_net_apply_eth();
    }
}

static void dm_page_net_mode_auto_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) != LV_EVENT_CLICKED)
    {
      return;
    }

  s_manual = false;
  dm_page_net_eth_refresh_fields();

  if (s_eth_state != NULL)
    {
      dm_label_set(s_eth_state, "%s", DM_I18N_NET_MODE_AUTO);
    }
}

static void dm_page_net_mode_manual_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) != LV_EVENT_CLICKED)
    {
      return;
    }

  s_manual = true;
  dm_page_net_eth_refresh_fields();

  if (s_eth_state != NULL)
    {
      dm_label_set(s_eth_state, "%s", DM_I18N_NET_MODE_MANUAL);
    }
}

/* ── 无线：列表、密码、断开 ───────────────────────────────────────────── */

static void dm_page_net_password_done(const char *text, void *arg)
{
  const char *ssid = (const char *)arg;

  if (dm_net_wifi_connect(ssid, text) == 0)
    {
      if (s_wifi_state != NULL)
        {
          dm_label_set(s_wifi_state, "%s", DM_I18N_NET_CONNECTING);
        }
    }
  else if (s_wifi_state != NULL)
    {
      dm_label_set(s_wifi_state, "%s", DM_I18N_NET_CONN_FAIL);
    }
}

static void dm_page_net_ap_cb(lv_event_t *event)
{
  const char *ssid = (const char *)lv_event_get_user_data(event);

  if (lv_event_get_code(event) != LV_EVENT_CLICKED || ssid == NULL)
    {
      return;
    }

  dm_page_net_input_modal(DM_I18N_NET_PASSWORD, "", false,
                          dm_page_net_password_done, (void *)ssid);
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
      lv_obj_set_size(row, lv_pct(100), 44);
      lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
      lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

      snprintf(text, sizeof(text), "%s   %d dBm%s", aps[i].ssid, aps[i].rssi,
               aps[i].secure ? "  *" : "");

      dm_label(row, text, DM_FONT_SMALL, lv_color_hex(DM_COL_TEXT));

      btn = dm_button(row, DM_I18N_NET_CONNECT, 110, 34, dm_page_net_ap_cb,
                      &s_ap_ssid[i]);
      lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

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

static void dm_page_net_disconnect_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) != LV_EVENT_CLICKED)
    {
      return;
    }

  (void)dm_net_wifi_disconnect();
  dm_page_net_update();
}

static void dm_page_net_ap_note_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) != LV_EVENT_CLICKED)
    {
      return;
    }

  if (s_wifi_state != NULL)
    {
      dm_label_set(s_wifi_state, "%s", DM_I18N_NET_AP_UNSUP);
    }
}

/* ── 构建与刷新 ───────────────────────────────────────────────────────── */

static void dm_page_net_eth_refresh_fields(void)
{
  lv_color_t colour = lv_color_hex(s_manual ? DM_COL_TEXT : DM_COL_MUTED);

  if (s_field_addr != NULL)
    {
      lv_obj_set_style_text_color(s_field_addr, colour, 0);
    }

  if (s_field_mask != NULL)
    {
      lv_obj_set_style_text_color(s_field_mask, colour, 0);
    }

  if (s_field_gw != NULL)
    {
      lv_obj_set_style_text_color(s_field_gw, colour, 0);
    }
}

static int dm_page_net_create(lv_obj_t *parent, int w, int h)
{
  lv_obj_t *card;
  lv_obj_t *row;

  s_root = dm_page_root(parent, w, h);

  /* 左：接口列表 */

  card = dm_card_stack(s_root, DM_MARGIN, DM_MARGIN, 300, h - 2 * DM_MARGIN,
                       lv_color_hex(DM_COL_CARD));
  dm_label(card, DM_I18N_NET_IFACES, DM_FONT_BODY, lv_color_hex(DM_COL_ACCENT));

  s_if_list = lv_obj_create(card);
  lv_obj_remove_style_all(s_if_list);
  lv_obj_set_width(s_if_list, lv_pct(100));
  lv_obj_set_flex_grow(s_if_list, 1);
  lv_obj_set_flex_flow(s_if_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_if_list, 8, 0);

  /* 右上：以太网 */

  card = dm_card_stack(s_root, 340, DM_MARGIN, w - 360, 206,
                       lv_color_hex(DM_COL_CARD));

  row = lv_obj_create(card);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, lv_pct(100), 44);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 12, 0);

  dm_label(row, "eth0", DM_FONT_BODY, lv_color_hex(DM_COL_ACCENT));
  (void)dm_button(row, DM_I18N_NET_MODE_AUTO, 150, 40,
                  dm_page_net_mode_auto_cb, NULL);
  (void)dm_button(row, DM_I18N_NET_MODE_MANUAL, 110, 40,
                  dm_page_net_mode_manual_cb, NULL);
  (void)dm_button(row, DM_I18N_NET_APPLY, 100, 40, dm_page_net_apply_cb, NULL);

  row = lv_obj_create(card);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, lv_pct(100), 78);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 10, 0);

  (void)dm_page_net_field(row, DM_I18N_NET_ADDR, &s_field_addr, s_draft_addr);
  (void)dm_page_net_field(row, DM_I18N_NET_MASK, &s_field_mask, s_draft_mask);
  (void)dm_page_net_field(row, DM_I18N_NET_GATEWAY, &s_field_gw, s_draft_gw);

  s_eth_state = dm_label(card, "", DM_FONT_SMALL, lv_color_hex(DM_COL_MUTED));

  /* 右下：无线 */

  card = dm_card_stack(s_root, 340, 246, w - 360, h - 266,
                       lv_color_hex(DM_COL_CARD));

  row = lv_obj_create(card);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, lv_pct(100), 44);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 12, 0);

  dm_label(row, DM_I18N_NET_WIFI, DM_FONT_BODY, lv_color_hex(DM_COL_ACCENT));
  (void)dm_button(row, DM_I18N_NET_SCAN, 110, 40, dm_page_net_scan_cb, NULL);
  (void)dm_button(row, DM_I18N_NET_FORGET, 110, 40, dm_page_net_disconnect_cb,
                  NULL);
  (void)dm_button(row, DM_I18N_NET_HOTSPOT, 110, 40, dm_page_net_ap_note_cb,
                  NULL);

  s_wifi_state = dm_label(card, "", DM_FONT_SMALL, lv_color_hex(DM_COL_TEXT));

  s_ap_list = lv_obj_create(card);
  lv_obj_remove_style_all(s_ap_list);
  lv_obj_set_width(s_ap_list, lv_pct(100));
  lv_obj_set_flex_grow(s_ap_list, 1);
  lv_obj_set_flex_flow(s_ap_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_ap_list, 6, 0);

  dm_page_net_update();

  DM_LOG("ui: network page built (PC-like)");
  return 0;
}

static void dm_page_net_update(void)
{
  const struct dm_net_status_s *net;
  char ssid[DM_NET_SSID_LEN];
  char addr[20] = "";
  char mask[20] = "";
  char gw[20] = "";
  int  rssi = 0;
  int  i;

  if (s_root == NULL)
    {
      return;
    }

  /* 扫描状态 */

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

  /* 节流：接口列表 1s 刷新一次（原来每 tick 都查，把驱动 RPC 刷爆过）。 */

  {
    uint32_t now = lv_tick_get();

    if (s_last_refresh_ms != 0 && now - s_last_refresh_ms < 1000)
      {
        return;
      }

    s_last_refresh_ms = now;
  }

  /* 网络状态由 dm_ui_tick 统一刷新（1s 节流），页面只读缓存。 */
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
              lv_color_t colour = e->ip[0] != '\0' && strcmp(e->ip, "0.0.0.0") != 0
                                  ? lv_color_hex(DM_COL_OK)
                                  : (e->up ? lv_color_hex(DM_COL_WARN) :
                                             lv_color_hex(DM_COL_MUTED));

              snprintf(text, sizeof(text), "%s %s", e->name,
                       e->running ? DM_I18N_NET_RUNNING
                                  : (e->up ? DM_I18N_NET_UP : DM_I18N_NET_DOWN));

              dm_label(s_if_list, text, DM_FONT_SMALL, colour);

              snprintf(text, sizeof(text), "  %s %s",
                       e->ip[0] != '\0' ? e->ip : "-",
                       e->mask[0] != '\0' ? e->mask : "");
              dm_label(s_if_list, text, DM_FONT_SMALL,
                       lv_color_hex(DM_COL_MUTED));
            }
        }
    }

  /* 异步 DHCP 结果 */

  {
    int derr = 0;
    int dst = dm_net_if_dhcp_status(&derr);

    if (s_eth_state != NULL && dst == 2)
      {
        dm_label_set(s_eth_state, "%s", DM_I18N_NET_MODE_AUTO " OK");
      }
    else if (s_eth_state != NULL && dst == -1)
      {
        dm_label_set(s_eth_state, "%s (%d)", DM_I18N_NET_DHCP_FAIL, derr);
      }
  }

  /* 以太网当前配置 → 草稿（首次进入或未手动编辑时同步一次） */

  (void)dm_net_if_get_config("eth0", addr, sizeof(addr), mask, sizeof(mask),
                             gw, sizeof(gw));

  if (!s_manual)
    {
      strlcpy(s_draft_addr, addr[0] != '\0' && strcmp(addr, "0.0.0.0") != 0 ?
              addr : "192.168.1.100", sizeof(s_draft_addr));
      strlcpy(s_draft_mask, mask[0] != '\0' && strcmp(mask, "0.0.0.0") != 0 ?
              mask : "255.255.255.0", sizeof(s_draft_mask));
      strlcpy(s_draft_gw, gw[0] != '\0' && strcmp(gw, "0.0.0.0") != 0 ?
              gw : "192.168.1.1", sizeof(s_draft_gw));
    }

  if (s_field_addr != NULL)
    {
      dm_label_set(s_field_addr, "%s", s_draft_addr);
    }

  if (s_field_mask != NULL)
    {
      dm_label_set(s_field_mask, "%s", s_draft_mask);
    }

  if (s_field_gw != NULL)
    {
      dm_label_set(s_field_gw, "%s", s_draft_gw);
    }

  if (s_eth_state != NULL && addr[0] != '\0')
    {
      dm_label_set(s_eth_state, "%s %s", DM_I18N_NET_ADDR, addr);
    }

  /* 无线状态 */

  if (s_wifi_state != NULL && !s_scanning && net != NULL)
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
          dm_label_set(s_wifi_state, "%s", DM_I18N_NET_UP " 0.0.0.0");
        }
    }

  if (!dm_net_ap_supported() && s_ap_count == 0 && s_wifi_state != NULL)
    {
      /* 热点不可用是平台限制，进页面就说清楚，别让人以为能开。 */
    }
}

static void dm_page_net_enter(void)
{
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
