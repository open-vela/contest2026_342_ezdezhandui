/****************************************************************************
 * apps/examples/deskmate/src/ui/dm_page_chat.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Conversation page: the visible half of the agent bridge.  Messages come from
 * dm_agent_poll() (LLM replies, offline-responder answers, and proactive
 * events the device raised on its own), and go out through dm_agent_send().
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <lvgl/lvgl.h>

#include "agent/dm_agent.h"
#include "dm_config.h"
#include "dm_log.h"
#include "ui/dm_i18n.h"
#include "ui/dm_page.h"
#include "ui/dm_theme.h"
#include "ui/dm_widgets.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define DM_CHAT_MAX_BUBBLES 24
#define DM_CHAT_PENDING     4

/****************************************************************************
 * Private Data
 ****************************************************************************/

static lv_obj_t *s_root;
static lv_obj_t *s_list;
static lv_obj_t *s_input;
static lv_obj_t *s_keyboard;
static int s_bubbles;
static char s_pending[DM_CHAT_PENDING][512];
static int s_pending_count;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void dm_page_chat_poll(void);
void dm_page_chat_flush_pending(void);

static void dm_chat_bubble(const char *who, const char *text, bool user,
                           bool proactive)
{
  lv_obj_t *bubble;
  lv_color_t bg = user ? lv_color_hex(DM_COL_ACCENT) :
                  (proactive ? lv_color_hex(0x3a2f18) :
                   lv_color_hex(DM_COL_CARD2));
  lv_color_t fg = user ? lv_color_hex(DM_COL_ON_ACCENT) :
                  lv_color_hex(DM_COL_TEXT);

  if (s_list == NULL)
    {
      return;
    }

  if (s_bubbles >= DM_CHAT_MAX_BUBBLES)
    {
      lv_obj_t *oldest = lv_obj_get_child(s_list, 0);

      if (oldest != NULL)
        {
          lv_obj_del(oldest);
          s_bubbles--;
        }
    }

  bubble = lv_obj_create(s_list);
  lv_obj_remove_style_all(bubble);
  lv_obj_set_width(bubble, lv_pct(88));
  lv_obj_set_height(bubble, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(bubble, bg, 0);
  lv_obj_set_style_bg_opa(bubble, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(bubble, 10, 0);
  lv_obj_set_style_pad_all(bubble, 10, 0);
  lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

  {
    lv_obj_t *title = dm_label(bubble, who, DM_FONT_SMALL,
                               lv_color_hex(DM_COL_MUTED));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
  }

  {
    lv_obj_t *body = dm_label(bubble, text, DM_FONT_SMALL, fg);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 20);
  }

  if (user)
    {
      lv_obj_set_style_margin_left(bubble, 40, 0);
    }

  s_bubbles++;

  lv_obj_scroll_to_view(bubble, LV_ANIM_OFF);
}

static void dm_chat_send_text(const char *text)
{
  if (text == NULL || text[0] == '\0')
    {
      return;
    }

  dm_chat_bubble(DM_I18N_CHAT_YOU, text, true, false);
  (void)dm_agent_send(text);
}

static void dm_chat_input_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) == LV_EVENT_READY && s_input != NULL)
    {
      const char *text = lv_textarea_get_text(s_input);

      dm_chat_send_text(text);
      lv_textarea_set_text(s_input, "");
    }
}

/* 常用语上屏：把点中的短语追加到输入框（中文输入法的兜底形态）。
 * 本机没有可用的拼音词库（无 pypinyin / fcitx 库为空），见 docs §23。
 */

static void dm_chat_phrase_cb(lv_event_t *event)
{
  const char *phrase = (const char *)lv_event_get_user_data(event);

  if (s_input == NULL || phrase == NULL)
    {
      return;
    }

  {
    const char *cur = lv_textarea_get_text(s_input);
    char buf[192];

    snprintf(buf, sizeof(buf), "%s%s", cur != NULL ? cur : "", phrase);
    lv_textarea_set_text(s_input, buf);
    lv_textarea_set_cursor_pos(s_input, LV_TEXTAREA_CURSOR_LAST);
  }
}

static void dm_chat_send_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) != LV_EVENT_CLICKED || s_input == NULL)
    {
      return;
    }

  dm_chat_send_text(lv_textarea_get_text(s_input));
  lv_textarea_set_text(s_input, "");

  if (s_keyboard != NULL)
    {
      lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void dm_chat_clear_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) != LV_EVENT_CLICKED || s_list == NULL)
    {
      return;
    }

  lv_obj_clean(s_list);
  s_bubbles = 0;
}

static void dm_chat_preset_cb(lv_event_t *event)
{
  const char *text = (const char *)lv_event_get_user_data(event);

  if (lv_event_get_code(event) != LV_EVENT_CLICKED)
    {
      return;
    }

  dm_chat_send_text(text);
}

static void dm_chat_focus_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) != LV_EVENT_CLICKED || s_keyboard == NULL)
    {
      return;
    }

  lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_keyboard_set_textarea(s_keyboard, s_input);
}

static int dm_page_chat_create(lv_obj_t *parent, int w, int h)
{
  lv_obj_t *card;
  lv_obj_t *row;
  int compose_h = 150;

  static const char *presets[] =
  {
    DM_I18N_PRESET_TIME,
    DM_I18N_PRESET_NET,
    DM_I18N_PRESET_CAMERA,
    DM_I18N_PRESET_NOTE,
  };

  s_root = dm_page_root(parent, w, h);

  card = dm_card(s_root, DM_MARGIN, DM_MARGIN, w - 2 * DM_MARGIN,
                 h - 2 * DM_MARGIN - compose_h - DM_MARGIN,
                 lv_color_hex(DM_COL_CARD));

  dm_label_at(card, DM_I18N_CHAT_TITLE, DM_FONT_BODY,
              lv_color_hex(DM_COL_ACCENT), 0, 0);

  s_list = lv_obj_create(card);
  lv_obj_remove_style_all(s_list);
  lv_obj_set_pos(s_list, 0, 34);
  lv_obj_set_size(s_list, lv_pct(100),
                  h - 2 * DM_MARGIN - compose_h - DM_MARGIN - 48);
  lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_list, 8, 0);
  lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

  /* Compose area. */

  card = dm_card(s_root, DM_MARGIN, h - DM_MARGIN - compose_h,
                 w - 2 * DM_MARGIN, compose_h, lv_color_hex(DM_COL_CARD2));

  row = lv_obj_create(card);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, lv_pct(100), 40);
  lv_obj_set_style_pad_column(row, 8, 0);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

  for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); i++)
    {
      lv_obj_t *btn = dm_button(row, presets[i], 190, 36,
                                dm_chat_preset_cb, (void *)presets[i]);

      lv_obj_set_style_bg_color(btn, lv_color_hex(DM_COL_ACCENT), 0);
    }

  s_input = lv_textarea_create(card);
  lv_obj_set_size(s_input, w - 2 * DM_MARGIN - 160, 44);
  lv_obj_align(s_input, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_textarea_set_one_line(s_input, true);
  lv_textarea_set_placeholder_text(s_input, DM_I18N_CHAT_HINT);
  lv_obj_add_event_cb(s_input, dm_chat_focus_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(s_input, dm_chat_input_cb, LV_EVENT_READY, NULL);

  {
    lv_obj_t *send = dm_button(card, DM_I18N_CHAT_SEND, 100, 44,
                               dm_chat_send_cb, NULL);
    lv_obj_align(send, LV_ALIGN_BOTTOM_RIGHT, -110, 0);

    (void)dm_button(card, DM_I18N_CHAT_CLEAR, 100, 44, dm_chat_clear_cb, NULL);
  }

  /* 常用语面板：一排 6 个，点一下追加到输入框（中文字体已覆盖 3755 字） */

  {
    static const char *phrases[] =
    {
      "你好", "今天天气怎么样", "打开相机",
      "记一下明早九点开会", "现在几点", "谢谢"
    };

    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, w - 2 * DM_MARGIN, 40);
    lv_obj_align(row, LV_ALIGN_BOTTOM_LEFT, 0, -56);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);

    for (unsigned k = 0; k < sizeof(phrases) / sizeof(phrases[0]); k++)
      {
        lv_obj_t *b = dm_button(row, phrases[k], 150, 40,
                                dm_chat_phrase_cb, (void *)phrases[k]);

        lv_obj_set_style_text_font(b, DM_FONT_SMALL, 0);
      }
  }

  s_keyboard = lv_keyboard_create(s_root);
  lv_obj_set_size(s_keyboard, w - 2 * DM_MARGIN, 200);
  lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, -DM_MARGIN);
  lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);

  dm_chat_bubble(DM_I18N_CHAT_AGENT, DM_I18N_CHAT_READY, false, false);

  DM_LOG("ui: chat page built");
  return 0;
}

static void dm_page_chat_poll(void)
{
  if (s_root == NULL)
    {
      return;
    }

  dm_page_chat_flush_pending();
}

void dm_page_chat_append_agent(const char *text)
{
  if (text == NULL || text[0] == '\0')
    {
      return;
    }

  if (s_root == NULL)
    {
      /* The user is on another tab: keep it until the page is built. */

      if (s_pending_count < DM_CHAT_PENDING)
        {
          strlcpy(s_pending[s_pending_count++], text, sizeof(s_pending[0]));
        }

      return;
    }

  dm_chat_bubble(DM_I18N_CHAT_AGENT, text, false, false);
}

void dm_page_chat_flush_pending(void)
{
  int i;

  if (s_root == NULL)
    {
      return;
    }

  for (i = 0; i < s_pending_count; i++)
    {
      dm_chat_bubble(DM_I18N_CHAT_AGENT, s_pending[i], false, false);
    }

  s_pending_count = 0;
}

int dm_page_chat_bubbles(void)
{
  return s_bubbles;
}

static void dm_page_chat_enter(void)
{
  dm_page_chat_flush_pending();
}

dm_page_t dm_page_chat =
{
  .id     = "chat",
  .title  = DM_I18N_TAB_CHAT,
  .create = dm_page_chat_create,
  .update = dm_page_chat_poll,
  .enter  = dm_page_chat_enter,
  .root   = NULL,
  .built  = false,
};
