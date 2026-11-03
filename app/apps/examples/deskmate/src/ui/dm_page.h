/****************************************************************************
 * apps/examples/deskmate/src/ui/dm_page.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Page contract shared by the DeskMate pages and the router.
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_DESKMATE_UI_DM_PAGE_H
#define __APPS_EXAMPLES_DESKMATE_UI_DM_PAGE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>

#include <lvgl/lvgl.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct dm_page_s
{
  const char *id;                       /* Stable name, used by scripts */
  const char *title;                    /* Tab caption */
  int  (*create)(lv_obj_t *parent, int w, int h);
  void (*update)(void);                 /* Called from the UI tick */
  void (*enter)(void);                  /* Called when the page is shown */
  lv_obj_t *root;
  bool built;
};

typedef struct dm_page_s dm_page_t;

/****************************************************************************
 * Public Data
 ****************************************************************************/

extern dm_page_t dm_page_home;
extern dm_page_t dm_page_camera;
extern dm_page_t dm_page_chat;
extern dm_page_t dm_page_net;
extern dm_page_t dm_page_settings;
extern dm_page_t dm_page_agent;

/* Agent 页生成配对二维码的结果（自检断言用，非 static 便于 JTAG 取证）。 */

extern int g_app_agent_qr_ok;

/* 相机预览节流（fps<=0 表示不限速）。 */

void dm_page_camera_set_fps(int fps);

/* 相机页 update() 被调用的次数（诊断用）。 */

uint32_t dm_page_camera_calls(void);

/* 主循环（= dm_ui_tick 调用）累计次数：UI 是否被拖死的直接指标。 */

uint32_t dm_ui_loop_count(void);

/* 两次 dm_ui_tick 之间的最大间隔（ms）——"卡顿/失帧"的直接度量。 */

uint32_t dm_ui_max_gap_ms(void);

/* 每轮忙时（ms）上报与读数：卡顿判据用忙时，而不是含 idle 睡眠的间隔。 */

void dm_ui_note_busy(uint32_t ms);
void dm_ui_note_timer(uint32_t ms);
void dm_ui_note_tick(uint32_t ms);
uint32_t dm_ui_max_busy_ms(void);

/* 发生最大忙时时所在的页面索引（-1 未知）——用来归因"谁把 UI 拖住了"。 */

int dm_ui_max_busy_page(void);

/* 清零上面两个统计（测量窗口开始前调用）。 */

void dm_ui_stats_reset(void);

/* 相机页统计（帧数、平均转换耗时 us）。 */

uint32_t dm_page_camera_frames(void);
uint32_t dm_page_camera_cost_us(void);

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Home page tap target, used by the self-test to hit a real widget. */

int dm_page_home_tap_target(int *x, int *y);
int dm_page_home_tap_count(void);

/* Append an agent message to the conversation.  The page may not have been
 * built yet (the user is on another tab), in which case the text is kept and
 * flushed when the page appears.
 */

void dm_page_chat_append_agent(const char *text);

/* Number of messages the conversation page has rendered. */

int dm_page_chat_bubbles(void);

#endif /* __APPS_EXAMPLES_DESKMATE_UI_DM_PAGE_H */
