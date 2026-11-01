/****************************************************************************
 * apps/examples/deskmate/src/dm_config.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Compile-time view of the DeskMate configuration.  Everything the rest of
 * the application needs from Kconfig is funnelled through this header so the
 * sources stay free of configuration ifdef soup.
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_DESKMATE_DM_CONFIG_H
#define __APPS_EXAMPLES_DESKMATE_DM_CONFIG_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define DM_APP_NAME      "deskmate"
#define DM_APP_VERSION   "0.1.0"

/* NSH built-in entry point name */

#ifdef CONFIG_EXAMPLES_DESKMATE_PROGNAME
#  define DM_PROGNAME    CONFIG_EXAMPLES_DESKMATE_PROGNAME
#else
#  define DM_PROGNAME    DM_APP_NAME
#endif

/* Display backend --------------------------------------------------------- */

#ifdef CONFIG_EXAMPLES_DESKMATE_DISPLAY_FBDEV
#  define DM_DISPLAY_FBDEV   1
#else
#  define DM_DISPLAY_FBDEV   0
#endif

#define DM_DISPLAY_MEM   (!DM_DISPLAY_FBDEV)

/* In-memory backend geometry */

#ifdef CONFIG_EXAMPLES_DESKMATE_WIDTH
#  define DM_MEM_WIDTH   CONFIG_EXAMPLES_DESKMATE_WIDTH
#else
#  define DM_MEM_WIDTH   1024
#endif

#ifdef CONFIG_EXAMPLES_DESKMATE_HEIGHT
#  define DM_MEM_HEIGHT  CONFIG_EXAMPLES_DESKMATE_HEIGHT
#else
#  define DM_MEM_HEIGHT  600
#endif

/* Framebuffer device */

#if DM_DISPLAY_FBDEV
#  ifdef CONFIG_EXAMPLES_DESKMATE_FB_PATH
#    define DM_FB_PATH   CONFIG_EXAMPLES_DESKMATE_FB_PATH
#  else
#    define DM_FB_PATH   "/dev/fb0"
#  endif
#endif

/* Touchscreen device */

#ifdef CONFIG_EXAMPLES_DESKMATE_INPUT_DEVPATH
#  define DM_INPUT_PATH  CONFIG_EXAMPLES_DESKMATE_INPUT_DEVPATH
#else
#  define DM_INPUT_PATH  "/dev/input0"
#endif

/* Optional hostfs mount (simulator snapshot exchange) */

#ifdef CONFIG_EXAMPLES_DESKMATE_HOSTFS_PATH
#  define DM_HOSTFS_PATH CONFIG_EXAMPLES_DESKMATE_HOSTFS_PATH
#else
#  define DM_HOSTFS_PATH ""
#endif

/* Automatic shutdown for scripted runs */

#ifdef CONFIG_EXAMPLES_DESKMATE_EXIT_AFTER
#  define DM_EXIT_AFTER  CONFIG_EXAMPLES_DESKMATE_EXIT_AFTER
#else
#  define DM_EXIT_AFTER  0
#endif

/* Snapshot directory used when no explicit path is given */

#ifndef DM_SNAP_DIR
#  define DM_SNAP_DIR    "/data/deskmate"
#endif

#ifndef DM_SNAP_HOST_DIR
#  define DM_SNAP_HOST_DIR "/host/deskmate"
#endif

/* Geometry helpers -------------------------------------------------------- */

#define DM_STATUS_BAR_H  56
#define DM_FOOTER_H      40

/* Snapshots are limited so a full tmpfs cannot be exhausted by accident.  A
 * 1024x600 RGB565 frame is 1.2 MB, so allow two frames.
 */

#define DM_SNAP_MAX_BYTES (3 * 1024 * 1024)

#endif /* __APPS_EXAMPLES_DESKMATE_DM_CONFIG_H */
