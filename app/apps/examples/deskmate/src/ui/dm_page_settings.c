/****************************************************************************
 * apps/examples/deskmate/src/ui/dm_page_settings.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Settings page: agent endpoint, a manual snapshot (the evidence path), and
 * the about block.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>

#include <lvgl/lvgl.h>

#include "agent/dm_agent.h"
#include "dm_config.h"
#include "dm_log.h"
#include "hal/dm_display.h"
#include "ui/dm_i18n.h"
#include "ui/dm_page.h"
#include "ui/dm_theme.h"
#include "ui/dm_widgets.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static lv_obj_t *s_root;
static lv_obj_t *s_status;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void dm_page_settings_update(void);

static void dm_settings_snapshot_cb(lv_event_t *event)
{
  char path[96];
  int written;

  if (lv_event_get_code(event) != LV_EVENT_CLICKED)
    {
      return;
    }

  /* 先按"模拟器约定"写到 hostfs（评审能直接拿到 PNG）；真机没有 /host，
   * 挂载失败时退回 tmpfs（CONFIG_LIBC_TMPDIR，P4 板级 bringup 会挂上）。
   *
   * 之前只有 /host 一个路径：真机上点「保存快照」**永远失败**，而失败信息
   * 只写进界面标签，命令行证据里看不到。
   */

  snprintf(path, sizeof(path), DM_SNAP_HOST_DIR "/dm_settings.bin");

  written = dm_display_snapshot(path);

  if (written <= 0)
    {
      snprintf(path, sizeof(path), "/tmp/deskmate/dm_settings.bin");
      written = dm_display_snapshot(path);
    }

  if (written > 0)
    {
      dm_label_set(s_status, DM_I18N_SNAP_WRITTEN, written, path);
      DM_LOG("settings: snapshot written (%d bytes) -> %s", written, path);
    }
  else
    {
      dm_label_set(s_status, DM_I18N_SNAP_FAILED, written);
      DM_WARN("settings: snapshot failed (%d)", written);
    }
}

static void dm_settings_proactive_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) != LV_EVENT_CLICKED)
    {
      return;
    }

  dm_agent_push_proactive(DM_I18N_SETTINGS_ANNOUNCE_TEXT);
  dm_label_set(s_status, "%s", DM_I18N_QUEUED);
}

static int dm_page_settings_create(lv_obj_t *parent, int w, int h)
{
  lv_obj_t *card;
  const struct dm_display_metrics_s *metrics = dm_display_metrics();
  int half = (w - 3 * DM_MARGIN) / 2;

  s_root = dm_page_root(parent, w, h);

  card = dm_card_stack(s_root, DM_MARGIN, DM_MARGIN, half, h - 2 * DM_MARGIN,
                       lv_color_hex(DM_COL_CARD));

  dm_label(card, DM_I18N_SETTINGS_TITLE, DM_FONT_BODY,
           lv_color_hex(DM_COL_ACCENT));

  {
    lv_obj_t *label = dm_label(card, "", DM_FONT_SMALL,
                               lv_color_hex(DM_COL_TEXT));

    dm_label_set(label, "%s: %s (chat id \"deskmate-ui\")",
                 DM_I18N_AGENT_ENDPOINT, dm_agent_state());
  }

  (void)dm_button(card, DM_I18N_SNAPSHOT, 200, 44,
                  dm_settings_snapshot_cb, NULL);
  (void)dm_button(card, DM_I18N_ANNOUNCE, 200, 44,
                  dm_settings_proactive_cb, NULL);

  s_status = dm_label(card, "-", DM_FONT_SMALL, lv_color_hex(DM_COL_MUTED));

  card = dm_card_stack(s_root, 2 * DM_MARGIN + half, DM_MARGIN, half,
                       h - 2 * DM_MARGIN, lv_color_hex(DM_COL_CARD2));

  dm_label(card, DM_I18N_ABOUT, DM_FONT_BODY, lv_color_hex(DM_COL_ACCENT));

  {
    lv_obj_t *label = dm_label(card, "", DM_FONT_SMALL,
                               lv_color_hex(DM_COL_TEXT));

    dm_label_set(label, "%s %s\n%s: %dx%d %dbpp %s\n%s: %s",
                 DM_APP_NAME, DM_APP_VERSION, DM_I18N_DISPLAY,
                 metrics != NULL ? metrics->width : 0,
                 metrics != NULL ? metrics->height : 0,
                 metrics != NULL ? metrics->bpp : 0,
                 metrics != NULL ? metrics->backend : "?",
                 DM_I18N_LVGL, lv_version_info());
  }

  dm_label(card, DM_I18N_ABOUT_TEXT, DM_FONT_SMALL, lv_color_hex(DM_COL_MUTED));

  dm_page_settings_update();

  DM_LOG("ui: settings page built");
  return 0;
}

static void dm_page_settings_update(void)
{
  if (s_root == NULL)
    {
      return;
    }
}

static void dm_page_settings_enter(void)
{
  dm_page_settings_update();
}

dm_page_t dm_page_settings =
{
  .id     = "settings",
  .title  = DM_I18N_TAB_SETTINGS,
  .create = dm_page_settings_create,
  .update = dm_page_settings_update,
  .enter  = dm_page_settings_enter,
  .root   = NULL,
  .built  = false,
};
