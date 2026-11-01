/****************************************************************************
 * apps/examples/deskmate/src/ui/dm_ui.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Widget layer of the DeskMate application.
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_DESKMATE_UI_DM_UI_H
#define __APPS_EXAMPLES_DESKMATE_UI_DM_UI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Build the UI.  dm_display_create() and dm_touch_create() must have run. */

int dm_ui_create(void);

/* Periodic refresh (clock, uptime, counters).  Called from the UI loop. */

void dm_ui_tick(void);

/* Number of taps the widgets have seen. */

int dm_ui_tap_count(void);

/* Screen coordinates of the tap target, so a self-test can hit it. */

int dm_ui_tap_target(int *x, int *y);

/* Number of children of the active screen: a cheap "did the UI get built"
 * probe for the self-test.
 */

int dm_ui_object_count(void);

#endif /* __APPS_EXAMPLES_DESKMATE_UI_DM_UI_H */
