/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_lcd_ek79007.c
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

/* EK79007 + EK73217 MIPI-DSI panel bring-up for the ESP32-P4X-C5-Function-EV-
 * Board's 7.0" 1024x600 module (AML070JGI50-07403L).
 *
 * Init table taken verbatim from Espressif's esp_lcd_ek79007 component
 * (components/display/lcd/esp_lcd_ek79007/esp_lcd_ek79007.c), function
 * panel_ek79007_send_init_cmds():
 *
 *   0xb2 <- 0x10   DSI pad control: 2 data lanes (EK79007_DSI_2_LANE)
 *   0x80 <- 0x8b   vendor specific
 *   0x81 <- 0x78
 *   0x82 <- 0x84
 *   0x83 <- 0x88
 *   0x84 <- 0xa8
 *   0x85 <- 0xe3
 *   0x86 <- 0x88
 *   0x11 <- --      exit sleep mode, 120 ms
 *
 * The reference driver sends 0x29 (display on) separately, from
 * esp_lcd_panel_disp_on_off(), i.e. after video mode has been enabled.
 * This board does the same: the framebuffer glue calls
 * mipi_dsi_dcs_set_display_on() after esp_mipi_dsi_video_start().
 *
 * No COLMOD (0x3a) is sent: the ESP-IDF DPI panel derives the pixel format
 * from the DSI host's in/out colour format, and the NuttX DSI host is
 * configured to RGB565 by esp_mipi_dsi_configure_dpi().
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/video/mipi_dsi.h>

#include <arch/board/board.h>

#include "esp32p4-function-ev-board.h"
#include "esp32p4_lcd_ek79007.h"

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_LCD

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define EK79007_NAME            "ek79007"

#define EK79007_PAD_CONTROL     0xb2
#define EK79007_DSI_2_LANE      0x10

/* Exit sleep mode and the settle time the reference driver uses. */

#define EK79007_DCS_SLEEP_OUT   0x11
#define EK79007_SLEEP_OUT_MS    120

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ek79007_init_cmd_s
{
  uint8_t cmd;
  FAR const uint8_t *data;
  uint8_t len;
  uint16_t delay_ms;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* EK79007 vendor init commands, in the order the reference driver sends
 * them.  The lane-count pad control (0xb2) is sent first, then the seven
 * vendor specific registers, then sleep-out with the reference delay.
 */

static const uint8_t g_ek79007_pad_control[] =
{
  EK79007_DSI_2_LANE
};

static const uint8_t g_ek79007_cmd_80[] =
{
  0x8b
};

static const uint8_t g_ek79007_cmd_81[] =
{
  0x78
};

static const uint8_t g_ek79007_cmd_82[] =
{
  0x84
};

static const uint8_t g_ek79007_cmd_83[] =
{
  0x88
};

static const uint8_t g_ek79007_cmd_84[] =
{
  0xa8
};

static const uint8_t g_ek79007_cmd_85[] =
{
  0xe3
};

static const uint8_t g_ek79007_cmd_86[] =
{
  0x88
};

static const struct ek79007_init_cmd_s g_ek79007_init[] =
{
  {
    EK79007_PAD_CONTROL, g_ek79007_pad_control,
    sizeof(g_ek79007_pad_control), 0
  },
  {
    0x80, g_ek79007_cmd_80, sizeof(g_ek79007_cmd_80), 0
  },
  {
    0x81, g_ek79007_cmd_81, sizeof(g_ek79007_cmd_81), 0
  },
  {
    0x82, g_ek79007_cmd_82, sizeof(g_ek79007_cmd_82), 0
  },
  {
    0x83, g_ek79007_cmd_83, sizeof(g_ek79007_cmd_83), 0
  },
  {
    0x84, g_ek79007_cmd_84, sizeof(g_ek79007_cmd_84), 0
  },
  {
    0x85, g_ek79007_cmd_85, sizeof(g_ek79007_cmd_85), 0
  },
  {
    0x86, g_ek79007_cmd_86, sizeof(g_ek79007_cmd_86), 0
  },
  {
    EK79007_DCS_SLEEP_OUT, NULL, 0, EK79007_SLEEP_OUT_MS
  },
};

#define EK79007_INIT_COUNT \
  (sizeof(g_ek79007_init) / sizeof(g_ek79007_init[0]))

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: ek79007_send_init
 *
 * Description:
 *   Send the EK79007 vendor initialization table over LP DCS.
 *
 * Input Parameters:
 *   device - The registered (and attached) MIPI-DSI device.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

static int ek79007_send_init(FAR struct mipi_dsi_device *device)
{
  unsigned int i;
  ssize_t n;

  for (i = 0; i < EK79007_INIT_COUNT; i++)
    {
      FAR const struct ek79007_init_cmd_s *cmd = &g_ek79007_init[i];

      n = mipi_dsi_dcs_write(device, cmd->cmd, cmd->data, cmd->len);
      if (n < 0)
        {
          syslog(LOG_ERR, "ERROR: EK79007 DCS 0x%02x failed: %zd (%u/%u)\n",
                 cmd->cmd, n, i + 1, (unsigned int)EK79007_INIT_COUNT);
          return (int)n;
        }

      if (cmd->delay_ms > 0)
        {
          up_mdelay(cmd->delay_ms);
        }
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_ek79007_initialize
 *
 * Description:
 *   Register the EK79007 panel as a mipi_dsi_device, attach it to the
 *   Espressif host and send the vendor DCS initialization table.
 *
 * Input Parameters:
 *   host - Registered MIPI-DSI host.
 *
 * Returned Value:
 *   Pointer to the registered device on success; NULL on failure.
 *
 ****************************************************************************/

FAR struct mipi_dsi_device *board_ek79007_initialize(
      FAR struct mipi_dsi_host *host)
{
  FAR struct mipi_dsi_device *device;
  int ret;

  if (host == NULL)
    {
      syslog(LOG_ERR, "ERROR: EK79007: DSI host is not registered\n");
      return NULL;
    }

  /* The reset pulse itself is issued by board_lcd_power_init(); repeat it
   * here so that the panel is in a known state even if the power path ran
   * long before the DSI host came up.
   */

  ret = board_lcd_reset();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: EK79007: reset pulse failed: %d\n", ret);
      return NULL;
    }

  device = mipi_dsi_device_register(host, EK79007_NAME, 0);
  if (device == NULL)
    {
      syslog(LOG_ERR, "ERROR: mipi_dsi_device_register failed\n");
      return NULL;
    }

  device->lanes      = BOARD_MIPI_DSI_LANES;
  device->format     = MIPI_DSI_FMT_RGB565;
  device->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
                       MIPI_DSI_MODE_LPM;
  device->hs_rate    = BOARD_MIPI_DSI_LANE_BITRATE_MBPS * 1000000UL;
  device->lp_rate    = 0;

  ret = mipi_dsi_attach(device);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: EK79007: attach failed: %d\n", ret);
      return NULL;
    }

  ret = ek79007_send_init(device);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: EK79007: DCS init failed: %d\n", ret);
      return NULL;
    }

  syslog(LOG_INFO, "EK79007: panel initialized (%d lanes, %d Mbps/lane)\n",
         BOARD_MIPI_DSI_LANES, BOARD_MIPI_DSI_LANE_BITRATE_MBPS);
  return device;
}

#endif /* CONFIG_ESP32P4_FUNCTION_EV_BOARD_LCD */
