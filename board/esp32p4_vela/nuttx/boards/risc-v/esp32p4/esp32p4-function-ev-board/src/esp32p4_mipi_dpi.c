/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_mipi_dpi.c
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

/* MIPI-DSI DPI timing for the AML070JGI50-07403L panel module
 * (EK79007 source driver + EK73217 gate driver), 1024x600 RGB565.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <assert.h>

#include <nuttx/video/mipi_dsi.h>

#include <arch/board/board.h>

#include "esp32p4-function-ev-board.h"

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_LCD

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_mipi_dsi_dpi_config
 *
 * Description:
 *   Fill the DPI configuration consumed by esp_mipi_dsi_configure_dpi().
 *   Values follow Espressif's
 *   EK79007_1024_600_PANEL_60HZ_CONFIG(LCD_COLOR_FMT_RGB565).
 *
 * Input Parameters:
 *   cfg - The configuration structure to fill.
 *
 ****************************************************************************/

void board_mipi_dsi_dpi_config(FAR struct esp_mipi_dsi_dpi_config_s *cfg)
{
  DEBUGASSERT(cfg != NULL);

  cfg->h_res              = BOARD_MIPI_DSI_H_RES;
  cfg->v_res              = BOARD_MIPI_DSI_V_RES;
  cfg->hsync_pulse_width  = BOARD_MIPI_DSI_HSYNC_PULSE_WIDTH;
  cfg->hsync_back_porch   = BOARD_MIPI_DSI_HSYNC_BACK_PORCH;
  cfg->hsync_front_porch  = BOARD_MIPI_DSI_HSYNC_FRONT_PORCH;
  cfg->vsync_pulse_width  = BOARD_MIPI_DSI_VSYNC_PULSE_WIDTH;
  cfg->vsync_back_porch   = BOARD_MIPI_DSI_VSYNC_BACK_PORCH;
  cfg->vsync_front_porch  = BOARD_MIPI_DSI_VSYNC_FRONT_PORCH;
  cfg->dpi_clock_freq_mhz = BOARD_MIPI_DSI_DPI_CLK_MHZ;
  cfg->virtual_channel    = 0;
  cfg->format             = MIPI_DSI_FMT_RGB565;
}

#endif /* CONFIG_ESP32P4_FUNCTION_EV_BOARD_LCD */
