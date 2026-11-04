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

#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include <nuttx/clock.h>
#include <nuttx/input/touchscreen.h>

#include <lvgl/lvgl.h>

#include "dm_config.h"
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

/* ── 硬件触摸：独立线程轮询 /dev/input0，UI 侧只读"最新样本" ──────────────
 *
 * 为什么这么做：LVGL 的设备 indev 会把 read(/dev/input0) 放在
 * lv_timer_handler（UI 线程）里；而这条 I2C0 与相机 SCCB/ES8311 共用，
 * 单次读最坏实测 230~800ms ⇒ 整屏一起卡。搬到本线程后，
 * **I2C 抖动再大也只影响触摸的新鲜度，不影响 UI 出帧**。
 */

static volatile int g_hw_x;
static volatile int g_hw_y;
static volatile int g_hw_pressed;
static volatile int g_hw_have;
static volatile int g_hw_seq;

/* 诊断（JTAG 可读）：硬件读次数与耗时（微秒，累计/单次最大） */

volatile uint32_t g_dbg_touch_hw_reads;
volatile uint32_t g_dbg_touch_hw_us;
volatile uint32_t g_dbg_touch_hw_us_max;

static void *dm_touch_hw_thread(void *arg)
{
  struct touch_sample_s sample;
  struct pollfd pfd;
  int fd;

  (void)arg;

  fd = open(DM_INPUT_PATH, O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    {
      DM_ERR("touch: cannot open %s for the poller", DM_INPUT_PATH);
      return NULL;
    }

  pfd.fd     = fd;
  pfd.events = POLLIN;

  for (;;)
    {
      /* 每个周期都读一次（GT911 是轮询型：没触摸时 read 会返回 npoints==0），
       * 这样触摸状态最多滞后 50ms；也是旧 LVGL 设备 indev 的行为。
       */

      (void)poll(&pfd, 1, 50);

      {
          uint32_t t0 = (uint32_t)TICK2USEC(clock_systime_ticks());
          ssize_t n   = read(fd, &sample, sizeof(sample));
          uint32_t dt = (uint32_t)TICK2USEC(clock_systime_ticks()) - t0;

          g_dbg_touch_hw_reads++;
          g_dbg_touch_hw_us += dt;
          if (dt > g_dbg_touch_hw_us_max)
            {
              g_dbg_touch_hw_us_max = dt;
            }

          if (n == (ssize_t)sizeof(sample))
            {
              if (sample.npoints > 0)
                {
                  g_hw_x       = sample.point[0].x;
                  g_hw_y       = sample.point[0].y;
                  g_hw_pressed = (sample.point[0].flags & TOUCH_DOWN) ? 1 : 0;
                }
              else
                {
                  g_hw_pressed = 0;
                }

              g_hw_have = 1;
              g_hw_seq++;
            }
        }
    }

  return NULL;
}

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
  else if (g_hw_have && g_hw_pressed)
    {
      /* 硬件最新样本（由 dm_touch_hw_thread 在别的线程里读进来的） */

      g_last_x      = g_hw_x;
      g_last_y      = g_hw_y;
      data->point.x = g_hw_x;
      data->point.y = g_hw_y;
      data->state   = LV_INDEV_STATE_PRESSED;
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

  /* 起一个**低优先级**线程做硬件轮询：UI 线程从此不碰 I2C */

  {
    pthread_t tid;
    pthread_attr_t attr;

    pthread_attr_init(&attr);
    (void)pthread_attr_setstacksize(&attr, 2048);

    if (pthread_create(&tid, &attr, dm_touch_hw_thread, NULL) != 0)
      {
        DM_ERR("touch: hardware poller thread not started");
      }
    else
      {
        (void)pthread_setname_np(tid, "dm-touch");
        DM_LOG("touch: hardware poller started (UI 不再直接读 I2C)");
      }

    pthread_attr_destroy(&attr);
  }

  DM_LOG("touch: script indev ready");
  return 0;
}
