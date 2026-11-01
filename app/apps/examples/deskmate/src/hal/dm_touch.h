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

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Register the script-driven pointer indev.  Returns 0 on success. */

int dm_touch_create(void);

/* Queue one pointer sample.  pressed=true is a touch-down, false a lift. */

void dm_touch_inject(int x, int y, bool pressed);

/* Queue a complete tap (down then up) at a point. */

void dm_touch_tap(int x, int y);

/* Number of queued samples not yet consumed by LVGL (for assertions). */

int dm_touch_pending(void);

/* Number of taps injected since start, and the last tap coordinates. */

unsigned int dm_touch_tap_count(void);
void dm_touch_last_tap(int *x, int *y);

#endif /* __APPS_EXAMPLES_DESKMATE_HAL_DM_TOUCH_H */
