/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_display.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/* /dev/fb0 glue for the ESP32-P4X-C5-Function-EV-Board's EK79007 MIPI-DSI
 * panel.  Mirrors the M5Stack Tab5 bring-up order:
 *
 *   1. Allocate the RGB565 framebuffer (PSRAM: 1024*600*2 = 1.2 MB, far too
 *      large for the internal SRAM heap)
 *   2. esp_mipi_dsi_configure_dpi()
 *   3. EK79007 DCS init (board_ek79007_initialize)
 *   4. esp_mipi_dsi_bind_framebuffer()  -> DW-GDMA streams FB to the bridge
 *   5. sleep-out, then esp_mipi_dsi_video_start()
 *   6. dcs_set_display_on()
 *   7. backlight on
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <debug.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/compiler.h>
#include <nuttx/kmalloc.h>
#include <nuttx/video/fb.h>
#include <nuttx/video/mipi_display.h>
#include <nuttx/video/mipi_dsi.h>

#include <arch/board/board.h>

#include "espressif/esp_mipi_dsi.h"

#include "esp32p4-function-ev-board.h"
#include "esp32p4_lcd_ek79007.h"

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_LCD

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BOARD_FB_BPP       16
#define BOARD_FB_WIDTH     BOARD_MIPI_DSI_H_RES
#define BOARD_FB_HEIGHT    BOARD_MIPI_DSI_V_RES
#define BOARD_FB_STRIDE    (BOARD_FB_WIDTH * (BOARD_FB_BPP / 8))
#define BOARD_FB_SIZE      (BOARD_FB_STRIDE * BOARD_FB_HEIGHT)

/* Solid red as the bring-up test colour: proves the DSI link, the panel
 * controller and the backlight all work without needing any app to run.
 */

#define BOARD_FB_TEST_COLOR  0xf800

/* All-Pixels-On / -Off DCS (used by the optional bring-up flash) */

#define BOARD_DCS_ALL_PIXELS_ON   0x23
#define BOARD_DCS_ALL_PIXELS_OFF  0x22
#define BOARD_APO_TEST_MS         1000

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int board_getvideoinfo(FAR struct fb_vtable_s *vtable,
                              FAR struct fb_videoinfo_s *vinfo);
static int board_getplaneinfo(FAR struct fb_vtable_s *vtable, int planeno,
                              FAR struct fb_planeinfo_s *pinfo);
#ifdef CONFIG_FB_UPDATE
static int board_updatearea(FAR struct fb_vtable_s *vtable,
                            FAR const struct fb_area_s *area);
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct fb_vtable_s g_board_vtable =
{
  .getvideoinfo = board_getvideoinfo,
  .getplaneinfo = board_getplaneinfo,
#ifdef CONFIG_FB_UPDATE
  .updatearea   = board_updatearea,
#endif
};

static struct fb_videoinfo_s g_board_video =
{
  .fmt     = FB_FMT_RGB16_565,
  .xres    = BOARD_FB_WIDTH,
  .yres    = BOARD_FB_HEIGHT,
  .nplanes = 1,
};

static struct fb_planeinfo_s g_board_plane =
{
  .fbmem        = NULL,
  .fblen        = BOARD_FB_SIZE,
  .stride       = BOARD_FB_STRIDE,
  .display      = 0,
  .bpp          = BOARD_FB_BPP,
  .xres_virtual = BOARD_FB_WIDTH,
  .yres_virtual = BOARD_FB_HEIGHT,
  .xoffset      = 0,
  .yoffset      = 0,
};

static FAR uint16_t *g_board_fb;
static bool g_board_fb_ready;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_getvideoinfo
 *
 * Description:
 *   Return the video information.
 *
 * Input Parameters:
 *   vtable - The vtable.
 *   vinfo  - The returned video information.
 *
 * Returned Value:
 *   Zero (OK) on success.
 *
 ****************************************************************************/

static int board_getvideoinfo(FAR struct fb_vtable_s *vtable,
                              FAR struct fb_videoinfo_s *vinfo)
{
  DEBUGASSERT(vtable != NULL && vtable == &g_board_vtable && vinfo != NULL);
  memcpy(vinfo, &g_board_video, sizeof(*vinfo));
  return OK;
}

/****************************************************************************
 * Name: board_getplaneinfo
 *
 * Description:
 *   Return the plane information.
 *
 * Input Parameters:
 *   vtable  - The vtable.
 *   planeno - The plane number.
 *   pinfo   - The returned plane information.
 *
 * Returned Value:
 *   Zero (OK) on success; -EINVAL if the plane number is invalid.
 *
 ****************************************************************************/

static int board_getplaneinfo(FAR struct fb_vtable_s *vtable, int planeno,
                              FAR struct fb_planeinfo_s *pinfo)
{
  DEBUGASSERT(vtable != NULL && vtable == &g_board_vtable && pinfo != NULL);

  if (planeno != 0 || g_board_plane.fbmem == NULL)
    {
      return -EINVAL;
    }

  memcpy(pinfo, &g_board_plane, sizeof(*pinfo));
  return OK;
}

#ifdef CONFIG_FB_UPDATE
/****************************************************************************
 * Name: board_updatearea
 *
 * Description:
 *   Write the dirty region back to memory so that DW-GDMA picks it up.
 *
 * Input Parameters:
 *   vtable - The vtable.
 *   area   - The area to update (NULL means the whole plane).
 *
 * Returned Value:
 *   Zero (OK) on success; -EAGAIN if the framebuffer is not ready.
 *
 ****************************************************************************/

static int board_updatearea(FAR struct fb_vtable_s *vtable,
                            FAR const struct fb_area_s *area)
{
  size_t offset;
  size_t len;

  DEBUGASSERT(vtable != NULL && vtable == &g_board_vtable);

  if (g_board_fb == NULL)
    {
      return -EAGAIN;
    }

  if (area == NULL)
    {
      return esp_mipi_dsi_flush_framebuffer(g_board_fb, BOARD_FB_SIZE);
    }

  if (area->y >= BOARD_FB_HEIGHT || area->x >= BOARD_FB_WIDTH)
    {
      return OK;
    }

  offset = (size_t)area->y * BOARD_FB_STRIDE +
           (size_t)area->x * (BOARD_FB_BPP / 8);
  len = (size_t)area->h * BOARD_FB_STRIDE;
  if (offset + len > BOARD_FB_SIZE)
    {
      len = BOARD_FB_SIZE - offset;
    }

  return esp_mipi_dsi_flush_framebuffer((FAR uint8_t *)g_board_fb + offset,
                                        len);
}
#endif /* CONFIG_FB_UPDATE */

/****************************************************************************
 * Name: board_fb_fill
 *
 * Description:
 *   Fill the whole plane with a colour and write it back to memory.
 *
 * Input Parameters:
 *   color - The RGB565 colour to fill with.
 *
 ****************************************************************************/

static void board_fb_fill(uint16_t color)
{
  size_t i;
  size_t npix = (size_t)BOARD_FB_WIDTH * BOARD_FB_HEIGHT;
  int ret;

  for (i = 0; i < npix; i++)
    {
      g_board_fb[i] = color;
    }

  ret = esp_mipi_dsi_flush_framebuffer(g_board_fb, BOARD_FB_SIZE);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: FB cache write-back failed: %d\n", ret);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_fbinitialize
 *
 * Description:
 *   Initialize the framebuffer and bring up the panel. Called by
 *   fb_register().
 *
 * Input Parameters:
 *   display - The display number.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int up_fbinitialize(int display)
{
  FAR struct mipi_dsi_host *host;
  FAR struct mipi_dsi_device *device;
  struct esp_mipi_dsi_dpi_config_s dpi;
  int ret;

  if (display != 0)
    {
      return -ENODEV;
    }

  if (g_board_fb_ready)
    {
      return OK;
    }

  /* 1024*600*2 = 1.2 MB: only the PSRAM heap region can satisfy this, so the
   * allocation lands in PSRAM as required for DW-GDMA streaming.
   */

  g_board_fb = kumm_memalign(64, BOARD_FB_SIZE);
  if (g_board_fb == NULL)
    {
      syslog(LOG_ERR,
             "ERROR: FB alloc failed (%u bytes; enable PSRAM)\n",
             (unsigned int)BOARD_FB_SIZE);
      return -ENOMEM;
    }

  memset(g_board_fb, 0, BOARD_FB_SIZE);
  g_board_plane.fbmem = g_board_fb;

  host = esp_mipi_dsi_host_get();
  if (host == NULL)
    {
      syslog(LOG_ERR,
             "ERROR: MIPI-DSI host not ready "
             "(enable ESP32P4_FUNCTION_EV_BOARD_MIPI_DSI)\n");
      ret = -EAGAIN;
      goto errout_fb;
    }

  /* ESP-IDF order: DPI config before the panel DCS init; display_on after
   * video mode is enabled.
   */

  syslog(LOG_INFO, "Configuring DPI %ux%u @ %u MHz...\n",
         BOARD_FB_WIDTH, BOARD_FB_HEIGHT,
         (unsigned int)BOARD_MIPI_DSI_DPI_CLK_MHZ);
  board_mipi_dsi_dpi_config(&dpi);
  ret = esp_mipi_dsi_configure_dpi(&dpi);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: configure_dpi failed: %d\n", ret);
      goto errout_fb;
    }

  syslog(LOG_INFO, "%s panel init...\n", BOARD_LCD_PANEL_NAME);
  device = board_ek79007_initialize(host);
  if (device == NULL)
    {
      ret = -EIO;
      goto errout_fb;
    }

  ret = esp_mipi_dsi_bind_framebuffer(g_board_fb, BOARD_FB_SIZE,
                                      BOARD_FB_WIDTH, BOARD_FB_HEIGHT,
                                      BOARD_FB_BPP);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: bind_framebuffer failed: %d\n", ret);
      goto errout_fb;
    }

  board_fb_fill(BOARD_FB_TEST_COLOR);

  ret = mipi_dsi_dcs_exit_sleep_mode(device);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: pre-video sleep_out failed: %d\n", ret);
      goto errout_fb;
    }

  up_mdelay(120);

  syslog(LOG_INFO, "Starting DSI video...\n");
  ret = esp_mipi_dsi_video_start();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: video_start failed: %d\n", ret);
      goto errout_fb;
    }

  ret = mipi_dsi_dcs_set_display_on(device);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: post-video display_on failed: %d\n", ret);
      goto errout_fb;
    }

  board_fb_fill(BOARD_FB_TEST_COLOR);

  /* Only now is it safe to light the panel up. */

  ret = board_lcd_backlight(true);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: backlight failed: %d\n", ret);
    }

  g_board_fb_ready = true;
  syslog(LOG_INFO, "/dev/fb0 ready %ux%u RGB565 @ %p (%u bytes)\n",
         BOARD_FB_WIDTH, BOARD_FB_HEIGHT, g_board_fb,
         (unsigned int)BOARD_FB_SIZE);

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_LCD_APO_TEST
  /* All-Pixels-On flash: proves DCS + backlight even if the DSI video
   * stream itself is misconfigured.  Restores the red test fill after.
   */

  mipi_dsi_dcs_write(device, BOARD_DCS_ALL_PIXELS_ON, NULL, 0);
  up_mdelay(BOARD_APO_TEST_MS);
  mipi_dsi_dcs_write(device, BOARD_DCS_ALL_PIXELS_OFF, NULL, 0);
  mipi_dsi_dcs_write(device, MIPI_DCS_ENTER_NORMAL_MODE, NULL, 0);
  mipi_dsi_dcs_set_display_on(device);
  board_fb_fill(BOARD_FB_TEST_COLOR);
#endif

  return OK;

errout_fb:
  kumm_free(g_board_fb);
  g_board_fb = NULL;
  g_board_plane.fbmem = NULL;
  return ret;
}

/****************************************************************************
 * Name: up_fbgetvplane
 *
 * Description:
 *   Return the framebuffer vtable for the requested plane.
 *
 * Input Parameters:
 *   display - The display number.
 *   vplane  - The vplane number.
 *
 * Returned Value:
 *   Pointer to the vtable on success; NULL on failure.
 *
 ****************************************************************************/

FAR struct fb_vtable_s *up_fbgetvplane(int display, int vplane)
{
  if (display != 0 || vplane != 0 || !g_board_fb_ready)
    {
      return NULL;
    }

  return &g_board_vtable;
}

/****************************************************************************
 * Name: up_fbuninitialize
 *
 * Description:
 *   Uninitialize the framebuffer.
 *
 * Input Parameters:
 *   display - The display number.
 *
 ****************************************************************************/

void up_fbuninitialize(int display)
{
  UNUSED(display);
}

/****************************************************************************
 * Name: board_lcd_reload_test_pattern
 *
 * Description:
 *   Call after fb_register(): the generic FB driver memsets the plane and
 *   would otherwise leave a black DMA buffer.
 *
 * Returned Value:
 *   Zero (OK) on success; -EAGAIN if the plane is not ready.
 *
 ****************************************************************************/

int board_lcd_reload_test_pattern(void)
{
  if (!g_board_fb_ready || g_board_fb == NULL)
    {
      return -EAGAIN;
    }

  board_fb_fill(BOARD_FB_TEST_COLOR);
  return OK;
}

#endif /* CONFIG_ESP32P4_FUNCTION_EV_BOARD_LCD */
