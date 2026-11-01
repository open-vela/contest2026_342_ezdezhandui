/****************************************************************************
 * apps/examples/deskmate/src/hal/dm_touch.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Script-driven pointer indev: see dm_touch.h.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <string.h>

#include <lvgl/lvgl.h>

#include "dm_log.h"
#include "hal/dm_touch.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define DM_TOUCH_QUEUE_LEN 16

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct dm_touch_sample_s
{
  int16_t x;
  int16_t y;
  bool    pressed;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static lv_indev_t *g_indev;
static struct dm_touch_sample_s g_queue[DM_TOUCH_QUEUE_LEN];
static volatile int g_head;      /* Next write slot */
static volatile int g_tail;      /* Next read slot */
static unsigned int g_taps;
static int g_last_x;
static int g_last_y;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int dm_touch_count(void)
{
  int count = g_head - g_tail;

  if (count < 0)
    {
      count += DM_TOUCH_QUEUE_LEN;
    }

  return count;
}

static void dm_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
  (void)indev;

  if (dm_touch_count() > 0)
    {
      struct dm_touch_sample_s *s = &g_queue[g_tail];

      data->point.x = s->x;
      data->point.y = s->y;
      data->state   = s->pressed ? LV_INDEV_STATE_PRESSED
                                 : LV_INDEV_STATE_RELEASED;

      g_tail = (g_tail + 1) % DM_TOUCH_QUEUE_LEN;
    }
  else
    {
      /* Hold the pointer where it was and report it released, which is what
       * a real panel reports while nobody touches it.
       */

      data->point.x = g_last_x;
      data->point.y = g_last_y;
      data->state   = LV_INDEV_STATE_RELEASED;
    }

  data->continue_reading = false;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void dm_touch_inject(int x, int y, bool pressed)
{
  int next = (g_head + 1) % DM_TOUCH_QUEUE_LEN;

  if (next == g_tail)
    {
      DM_WARN("touch queue full, dropping sample (%d,%d)", x, y);
      return;
    }

  g_queue[g_head].x       = (int16_t)x;
  g_queue[g_head].y       = (int16_t)y;
  g_queue[g_head].pressed = pressed;
  g_head = next;

  g_last_x = x;
  g_last_y = y;

  DM_LOG("touch %s at (%d,%d)", pressed ? "down" : "up", x, y);
}

void dm_touch_tap(int x, int y)
{
  dm_touch_inject(x, y, true);
  dm_touch_inject(x, y, false);

  g_taps++;
  g_last_x = x;
  g_last_y = y;

  DM_LOG("tap #%u at (%d,%d)", g_taps, x, y);
}

int dm_touch_pending(void)
{
  return dm_touch_count();
}

unsigned int dm_touch_tap_count(void)
{
  return g_taps;
}

void dm_touch_last_tap(int *x, int *y)
{
  if (x != NULL)
    {
      *x = g_last_x;
    }

  if (y != NULL)
    {
      *y = g_last_y;
    }
}

int dm_touch_create(void)
{
  g_head = 0;
  g_tail = 0;

  g_indev = lv_indev_create();
  if (g_indev == NULL)
    {
      DM_ERR("lv_indev_create failed");
      return -1;
    }

  lv_indev_set_type(g_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(g_indev, dm_touch_read_cb);

  DM_LOG("touch: script indev ready");
  return 0;
}
