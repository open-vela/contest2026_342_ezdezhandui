/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4-function-ev-board.h
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

#ifndef __BOARDS_RISCV_ESP32P4_ESP32P4_FUNCTION_EV_BOARD_SRC_ESP32P4_FUNCTION_EV_BOARD_H
#define __BOARDS_RISCV_ESP32P4_ESP32P4_FUNCTION_EV_BOARD_SRC_ESP32P4_FUNCTION_EV_BOARD_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/compiler.h>

#include <stdbool.h>

#ifdef CONFIG_ESPRESSIF_MIPI_DSI
#  include "espressif/esp_mipi_dsi.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* RMT gpio */

#define RMT_RXCHANNEL       4
#define RMT_TXCHANNEL       0

#ifdef CONFIG_RMT_LOOP_TEST_MODE
#  define RMT_INPUT_PIN     0
#  define RMT_OUTPUT_PIN    0
#else
#  define RMT_INPUT_PIN     2
#  define RMT_OUTPUT_PIN    8
#endif

/****************************************************************************
 * Public Types
 ****************************************************************************/

/****************************************************************************
 * Public Data
 ****************************************************************************/

#ifndef __ASSEMBLY__

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: esp_bringup
 *
 * Description:
 *   Perform architecture-specific initialization.
 *
 * Input Parameters:
 *   None.
 *
 * Returned Value:
 *   Zero (OK) is returned on success; A negated errno value is returned on
 *   any failure.
 *
 ****************************************************************************/

int esp_bringup(void);

/****************************************************************************
 * Name: board_twai_setup
 *
 * Description:
 *  Initialize TWAI and register the TWAI device
 *
 * Input Parameters:
 *   port - Port number (for hardware that has multiple TWAI interfaces)
 *
 * Returned Value:
 *   Zero (OK) is returned on success; A negated errno value is returned on
 *   any failure.
 *
 ****************************************************************************/

#ifdef CONFIG_ESPRESSIF_TWAI
int board_twai_setup(int port);
#endif

/****************************************************************************
 * Name: esp_gpio_init
 *
 * Description:
 *   Configure the GPIO driver.
 *
 * Input Parameters:
 *   None.
 *
 * Returned Value:
 *   Zero (OK).
 *
 ****************************************************************************/

#ifdef CONFIG_DEV_GPIO
int esp_gpio_init(void);
#endif

/****************************************************************************
 * Name: board_emac_init
 *
 * Description:
 *   Bring up the ESP32-P4 Ethernet MAC driver (esp_eth backed).
 *
 * Input Parameters:
 *   None.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

#ifdef CONFIG_ESPRESSIF_EMAC
int board_emac_init(void);
#endif

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_LCD_POWER

/****************************************************************************
 * Name: board_mipi_phy_power
 *
 * Description:
 *   Acquire or release on-chip LDO channel 3 (ESP_LDO_VO3) at 2500 mV, the
 *   supply of the MIPI-DSI/CSI PHY (VDD_MIPI_DPHY).
 *
 * Input Parameters:
 *   on - True to enable the LDO, false to release it.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int board_mipi_phy_power(bool on);

/****************************************************************************
 * Name: board_lcd_reset
 *
 * Description:
 *   Pulse the panel reset line (GPIO27, active low).  No-op unless
 *   CONFIG_ESP32P4_FUNCTION_EV_BOARD_LCD_RST is enabled, because the line is
 *   only reachable when the J1 -> J6 jumper is fitted.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int board_lcd_reset(void);

/****************************************************************************
 * Name: board_lcd_backlight
 *
 * Description:
 *   Enable or disable the panel backlight (GPIO26).  No-op unless
 *   CONFIG_ESP32P4_FUNCTION_EV_BOARD_LCD_BL is enabled, because the line is
 *   only reachable when the J1 -> J6 jumper is fitted.
 *
 * Input Parameters:
 *   on - True to enable the backlight, false to disable it.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int board_lcd_backlight(bool on);

/****************************************************************************
 * Name: board_lcd_power_init
 *
 * Description:
 *   Bring up the display power rails in hardware order:
 *   LDO ch3 2500 mV (DSI PHY) -> panel reset pulse -> backlight off.
 *   Must run before esp_mipi_dsi_initialize().
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int board_lcd_power_init(void);

#endif /* CONFIG_ESP32P4_FUNCTION_EV_BOARD_LCD_POWER */

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_LCD

/****************************************************************************
 * Name: board_lcd_reload_test_pattern
 *
 * Description:
 *   Re-fill the plane with the bring-up test colour and write the cache
 *   back.  Call after fb_register(): the generic FB driver memsets the
 *   plane and would otherwise leave a black DMA buffer.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int board_lcd_reload_test_pattern(void);

/****************************************************************************
 * Name: board_mipi_dsi_dpi_config
 *
 * Description:
 *   Fill the DPI timing for the EK79007/EK73217 panel module.  The arch
 *   host has no panel defaults, so the board must supply this to
 *   esp_mipi_dsi_configure_dpi().
 *
 * Input Parameters:
 *   cfg - The DPI configuration structure to fill.
 *
 ****************************************************************************/

void board_mipi_dsi_dpi_config(FAR struct esp_mipi_dsi_dpi_config_s *cfg);

#endif /* CONFIG_ESP32P4_FUNCTION_EV_BOARD_LCD */

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_TOUCHSCREEN

/****************************************************************************
 * Name: board_touchscreen_init
 *
 * Description:
 *   Probe the GT911 touch controller on I2C0 and register it as
 *   /dev/input0.  The panel has no interrupt or reset line routed, so the
 *   controller is polled by its reader.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int board_touchscreen_init(void);

#endif /* CONFIG_ESP32P4_FUNCTION_EV_BOARD_TOUCHSCREEN */


/****************************************************************************
 * Name: esp32p4_camera_initialize
 *
 * Description:
 *   Register the MIPI-CSI camera (SC2336 on the AS-AG638A32M2-50 module) as
 *   a V4L2 capture device.  Returns 0 on success, a negated errno otherwise.
 *
 ****************************************************************************/

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_CAMERA
int esp32p4_camera_initialize(void);
int esp32p4_camera_uninitialize(void);
#endif

#endif /* __ASSEMBLY__ */
#endif /* __BOARDS_RISCV_ESP32P4_ESP32P4_FUNCTION_EV_BOARD_SRC_ESP32P4_FUNCTION_EV_BOARD_H */
