/****************************************************************************
 * apps/examples/deskmate/src/hal/dm_camera.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Camera half of the DeskMate HAL.
 *
 * Two backends behind one call:
 *
 *   v4l2      : /dev/video0 through the V4L2 capture interface, which is what
 *               the ESP32-P4 board exposes (SC2336 -> MIPI-CSI -> V4L2).  The
 *               sensor delivers RAW8 (SBGGR8); the preview does a 2x2 quad
 *               demosaic so a colour thumbnail comes out at half resolution
 *               without an ISP or a heavy scaler.  If the driver can hand out
 *               RGB565 directly, that path is used unchanged.
 *
 *   synthetic : a moving pattern generated on the fly.  This is what the
 *               simulator uses (there is no /dev/video0 on a headless CI
 *               host) and what the UI falls back to when the sensor is
 *               missing, so the camera page and the event engine can always be
 *               exercised end to end.
 *
 * Frames are RGB565, already sized for the preview canvas.
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_DESKMATE_HAL_DM_CAMERA_H
#define __APPS_EXAMPLES_DESKMATE_HAL_DM_CAMERA_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct dm_camera_frame_s
{
  const uint16_t *pixels;   /* RGB565 preview, width*height */
  int  width;
  int  height;
  int  seq;                 /* Frame counter since start */
  bool synthetic;           /* true: generated pattern, not a real sensor */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Probe the camera and set up the capture pipeline.  preview_w/h is the size
 * the UI wants.  Returns 0 when *some* backend is ready (possibly synthetic).
 */

int dm_camera_create(int preview_w, int preview_h);

/* Backend actually in use: "v4l2" or "synthetic". */

const char *dm_camera_backend(void);

bool dm_camera_ready(void);

/* Fetch the newest frame.  Returns 0 when a frame was produced, -EAGAIN when
 * nothing new is available this iteration.
 */

int dm_camera_poll(struct dm_camera_frame_s *out);

/* Push the last frame through the same path without new capture (used after a
 * page rebuild, so the canvas always has something to show).
 */

const struct dm_camera_frame_s *dm_camera_last(void);

void dm_camera_destroy(void);

#endif /* __APPS_EXAMPLES_DESKMATE_HAL_DM_CAMERA_H */
