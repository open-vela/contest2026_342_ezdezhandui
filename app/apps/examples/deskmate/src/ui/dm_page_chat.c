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

/* dm_card() 的 border(1) + pad(18) ⇒ 内容区从卡片左上内缩 19px。 */

#define DM_CARD_INSET       19

/* 合成区高度：必须容得下 3 行 —— 预设 36 + 8 + 常用语 40 + 8 + 输入 44 = 136，
 * 加上下各 19px 的 border+pad。
 *
 * 原来是 150（内容区只有 112），三行共 122 放不下 ⇒ 常用语行直接压到预设行上、
 * 输入框与发送键互相覆盖 —— 这是"界面很乱"的主因。
 */

#define DM_CHAT_COMPOSE_H   176

/* 键盘高度。显示时合成卡上移到键盘之上，输入框不会被遮住。 */

#define DM_CHAT_KEYBOARD_H  190

/****************************************************************************
 * Private Data
 ****************************************************************************/

static lv_obj_t *s_root;
static lv_obj_t *s_list;
static lv_obj_t *s_input;
static lv_obj_t *s_keyboard;
static lv_obj_t *s_compose;
static int s_page_h;
static int s_bubbles;

/* 首条"已就绪"提示气泡（只此一条会被在线状态变化替换），以及上一次已知的
 * 在线状态。原来这条提示是写死的"离线应答后端"，ws 连上之后也不会变。
 */

static lv_obj_t *s_ready_body;
static bool s_ready_online;

static char s_pending[DM_CHAT_PENDING][512];
static int s_pending_count;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void dm_page_chat_poll(void);
void dm_page_chat_flush_pending(void);

/* 键盘显示/隐藏：隐藏 = 恢复合成卡的原始位置；显示 = 合成卡整体上移到键盘
 * 之上，保证输入框与发送/清空键始终完整可见（之前键盘直接盖在合成卡上，
 * 输入框只剩底部 21px 露出来）。
 */

static void dm_chat_keyboard_show(bool show)
{
  if (s_keyboard == NULL)
    {
      return;
    }

  if (show)
    {
      if (s_input != NULL)
        {
          lv_keyboard_set_textarea(s_keyboard, s_input);
        }

      lv_obj_clear_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);

      if (s_compose != NULL)
        {
          int y = s_page_h - DM_CHAT_KEYBOARD_H - DM_CHAT_COMPOSE_H;

          if (y < 0)
            {
              y = 0;
            }

          lv_obj_set_y(s_compose, y);
        }
    }
  else
    {
      lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);

      if (s_compose != NULL)
        {
          lv_obj_set_y(s_compose, s_page_h - DM_MARGIN - DM_CHAT_COMPOSE_H);
        }
    }
}

static lv_obj_t *dm_chat_bubble(const char *who, const char *text, bool user,
                               bool proactive)
{
  lv_obj_t *bubble;
  lv_obj_t *body;
  lv_color_t bg = user ? lv_color_hex(DM_COL_ACCENT) :
                  (proactive ? lv_color_hex(0x3a2f18) :
                   lv_color_hex(DM_COL_CARD2));
  lv_color_t fg = user ? lv_color_hex(DM_COL_ON_ACCENT) :
                  lv_color_hex(DM_COL_TEXT);

  if (s_list == NULL)
    {
      return NULL;
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
    /* 标题颜色必须跟着气泡底色走：用户气泡是强调蓝，用 MUTED 的话对比度只有
     * 1.17:1（等于看不见）。
     */

    lv_obj_t *title = dm_label(bubble, who, DM_FONT_SMALL,
                               user ? lv_color_hex(DM_COL_ON_ACCENT) :
                                      lv_color_hex(DM_COL_MUTED));
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
  }

  body = dm_label(bubble, text, DM_FONT_SMALL, fg);
  lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(body, lv_pct(100));
  lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 20);

  if (user)
    {
      lv_obj_set_style_margin_left(bubble, 40, 0);
    }

  s_bubbles++;

  lv_obj_scroll_to_view(bubble, LV_ANIM_OFF);

  return body;
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

  dm_chat_keyboard_show(false);
}

static void dm_chat_clear_cb(lv_event_t *event)
{
  if (lv_event_get_code(event) != LV_EVENT_CLICKED || s_list == NULL)
    {
      return;
    }

  lv_obj_clean(s_list);
  s_bubbles = 0;

  /* 「清空」会连首条提示一起删掉，而 dm_page_chat_poll() 还留着指向它的
   * s_ready_body —— 之后一旦在线状态变化就会对已释放的对象调
   * lv_label_set_text()（use-after-free）。清空后重建首条提示，指针始终有效。
   */

  s_ready_online = strcmp(dm_agent_state(), "online") == 0;
  s_ready_body   = dm_chat_bubble(DM_I18N_CHAT_AGENT,
                                  s_ready_online ? DM_I18N_CHAT_READY_WS :
                                                   DM_I18N_CHAT_READY_LOCAL,
                                  false, false);
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

  dm_chat_keyboard_show(true);
}

/* 键盘的 OK（LV_EVENT_READY）与收起（LV_EVENT_CANCEL）都走这里。
 * 之前一个处理函数都没挂，键盘的收起键完全是空操作。
 */

static void dm_chat_kb_cb(lv_event_t *event)
{
  lv_event_code_t code = lv_event_get_code(event);

  if (code == LV_EVENT_READY)
    {
      if (s_input != NULL)
        {
          dm_chat_send_text(lv_textarea_get_text(s_input));
          lv_textarea_set_text(s_input, "");
        }
    }

  dm_chat_keyboard_show(false);
}

static int dm_page_chat_create(lv_obj_t *parent, int w, int h)
{
  lv_obj_t *card;
  lv_obj_t *row;
  int compose_h = DM_CHAT_COMPOSE_H;
  int card_h;
  int list_h;

  static const char *presets[] =
  {
    DM_I18N_PRESET_TIME,
    DM_I18N_PRESET_NET,
    DM_I18N_PRESET_CAMERA,
    DM_I18N_PRESET_NOTE,
  };

  s_page_h = h;
  s_root = dm_page_root(parent, w, h);

  /* 消息卡：从顶部起，给合成区留 + 底部留一个 DM_MARGIN。
   *
   * 之前这里用 `h - 2*DM_MARGIN - compose_h - DM_MARGIN`，但又在它下面额外
   * 减了 48（那是没有一点依据的拍脑袋常数），结果列表比卡片高 5px 被裁。
   * 现在卡片高度与列表高度都由同一个 card_h 推出来，二者不可能再对不上。
   */

  card_h = h - 3 * DM_MARGIN - compose_h;

  card = dm_card(s_root, DM_MARGIN, DM_MARGIN, w - 2 * DM_MARGIN, card_h,
                 lv_color_hex(DM_COL_CARD));

  dm_label_at(card, DM_I18N_CHAT_TITLE, DM_FONT_BODY,
              lv_color_hex(DM_COL_ACCENT), 0, 0);

  /* 列表高度 = 卡片内容高（card_h - 上下各 19px 的 border+pad）- 标题占用的 34。
   */

  list_h = card_h - 2 * DM_CARD_INSET - 34;
  if (list_h < 40)
    {
      list_h = 40;
    }

  s_list = lv_obj_create(card);
  lv_obj_remove_style_all(s_list);
  lv_obj_set_pos(s_list, 0, 34);
  lv_obj_set_size(s_list, lv_pct(100), list_h);
  lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(s_list, 8, 0);
  lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

  /* 合成区。

   * 内容区（去掉 border+pad 后）是 `w-2*DM_MARGIN-2*DM_CARD_INSET` 宽。
   * 之前所有子控件都按 **屏幕宽 w** 算宽度，比内容区宽出 38px，
   * 而「清空」还因为没写 align 落在父内容原点、正压在第一个预设键上。
   * 现在：宽一律由内容宽推出；每个子控件都显式定位，不再有"默认原点"。
   */

  card = dm_card(s_root, DM_MARGIN, h - DM_MARGIN - compose_h,
                 w - 2 * DM_MARGIN, compose_h, lv_color_hex(DM_COL_CARD2));

  s_compose = card;

  {
    int inner_w = w - 2 * DM_MARGIN - 2 * DM_CARD_INSET;

    /* 行 1：预设（4 × 190 + 3 × 8 = 784 <= 内容宽）。 */

    row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_pos(row, 0, 0);
    lv_obj_set_size(row, inner_w, 36);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    for (size_t i = 0; i < sizeof(presets) / sizeof(presets[0]); i++)
      {
        lv_obj_t *btn = dm_button(row, presets[i], 190, 36,
                                  dm_chat_preset_cb, (void *)presets[i]);

        lv_obj_set_style_bg_color(btn, lv_color_hex(DM_COL_ACCENT), 0);
      }

    /* 行 2：常用语（6 × 150 + 5 × 8 = 940 <= 内容宽，之前按屏幕宽算成 984 溢出）。 */

    row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_pos(row, 0, 44);
    lv_obj_set_size(row, inner_w, 40);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 8, 0);

    {
      static const char *phrases[] =
      {
        "你好", "今天天气怎么样", "打开相机",
        "记一下明早九点开会", "现在几点", "谢谢"
      };

      for (unsigned k = 0; k < sizeof(phrases) / sizeof(phrases[0]); k++)
        {
          lv_obj_t *b = dm_button(row, phrases[k], 150, 40,
                                  dm_chat_phrase_cb, (void *)phrases[k]);

          lv_obj_set_style_text_font(b, DM_FONT_SMALL, 0);
        }
    }

    /* 行 3：输入框 + 发送 + 清空。三者横向排开、互不相交。 */

    s_input = lv_textarea_create(card);

    /* **必须先设字体**：textarea 自己不设 font 时会继承到 LV_FONT_DEFAULT
     * （montserrat，无 CJK），中文与中文占位提示都会被画成空心方框、且步进只有
     * 10px（正确字体 14px）—— 这就是"在框里被挤压成乱码"的真因。
     * 网络页的密码框一直是设了字体的（dm_page_net.c），只有这里漏了。
     */

    lv_obj_set_style_text_font(s_input, DM_FONT_BODY, 0);
    lv_textarea_set_one_line(s_input, true);

    /* one_line 会把高度覆盖成 LV_SIZE_CONTENT，所以高度必须放在它**之后**设。 */

    lv_obj_set_pos(s_input, 0, 98);
    lv_obj_set_size(s_input, inner_w - 216, 44);
    lv_textarea_set_placeholder_text(s_input, DM_I18N_CHAT_HINT);
    lv_obj_add_event_cb(s_input, dm_chat_focus_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_input, dm_chat_input_cb, LV_EVENT_READY, NULL);

    {
      lv_obj_t *send = dm_button(card, DM_I18N_CHAT_SEND, 100, 44,
                                 dm_chat_send_cb, NULL);

      lv_obj_set_pos(send, inner_w - 208, 98);
    }

    {
      lv_obj_t *clear = dm_button(card, DM_I18N_CHAT_CLEAR, 100, 44,
                                  dm_chat_clear_cb, NULL);

      lv_obj_set_pos(clear, inner_w - 100, 98);
    }
  }

  /* 键盘：默认隐藏；显示时把合成卡整体上移到键盘之上，让输入框始终可见。
   * 之前只在 s_input 上挂了 CLICKED/READY，键盘自己的关闭键（LV_EVENT_CANCEL）
   * 没有任何处理函数，所以键盘根本收不起来。照网络页的做法补齐两个回调。
   */

  s_keyboard = lv_keyboard_create(s_root);
  lv_obj_set_size(s_keyboard, w, DM_CHAT_KEYBOARD_H);
  lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_event_cb(s_keyboard, dm_chat_kb_cb, LV_EVENT_READY, NULL);
  lv_obj_add_event_cb(s_keyboard, dm_chat_kb_cb, LV_EVENT_CANCEL, NULL);
  lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);

  /* 首条提示必须反映**当下**的连接状态：原来无论连没连上都写"当前是离线
   * 应答后端"，于是 --agent-spawn 连上 ws 之后（顶栏已显示"已连服务"），
   * 对话区还在说自己是离线 —— 自相矛盾。
   */

  s_ready_online = strcmp(dm_agent_state(), "online") == 0;
  s_ready_body = dm_chat_bubble(DM_I18N_CHAT_AGENT,
                                s_ready_online ? DM_I18N_CHAT_READY_WS :
                                                 DM_I18N_CHAT_READY_LOCAL,
                                false, false);

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

  /* 首条提示跟随在线状态变化。只动这一条标签 —— 不要整页重建，
   * lv_obj_clean() 会把用户正在读的历史一起清掉。
   *
   * lv_obj_is_valid() 这道判断不能省：气泡数超过 DM_CHAT_MAX_BUBBLES 时
   * dm_chat_bubble() 会删掉最旧的一条，而首条提示正是最旧的那条 ——
   * 删完之后 s_ready_body 就是野指针，再写入就是 use-after-free。
   */

  if (s_ready_body != NULL && lv_obj_is_valid(s_ready_body))
    {
      bool online = strcmp(dm_agent_state(), "online") == 0;

      if (online != s_ready_online)
        {
          s_ready_online = online;
          lv_label_set_text(s_ready_body, online ? DM_I18N_CHAT_READY_WS :
                                                  DM_I18N_CHAT_READY_LOCAL);
        }
    }
  else
    {
      s_ready_body = NULL;   /* 已被回收：等下一次建页/清空时再补 */
    }
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

/* 输入框**实际解析到**的字体。s_input 没建好时返回 NULL。
 *
 * 自检用它来做"输入框必须有中文字形"的断言：LV_STYLE_TEXT_FONT 是可继承的，
 * 但只要没人显式设过，最终就会落到 LV_FONT_DEFAULT（montserrat，无 CJK），
 * 中文会被画成一排空心方框 —— 这正是"输入框里显示乱码"的真因。
 */

const struct _lv_font_t *dm_page_chat_input_font(void)
{
  if (s_input == NULL)
    {
      return NULL;
    }

  return (const struct _lv_font_t *)
         lv_obj_get_style_text_font(s_input, LV_PART_MAIN);
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
