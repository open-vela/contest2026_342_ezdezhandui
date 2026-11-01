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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define DM_CAMERA_PREVIEW_W  480
#define DM_CAMERA_PREVIEW_H  270
#define DM_CAMERA_MOTION_PCT 6        /* Percent of changed samples */
#define DM_CAMERA_MOTION_DIFF 1200    /* Per-pixel RGB565 delta */

/****************************************************************************
 * Private Data
 ****************************************************************************/

static lv_obj_t *s_root;
static lv_obj_t *s_canvas;
static lv_obj_t *s_info;
static lv_obj_t *s_stats;
static uint16_t *s_prev;
static int  s_frames;
static int  s_motion;
static bool s_motion_latched;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void dm_page_camera_update(void);

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

  snprintf(prompt, sizeof(prompt),
           "Look at the camera frame %d (%s backend, %dx%d) and describe it.",
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
      dm_label(s_root, "camera unavailable", DM_FONT_BODY,
               lv_color_hex(DM_COL_ERR));
      return -1;
    }

  s_prev = malloc((size_t)DM_CAMERA_PREVIEW_W * DM_CAMERA_PREVIEW_H * 2);
  if (s_prev != NULL)
    {
      memset(s_prev, 0, (size_t)DM_CAMERA_PREVIEW_W * DM_CAMERA_PREVIEW_H * 2);
    }

  (void)dm_camera_poll(NULL);
  first = dm_camera_last();

  card = dm_card(s_root, DM_MARGIN, DM_MARGIN,
                 w - 2 * DM_MARGIN - info_w - DM_MARGIN, h - 2 * DM_MARGIN,
                 lv_color_hex(DM_COL_CARD));

  s_canvas = lv_canvas_create(card);
  lv_canvas_set_buffer(s_canvas, (void *)first->pixels,
                       DM_CAMERA_PREVIEW_W, DM_CAMERA_PREVIEW_H,
                       LV_COLOR_FORMAT_RGB565);
  lv_obj_align(s_canvas, LV_ALIGN_TOP_LEFT, 0, 0);

  /* Right column: what the pipeline is doing. */

  card = dm_card_stack(s_root, w - DM_MARGIN - info_w, DM_MARGIN, info_w,
                       h - 2 * DM_MARGIN, lv_color_hex(DM_COL_CARD2));

  dm_label(card, DM_I18N_CAMERA_TITLE, DM_FONT_BODY,
           lv_color_hex(DM_COL_ACCENT));

  s_info  = dm_label(card, "", DM_FONT_SMALL, lv_color_hex(DM_COL_TEXT));
  s_stats = dm_label(card, "", DM_FONT_SMALL, lv_color_hex(DM_COL_MUTED));

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
  size_t samples = (size_t)DM_CAMERA_PREVIEW_W * DM_CAMERA_PREVIEW_H;
  const char *source;

  memset(&frame, 0, sizeof(frame));

  if (s_root == NULL)
    {
      return;
    }

  if (dm_camera_poll(&frame) == 0 && frame.pixels != NULL)
    {
      s_frames++;

      if (s_prev != NULL)
        {
          const size_t step = 7;
          int changed = 0;

          for (size_t i = 0; i + step < samples; i += step)
            {
              int d = (int)s_prev[i] - (int)frame.pixels[i];

              if (d < 0)
                {
                  d = -d;
                }

              if (d > DM_CAMERA_MOTION_DIFF)
                {
                  changed++;
                }
            }

          memcpy(s_prev, frame.pixels, samples * 2);

          s_motion = (int)(((size_t)changed * 100) / (samples / step));

          if (!s_motion_latched && s_motion >= DM_CAMERA_MOTION_PCT)
            {
              s_motion_latched = true;
              dm_agent_push_proactive("Motion detected in front of the camera.");
              DM_LOG("camera: motion %d%% -> proactive event", s_motion);
            }
        }

      lv_obj_invalidate(s_canvas);
    }

  source = (frame.pixels != NULL && frame.synthetic) ?
           DM_I18N_CAMERA_SYNTH : DM_I18N_CAMERA_V4L2;

  if (s_info != NULL)
    {
      dm_label_set(s_info, "%s %s\n%s %d\n", DM_I18N_CAMERA_STATE,
                   dm_camera_backend(), DM_I18N_CAMERA_FRAMES, s_frames);
    }

  if (s_stats != NULL)
    {
      dm_label_set(s_stats, "%s %d%%\n%s", DM_I18N_CAMERA_MOTION, s_motion,
                   source);
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
