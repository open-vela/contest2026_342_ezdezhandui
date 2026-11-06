/****************************************************************************
 * apps/examples/deskmate/src/hal/dm_touch.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Touch half of the DeskMate HAL.
 *
 * The board's touchscreen (/dev/input0) is attached by the display backend
 * through the LVGL NuttX glue.  On top of that this module always registers a
 * script-driven pointer indev, so the UI can be driven without any human and
 * without any input device: that is what makes the simulator and the goldfish
 * emulator runs scriptable, and it is also what dm_selftest uses to prove
 * that taps reach the widgets.
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_DESKMATE_HAL_DM_TOUCH_H
#define __APPS_EXAMPLES_DESKMATE_HAL_DM_TOUCH_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Register the script-driven pointer indev.  Returns 0 on success. */

int dm_touch_create(void);

/* 让硬件轮询线程退出（容器任务退出前必须调用） */
void dm_touch_shutdown(void);

/* Queue one pointer sample.  pressed=true is a touch-down, false a lift. */

void dm_touch_inject(int x, int y, bool pressed);

/* Queue a complete tap (down then up) at a point. */

void dm_touch_tap(int x, int y);

/* Number of queued samples not yet consumed by LVGL (for assertions). */

int dm_touch_pending(void);

/* Number of taps injected since start, and the last tap coordinates. */

unsigned int dm_touch_tap_count(void);
void dm_touch_last_tap(int *x, int *y);

/* 驱动侧采样统计（见 dm_touch.c 的实现说明）。
 * 各字段含义：
 *   ticks       定时器触发次数（采样尝试数）
 *   xfers       实际完成的 I2C 采样事务数
 *   xfer_err    失败的采样事务数
 *   events      产出的 DOWN/MOVE/UP 事件数
 *   lock_waits  worker 等 devlock 的次数
 *   lock_ms_max 等 devlock 的最长时间（ms）
 *   xfer_us_max 单次采样事务的最长时间（us）
 *   hist[8]     采样事务耗时直方图（2/5/10/20/50/100/500ms 分桶）
 *
 * 返回 0 表示拿到数据；负值表示这条路径上没有 GT9xx 驱动。
 */

struct dm_touch_drv_stats_s
{
  uint32_t ticks;
  uint32_t xfers;
  uint32_t xfer_err;
  uint32_t events;
  uint32_t lock_waits;
  uint32_t lock_ms_max;
  uint32_t xfer_us_max;
  uint32_t hist[8];
};

int dm_touch_driver_stats(struct dm_touch_drv_stats_s *out);

#endif /* __APPS_EXAMPLES_DESKMATE_HAL_DM_TOUCH_H */
