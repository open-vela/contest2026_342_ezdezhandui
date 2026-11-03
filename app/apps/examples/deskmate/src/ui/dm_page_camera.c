/****************************************************************************
 * apps/examples/deskmate/src/ui/dm_page_camera.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Camera page: a live preview canvas fed straight from the camera HAL buffer,
 * a frame/motion readout, and the two actions the product narrative needs
 * (keep a frame, hand the picture to the agent).
 *
 * The motion figure is a plain frame difference between consecutive previews.
 * It is deliberately not called "person detection": it drives the proactive
 * event, and the document says exactly what it is.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/clock.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <lvgl/lvgl.h>

#include "agent/dm_agent.h"
#include "dm_config.h"
#include "dm_log.h"
#include "hal/dm_camera.h"
#include "ui/dm_i18n.h"
#include "ui/dm_page.h"
#include "ui/dm_theme.h"
#include "ui/dm_widgets.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/


/****************************************************************************
 * Private Data
 ****************************************************************************/

static lv_obj_t *s_root;
static lv_obj_t *s_canvas;
static lv_obj_t *s_info;
static lv_obj_t *s_stats;
static uint16_t *s_fallback;
static int  s_frames;
static int  s_motion;
static bool s_motion_latched;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void dm_page_camera_update(void);

/* 预览节流与计量：主循环每轮都调 update()，不节流就会把 UI 拖死。 */

static uint32_t s_preview_ms = DM_CAMERA_PREVIEW_MS;
static uint32_t s_last_draw_ms;
static uint32_t s_cost_us;       /* 累计耗时（us） */
static uint32_t s_cost_n;
static uint32_t s_report_ms;
static uint32_t s_calls;

/* 诊断开关：冻结预览（不改 draw_buf->data），用来验证"每帧换图源是否触发整屏重绘"。 */

int g_freeze_preview;

uint32_t dm_page_camera_calls(void)
{
  return s_calls;
}

uint32_t dm_page_camera_frames(void)
{
  return s_frames;
}

uint32_t dm_page_camera_cost_us(void)
{
  return (s_cost_n > 0) ? (s_cost_us / s_cost_n) : 0;
}

void dm_page_camera_set_fps(int fps)
{
  s_preview_ms = (fps > 0) ? (uint32_t)(1000 / fps) : 0;
  DM_LOG("camera: preview throttled to %d fps (%u ms)", fps, s_preview_ms);
}

static void dm_page_camera_capture_cb(lv_event_t *event)
{
  const struct dm_camera_frame_s *frame;
  char path[96];
  FILE *fp;

  if (lv_event_get_code(event) != LV_EVENT_CLICKED)
    {
      return;
    }

  frame = dm_camera_last();
  if (frame == NULL || frame->pixels == NULL)
    {
      DM_WARN("camera: nothing to capture yet");
      return;
    }

  (void)mkdir("/tmp/deskmate", 0777);

  snprintf(path, sizeof(path), "/tmp/deskmate/capture-%d.rgb565", frame->seq);

  fp = fopen(path, "wb");
  if (fp == NULL)
    {
      DM_WARN("camera: cannot write %s (%d)", path, errno);
      return;
    }

  (void)fwrite(frame->pixels, 2, (size_t)frame->width * frame->height, fp);
  fclose(fp);

  DM_LOG("camera: captured %s (%dx%d)", path, frame->width, frame->height);
}

static void dm_page_camera_ask_cb(lv_event_t *event)
{
  char prompt[160];

  if (lv_event_get_code(event) != LV_EVENT_CLICKED)
    {
      return;
    }

  snprintf(prompt, sizeof(prompt), DM_I18N_CAMERA_ASK,
           s_frames, dm_camera_backend(), DM_CAMERA_PREVIEW_W,
           DM_CAMERA_PREVIEW_H);

  (void)dm_agent_send(prompt);
}

static int dm_page_camera_create(lv_obj_t *parent, int w, int h)
{
  lv_obj_t *card;
  const struct dm_camera_frame_s *first;
  int info_w = 300;

  s_root = dm_page_root(parent, w, h);

  if (dm_camera_create(DM_CAMERA_PREVIEW_W, DM_CAMERA_PREVIEW_H) != 0)
    {
      /* 摄像头关掉/不可用时页面仍然要建起来（只显示说明），否则路由会失败。 */

      dm_label_at(s_root, DM_I18N_CAMERA_ABSENT, DM_FONT_BODY,
                  lv_color_hex(DM_COL_ERR), 0, 0);
      dm_label_at(s_root, DM_I18N_CAMERA_OFF_HINT, DM_FONT_SMALL,
                  lv_color_hex(DM_COL_MUTED), 0, 48);
      DM_LOG("ui: camera page built without a camera");
      return 0;
    }

  (void)dm_camera_poll(NULL);
  first = dm_camera_last();

  if (first == NULL || first->pixels == NULL)
    {
      /* 真机上首帧可能还没就绪：先给一块黑底缓冲，别让 canvas 拿到 NULL。 */

      s_fallback = calloc((size_t)DM_CAMERA_PREVIEW_W * DM_CAMERA_PREVIEW_H, 2);

      if (s_fallback == NULL)
        {
          dm_label(s_root, DM_I18N_CAMERA_ABSENT, DM_FONT_BODY,
                   lv_color_hex(DM_COL_ERR));
          return -1;
        }
    }

  card = dm_card(s_root, DM_MARGIN, DM_MARGIN,
                 w - 2 * DM_MARGIN - info_w - DM_MARGIN, h - 2 * DM_MARGIN,
                 lv_color_hex(DM_COL_CARD));

  s_canvas = lv_canvas_create(card);
  lv_canvas_set_buffer(s_canvas,
                       (void *)((first != NULL && first->pixels != NULL) ?
                                first->pixels : s_fallback),
                       DM_CAMERA_PREVIEW_W, DM_CAMERA_PREVIEW_H,
                       LV_COLOR_FORMAT_RGB565);
  /* 画布必须**定死 1:1 尺寸**：父卡片是 flex 布局，会把画布拉伸到卡片宽度，
   * LVGL 就得每帧做图像缩放（软件渲染），真机实测这会把单轮 UI 忙时拖到
   * 200~340ms。显式设定尺寸 + 移出布局流，绘制就退化成一次 320x180 的 blit。
   */

  lv_obj_set_size(s_canvas, DM_CAMERA_PREVIEW_W, DM_CAMERA_PREVIEW_H);
  lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_FLOATING);
  lv_obj_align(s_canvas, LV_ALIGN_TOP_LEFT, 0, 0);

  /* Right column: what the pipeline is doing. */

  card = dm_card_stack(s_root, w - DM_MARGIN - info_w, DM_MARGIN, info_w,
                       h - 2 * DM_MARGIN, lv_color_hex(DM_COL_CARD2));

  dm_label(card, DM_I18N_CAMERA_TITLE, DM_FONT_BODY,
           lv_color_hex(DM_COL_ACCENT));

  s_info  = dm_label(card, "", DM_FONT_SMALL, lv_color_hex(DM_COL_TEXT));
  s_stats = dm_label(card, "", DM_FONT_SMALL, lv_color_hex(DM_COL_MUTED));

  /* 固定宽度：文字变化时尺寸不变，上层（flex 卡片/页面）就不会重算布局。
   * 真机实测：自动尺寸标签一改文字就整屏失效 → 单次 1.2MB framebuffer 回写。
   */

  lv_obj_set_width(s_info, info_w - 2 * DM_MARGIN);
  lv_obj_set_width(s_stats, info_w - 2 * DM_MARGIN);

  /* **高度也固定**：这两行文字每秒变一次，如果高度随内容变，LVGL 会重跑父容器
   * 的 flex 布局 → 较大脏区 → 一次上百毫秒的刷写尖峰（真机实测 180ms）。
   * 固定成两行高度之后，文字变化只失效标签自己。
   */

  lv_obj_set_height(s_info, lv_font_get_line_height(DM_FONT_SMALL) * 2 + 6);
  lv_obj_set_height(s_stats, lv_font_get_line_height(DM_FONT_SMALL) * 2 + 6);

  (void)dm_button(card, DM_I18N_CAPTURE, 180, 46,
                  dm_page_camera_capture_cb, NULL);
  (void)dm_button(card, DM_I18N_ASK_VISION, 180, 46,
                  dm_page_camera_ask_cb, NULL);

  dm_page_camera_update();

  DM_LOG("ui: camera page built");
  return 0;
}

static void dm_page_camera_update(void)
{
  struct dm_camera_frame_s frame;
  const char *source;

  memset(&frame, 0, sizeof(frame));

  s_calls++;

  if (s_root == NULL || s_canvas == NULL)
    {
      return;
    }

  /* 节流：没到下一帧时间就什么都不做（采集、转换、检测、重绘都是大头）。 */

  if (s_preview_ms > 0)
    {
      uint32_t now0 = (uint32_t)TICK2MSEC(clock_systime_ticks());

      if (s_last_draw_ms != 0 && now0 - s_last_draw_ms < s_preview_ms)
        {
          return;
        }

      s_last_draw_ms = now0;
    }

  uint32_t cost_begin = (uint32_t)TICK2MSEC(clock_systime_ticks());

  if (dm_camera_poll(&frame) == 0 && frame.pixels != NULL)
    {
      s_frames++;

      /* 运动占比由**采集线程**算好（C3：dm_ui_tick() 里不允许 O(像素) 循环）。 */

      s_motion = dm_camera_motion();

      if (!s_motion_latched && s_motion >= DM_CAMERA_MOTION_PCT)
        {
          s_motion_latched = true;
          dm_agent_push_proactive(DM_I18N_MOTION_EVENT);
          DM_LOG("camera: motion %d%% -> proactive event", s_motion);
        }

      /* 采集线程会换缓冲发布，所以每帧都要把画布指到"最近完成"的那块。
       *
       * 注意：**不能**用 lv_canvas_set_buffer() —— 它会改画布尺寸，触发上层布局
       * 重算，导致整屏失效（真机实测：单次刷写 614400 px = 整屏，一次就是 1.2MB
       * 的 framebuffer 回写 ≈ 140ms 忙时尖峰）。这里原地改 draw_buf 的 data 指针，
       * 尺寸/步长都不变，只会失效画布那一小块。
       */

      {
        lv_draw_buf_t *db = lv_canvas_get_draw_buf(s_canvas);

        if (db != NULL && !g_freeze_preview)
          {
            db->data = (void *)frame.pixels;
          }
      }

      lv_obj_invalidate(s_canvas);
    }

  source = (frame.pixels != NULL && frame.synthetic) ?
           DM_I18N_CAMERA_SYNTH : DM_I18N_CAMERA_V4L2;

  {
    uint32_t cost = (uint32_t)TICK2MSEC(clock_systime_ticks()) - cost_begin;

    s_cost_us += cost * 1000u;
    s_cost_n++;

    if (s_report_ms == 0 ||
        (uint32_t)TICK2MSEC(clock_systime_ticks()) - s_report_ms >= 5000u)
      {
        s_report_ms = (uint32_t)TICK2MSEC(clock_systime_ticks());

        if (s_cost_n > 0)
          {
            DM_LOG("camera: frames=%u cost=%u us/frame throttle=%u ms",
                   s_cost_n, s_cost_us / s_cost_n, s_preview_ms);
          }

        s_cost_us = 0;
        s_cost_n  = 0;
      }
  }

  /* 文字每帧都改会让自动尺寸标签触发布局重算 → **整屏失效** → 一次 1.2MB
   * 的 framebuffer 回写（真机实测 ~140ms 忙时尖峰）。降频到 1 秒一次，
   * 每帧只失效画布那一小块。
   */

  if (s_info != NULL)
    {
      static uint32_t info_ms;

      if (info_ms == 0 ||
          (uint32_t)TICK2MSEC(clock_systime_ticks()) - info_ms >= 1000u)
        {
          info_ms = (uint32_t)TICK2MSEC(clock_systime_ticks());
          dm_label_set(s_info, "%s %s\n%s %d\n", DM_I18N_CAMERA_STATE,
                       dm_camera_backend(), DM_I18N_CAMERA_FRAMES, s_frames);
        }
    }

  if (s_stats != NULL)
    {
      /* 同上：**不能每帧写**。这个标签和画布在同一个卡片里，它一脏就会和画布
       * 合并成一个大脏区（实测平均 200k px/次刷写）。文字变了才写即可。
       */

      static char last_stats[96];
      char now_stats[96];

      snprintf(now_stats, sizeof(now_stats), "%s %d%%\n%s",
               DM_I18N_CAMERA_MOTION, s_motion, source);

      if (strcmp(now_stats, last_stats) != 0)
        {
          strlcpy(last_stats, now_stats, sizeof(last_stats));
          lv_label_set_text(s_stats, now_stats);
        }
    }
}

static void dm_page_camera_enter(void)
{
  dm_page_camera_update();
}

dm_page_t dm_page_camera =
{
  .id     = "camera",
  .title  = DM_I18N_TAB_CAMERA,
  .create = dm_page_camera_create,
  .update = dm_page_camera_update,
  .enter  = dm_page_camera_enter,
  .root   = NULL,
  .built  = false,
};
