/****************************************************************************
 * apps/examples/deskmate/src/ui/dm_ui.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Widget layer of the DeskMate application: a status bar, a page area and a
 * tab bar, with one page per product capability (home, camera, chat, network,
 * settings).
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

/* Periodic refresh (clock, current page, agent messages, toast). */

void dm_ui_tick(void);

/* Pages */

int  dm_ui_page_count(void);
const char *dm_ui_page_id(int index);
int  dm_ui_page_index(const char *id);
int  dm_ui_current(void);
const char *dm_ui_current_id(void);

/* Switch to a page by index or id.  Returns 0 on success.  The page is built
 * the first time it is shown.
 */

int dm_ui_goto(int index);
int dm_ui_goto_id(const char *id);

/* Screen coordinates of a tab button, so a script or the self-test can tap
 * it.  Accepts a page id ("camera") or index as a string ("1").
 */

int dm_ui_tab_target(const char *id, int *x, int *y);

/* Number of children of the active screen: a cheap "did the UI get built"
 * probe for the self-test.
 */

int dm_ui_object_count(void);

/* Short lived notice at the top of the page area (used for proactive events
 * raised while another tab is visible).
 */

void dm_ui_toast(const char *text);

#endif /* __APPS_EXAMPLES_DESKMATE_UI_DM_UI_H */
