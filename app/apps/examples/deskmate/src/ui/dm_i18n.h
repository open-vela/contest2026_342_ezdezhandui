/****************************************************************************
 * apps/examples/deskmate/src/ui/dm_i18n.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Every user-visible string of the DeskMate UI lives here.  Two reasons:
 *
 *   1. translation is a single-file change, and
 *   2. tools/engineering/gen_dm_font.sh extracts the glyph set from this file
 *      to generate the CJK subset font, so no label may be written inline in
 *      a page implementation.
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_DESKMATE_UI_DM_I18N_H
#define __APPS_EXAMPLES_DESKMATE_UI_DM_I18N_H

/* Titles and chrome */

#define DM_I18N_APP_TITLE       "DeskMate"
#define DM_I18N_APP_SUBTITLE    "openvela control panel"

/* Home page */

#define DM_I18N_HOME            "HOME"
#define DM_I18N_SYSTEM_CARD     "System"
#define DM_I18N_BUILD           "Build"
#define DM_I18N_LVGL            "LVGL"
#define DM_I18N_DISPLAY         "Display"
#define DM_I18N_BACKEND         "Backend"
#define DM_I18N_UPTIME          "Uptime"
#define DM_I18N_TOUCH_CARD      "Touch"
#define DM_I18N_TAP_BUTTON      "TAP"
#define DM_I18N_TAP_HINT        "tap to prove input reaches the widgets"
#define DM_I18N_TAP_COUNT       "taps"

/* Placeholders for pages that land in the next milestone.  Kept here so the
 * font subset already covers them.
 */

#define DM_I18N_CAMERA          "CAMERA"
#define DM_I18N_CHAT            "CHAT"
#define DM_I18N_NETWORK         "NETWORK"
#define DM_I18N_SETTINGS        "SETTINGS"
#define DM_I18N_NOT_READY       "not implemented yet"

#endif /* __APPS_EXAMPLES_DESKMATE_UI_DM_I18N_H */
