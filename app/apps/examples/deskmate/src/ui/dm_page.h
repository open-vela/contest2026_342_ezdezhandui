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
