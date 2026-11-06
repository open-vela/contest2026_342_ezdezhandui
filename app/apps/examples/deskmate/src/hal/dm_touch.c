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
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include <nuttx/clock.h>
#include <nuttx/input/touchscreen.h>

#if defined(CONFIG_INPUT_GT9XX)
#  include <nuttx/input/gt9xx.h>
#endif

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

/* 这里原本有 g_hw_hold_until（120ms 时间锁存）和 g_cb_hold（4 次回调锁存）。
 *
 * 删掉的理由是实测：驱动侧改造前，一次 read 的 p50 是 17.7ms、最坏 204ms、
 * 采样率只有 7.1Hz；改造后驱动自己保证 DOWN→MOVE→UP 的事件语义，UI 侧不需要
 * 再"猜"手指还在不在。
 *
 * 而且这两个常数本来就是错的量级：120ms 比最坏的一次读还短（710ms），4 次回调
 * 在 30~50ms 的回调周期下只有 120~200ms —— 也就是说它们不但没解决问题，
 * 还会把"已经抬手"继续报成按下（g_cb_hold 的递减与 g_hw_pressed 的赋值互相覆盖），
 * 表现为"抬手后界面还在拖"。
 */

/* 诊断（JTAG 可读）：LVGL 回调侧 —— 证明"快照 -> 回调 -> UI"这一段。
 *   cb_calls   ：回调被 LVGL 调用的次数
 *   cb_pressed ：其中"按硬件快照报 PRESSED"的次数（手指按下时应增长）
 *   cb_x/cb_y  ：最近一次上报的坐标
 */

volatile uint32_t g_dbg_touch_cb_calls;
volatile uint32_t g_dbg_touch_cb_pressed;
volatile uint32_t g_dbg_touch_cb_x;
volatile uint32_t g_dbg_touch_cb_y;

/* 诊断（JTAG 可读）：硬件读次数与耗时（微秒，累计/单次最大） */

volatile uint32_t g_dbg_touch_hw_reads;    /* 发起的 read 次数 */
volatile uint32_t g_dbg_touch_hw_ok;       /* read 返回完整样本的次数 */
volatile uint32_t g_dbg_touch_hw_hits;     /* npoints>0（含松手；驱动把 UP 也报成 1 点） */
volatile uint32_t g_dbg_touch_hw_downs;    /* 其中 flags & TOUCH_DOWN 的**真正按下** */
volatile uint32_t g_dbg_touch_hw_err;      /* 返回长度不符的次数 */
volatile uint32_t g_dbg_touch_hw_again;    /* read 返回 -EAGAIN 的次数（正常） */
volatile uint32_t g_dbg_touch_hw_idle;     /* read 成功但这一拍没有新事件 */
volatile uint32_t g_dbg_touch_hw_us;
volatile uint32_t g_dbg_touch_hw_us_max;

/* 轮询线程停止标志：容器任务（deskmate 作为 NSH 内置命令运行）退出后必须让它退出。
 * 否则旧线程会和新实例抢同一个触摸设备：
 *
 *   2026-11-04 实测：自检退出（DM-EXIT result=0）后再启动 deskmate，`ps` 里出现
 *   **两个 dm-touch**（PID 14 旧 + 45 新），触摸通路随即崩溃。
 *
 * dm_touch_shutdown() 置位、dm_touch_create() 复位；线程每 50ms 检查一次。
 */

static volatile uint32_t g_touch_gen;

static void *dm_touch_hw_thread(void *arg)
{
  FAR struct touch_sample_s *sample;
  struct pollfd pfd;
  uint8_t maxpoint = 1;
  size_t  ssize;
  int fd;

  (void)arg;

  fd = open(DM_INPUT_PATH, O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    {
      DM_ERR("touch: cannot open %s for the poller (errno=%d)", DM_INPUT_PATH,
             errno);
      return NULL;
    }

  /* 与 LVGL 的 nuttx 触摸驱动一致：先问"最多几个点"，再据此定缓冲长度。
   * 之前直接用 sizeof(struct touch_sample_s) 并要求 read 恰好返回该长度，
   * 一旦与驱动的长度判定不一致，read 会被判失败 ⇒ 硬件样本一个都进不来。
   */

  if (ioctl(fd, TSIOC_GETMAXPOINTS, &maxpoint) < 0 || maxpoint == 0)
    {
      DM_ERR("touch: TSIOC_GETMAXPOINTS failed (errno=%d), assume 1 point",
             errno);
      maxpoint = 1;
    }

  ssize  = SIZEOF_TOUCH_SAMPLE_S(maxpoint);
  sample = malloc(ssize);
  if (sample == NULL)
    {
      close(fd);
      return NULL;
    }

  DM_LOG("touch: hardware poller ready (%s, maxpoint=%u, %zu bytes)",
         DM_INPUT_PATH, (unsigned)maxpoint, ssize);

  pfd.fd     = fd;
  pfd.events = POLLIN;

  uint32_t my_gen = g_touch_gen;

  while (g_touch_gen == my_gen)
    {
      /* 等驱动的事件源。这里的 200ms **不是**采样周期，只是退出检查周期。
       *
       * 关键区别：驱动侧已经把 poll_notify() 真正接起来了（gt9xx 的 LPWORK
       * worker 每 8ms 采一次样并唤醒等待者），所以有手指动作时 poll() 立刻返回，
       * 超时值只在"长时间没有任何事件"时决定线程多久回来看一次 g_touch_gen。
       *
       * 改造前 poll_notify() 从未被调用过，任何超时值都等于"必然睡满"，
       * 50ms 直接决定了 7.1Hz 的采样率上限 —— 那个数字是用逻辑分析仪量出来的。
       *
       * 不能用 -1（永久阻塞）：dm_touch_shutdown() 靠 g_touch_gen 通知退出，
       * 线程要是死在 poll 里就永远看不到那个变化，容器任务退出后线程会残留，
       * 并与下一个实例抢同一个 /dev/input0。
       */

      (void)poll(&pfd, 1, 200);

      {
        uint32_t t0 = (uint32_t)TICK2USEC(clock_systime_ticks());
        ssize_t  n  = read(fd, sample, ssize);
        uint32_t dt = (uint32_t)TICK2USEC(clock_systime_ticks()) - t0;

        g_dbg_touch_hw_reads++;
        g_dbg_touch_hw_us += dt;
        if (dt > g_dbg_touch_hw_us_max)
          {
            g_dbg_touch_hw_us_max = dt;
          }

        if (n == (ssize_t)ssize)
          {
            g_dbg_touch_hw_ok++;

            if (sample->npoints > 0)
              {
                g_hw_x = sample->point[0].x;
                g_hw_y = sample->point[0].y;

                /* 驱动现在保证语义：DOWN 之后每一次接触都会报 MOVE，
                 * 松手报恰好一次 UP。所以这里**不需要**时间锁存 —— 只要
                 * 把 flags 里"接触是否仍然成立"如实转成 g_hw_pressed。
                 *
                 * 判据必须是 (DOWN|MOVE)，不能只看 DOWN：只认 DOWN 的话，
                 * 手指按住不动时驱动发的 MOVE 会被当成"没按"，下一拍再被
                 * 自己的 else 分支清掉 —— 这正是原来需要 120ms 锁存去掩盖的
                 * 那个 bug 的根源。
                 */

                if ((sample->point[0].flags & TOUCH_UP) != 0)
                  {
                    g_hw_pressed = 0;
                  }
                else if ((sample->point[0].flags &
                          (TOUCH_DOWN | TOUCH_MOVE)) != 0)
                  {
                    g_hw_pressed = 1;

                    g_dbg_touch_hw_downs++;
                  }

                g_dbg_touch_hw_hits++;

                /* 只在前 3 次**按下**时打印。
                 *
                 * 原来按 "hits<=3" 打，而 hits 现在包含每一拍 MOVE；一旦有人
                 * 按住不放，前 3 条日志会在 30ms 内打完，之后注入坐标、
                 * 排查坐标错误时就什么都看不到了。
                 */

                if (g_dbg_touch_hw_downs <= 3)
                  {
                    DM_LOG("touch: hw sample #%u x=%d y=%d flags=0x%02x",
                           (unsigned)g_dbg_touch_hw_downs, (int)g_hw_x,
                           (int)g_hw_y, (unsigned)sample->point[0].flags);
                  }

                g_hw_seq++;
              }
            else
              {
                /* npoints == 0 现在是"这一拍没有新事件"，**不是**"手指抬起"。
                 *
                 * 旧驱动的 npoints==0 意思是"没按"（它从不发 MOVE），所以
                 * 这里清 g_hw_pressed 是对的；新驱动用 MOVE 维持接触、用 UP
                 * 报告抬起，如果还在这里清零，按住不动时每一拍都会被误判成
                 * 抬手，表现就是"按下后立刻断开、拖不动"。
                 */

                g_dbg_touch_hw_idle++;
              }

            g_hw_have = 1;
          }
        else if (n == -1 && errno == EAGAIN)
          {
            /* 驱动快照正被 worker 持有，这一拍没有新样本。
             *
             * 这**不是**错误，也不该重试：poll 会立刻返回（因为驱动仍然
             * 认为有数据待取），重试就变成忙等。直接进入下一轮 poll 即可。
             */

            g_dbg_touch_hw_again++;
          }
        else
          {
            g_dbg_touch_hw_err++;
            if (g_dbg_touch_hw_err <= 3)
              {
                DM_ERR("touch: read -> %d (want %zu, errno=%d)",
                       (int)n, ssize, errno);
              }
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

  g_dbg_touch_cb_calls++;

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

      g_dbg_touch_cb_pressed++;
      g_dbg_touch_cb_x = (uint32_t)g_hw_x;
      g_dbg_touch_cb_y = (uint32_t)g_hw_y;
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

/****************************************************************************
 * Name: dm_touch_shutdown
 *
 * Description:
 *   让硬件轮询线程退出（见 g_touch_gen 的说明）。容器任务退出前必须调用，
 *   否则线程会越过模块生命周期继续读设备，并与下一个实例抢同一份硬件。
 *
 ****************************************************************************/

void dm_touch_shutdown(void)
{
  g_touch_gen++;               /* 旧线程看到代际变化即退出（见 g_touch_gen 说明） */

  /* 线程的 poll 超时是 200ms（见 dm_touch_hw_thread），这里给它一整轮多一点
   * 的时间退出。没有事件时线程最多 200ms 就会回来看一次 g_touch_gen。
   */

  usleep(250 * 1000);
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

  g_touch_gen++;             /* 新一代：上一次运行的轮询线程会自行退出 */

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

/****************************************************************************
 * Name: dm_touch_fake_hw
 *
 * Description:
 *   把一次"合成按下"直接写进硬件快照（等价于轮询线程刚读到一次按下）。
 *   用途：不依赖手指，验证"快照 → indev 回调 → UI"这一段是通的。
 *
 ****************************************************************************/

void dm_touch_fake_hw(int x, int y)
{
  g_hw_x          = x;
  g_hw_y          = y;
  g_hw_pressed    = 1;
  g_hw_have       = 1;
  g_hw_seq++;

  DM_LOG("touch: fake hw press at (%d,%d)", x, y);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: dm_touch_driver_stats
 *
 * Description:
 *   把 gt9xx 驱动侧的采样统计读出来。返回 0 表示拿到数据，负值表示这块平台
 *   没有 GT9xx（模拟器 / 其它触摸控制器）。
 *
 *   为什么需要：应用侧的 g_dbg_touch_hw_us_max 只能看到"read() 花了多久"，
 *   而 read() 现在只读快照、几乎不花时间。真正的采样健康度在驱动里 ——
 *   定时器是否在跑（ticks）、worker 是否被调度到（xfers）、传输本身多快
 *   （xfer_us_max / hist）、以及有没有被锁挡住（lock_waits）。
 *
 *   这四个数字分开看才有意义：只有 ticks 涨而 xfers 不涨 = LPWORK 被饿死；
 *   xfers 涨但 hist 高 = 总线或控制器慢；两者都正常而事件少 = 帧间丢事件。
 *
 ****************************************************************************/

int dm_touch_driver_stats(struct dm_touch_drv_stats_s *out)
{
#if defined(CONFIG_INPUT_GT9XX)
  if (out == NULL)
    {
      return -1;
    }

  memset(out, 0, sizeof(*out));

  if (gt9xx_stats(&out->ticks, &out->xfers, &out->xfer_err, &out->events,
                  &out->lock_waits, &out->lock_ms_max,
                  &out->xfer_us_max, out->hist) < 0)
    {
      return -1;
    }

  return 0;
#else
  (void)out;
  return -1;
#endif
}
