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

/* Tabs */

#define DM_I18N_TAB_HOME        "Home"
#define DM_I18N_TAB_CAMERA      "Camera"
#define DM_I18N_TAB_CHAT        "Chat"
#define DM_I18N_TAB_NET         "Network"
#define DM_I18N_TAB_SETTINGS    "Settings"

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
#define DM_I18N_STATE_CARD      "Capabilities"
#define DM_I18N_CAMERA_STATE    "Camera"
#define DM_I18N_NET_STATE       "Network"
#define DM_I18N_AGENT_STATE     "Agent"

/* Camera page */

#define DM_I18N_CAMERA_TITLE    "Camera preview"
#define DM_I18N_CAMERA_SYNTH    "synthetic source (no sensor on this target)"
#define DM_I18N_CAMERA_V4L2     "V4L2 /dev/video0"
#define DM_I18N_CAMERA_FRAMES   "frames"
#define DM_I18N_CAMERA_MOTION   "motion"
#define DM_I18N_CAPTURE         "Capture"
#define DM_I18N_ASK_VISION      "Ask the agent"

/* Chat page */

#define DM_I18N_CHAT_TITLE      "Conversation"
#define DM_I18N_CHAT_HINT       "pick a prompt or type below"
#define DM_I18N_CHAT_SEND       "Send"
#define DM_I18N_CHAT_CLEAR      "Clear"
#define DM_I18N_CHAT_YOU        "You"
#define DM_I18N_CHAT_AGENT      "DeskMate"

/* Network page */

#define DM_I18N_NET_TITLE       "Network"
#define DM_I18N_NET_IFACES      "Interfaces"
#define DM_I18N_NET_WIFI        "WiFi"
#define DM_I18N_NET_SCAN        "Scan"
#define DM_I18N_NET_CONNECT     "Connect"
#define DM_I18N_NET_NO_WIFI     "no wireless interface"
#define DM_I18N_NET_NO_AP       "no access point found"
#define DM_I18N_NET_NONE        "no interface"
#define DM_I18N_NET_LINKED      "associated with"

/* Settings page */

#define DM_I18N_SETTINGS_TITLE  "Settings"
#define DM_I18N_AGENT_ENDPOINT  "Agent endpoint"
#define DM_I18N_SELFTEST        "Run self-test"
#define DM_I18N_SNAPSHOT        "Save snapshot"
#define DM_I18N_ABOUT           "About"

#endif /* __APPS_EXAMPLES_DESKMATE_UI_DM_I18N_H */
