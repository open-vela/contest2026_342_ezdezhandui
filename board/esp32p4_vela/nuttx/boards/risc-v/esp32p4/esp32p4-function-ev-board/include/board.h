/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/include/board.h
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

#ifndef __BOARDS_RISCV_ESP32P4_ESP32P4_FUNCTION_EV_BOARD_INCLUDE_BOARD_H
#define __BOARDS_RISCV_ESP32P4_ESP32P4_FUNCTION_EV_BOARD_INCLUDE_BOARD_H

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* GPIO pins used by the GPIO Subsystem */

#define BOARD_NGPIOOUT    2 /* Amount of GPIO Output pins */
#define BOARD_NGPIOINT    1 /* Amount of GPIO Input w/ Interruption pins */

/* ESP32P4-Generic GPIOs ****************************************************/

/* BOOT Button */

#define BUTTON_BOOT  35

/* MIPI-DSI display *********************************************************/

/* The ESP32-P4X-C5-Function-EV-Board ships with a 7.0" 1024x600 MIPI-DSI
 * panel module "AML070JGI50-07403L" driven by an EK79007 source driver plus
 * EK73217 gate driver.  The module plugs into the LCD FPC connector; two
 * signals are NOT on that FPC and must be jumpered from header J1 (or the
 * P4X-C5 main board header) to the panel adapter's J6 header:
 *
 *   GPIO26 -> panel backlight enable    (LCD_BL)
 *   GPIO27 -> panel reset, active low   (LCD_RST)
 *
 * Both are optional at build time because a module which brings its own
 * pull-ups / a permanently-on backlight still produces a picture without
 * them (see CONFIG_ESP32P4_FUNCTION_EV_BOARD_LCD_BL / _LCD_RST).
 *
 * The DSI clock/data lanes are dedicated MIPI pins (not GPIO-muxed) and are
 * powered by VDD_MIPI_DPHY, which is supplied by on-chip LDO channel 3
 * (ESP_LDO_VO3) at 2500 mV and MUST be enabled before the DSI host is
 * initialized.
 */

#define BOARD_LCD_GPIO_BL       26  /* Backlight enable (jumper needed) */
#define BOARD_LCD_GPIO_RST      27  /* Panel reset, active low (jumper needed) */

/* MIPI-DSI DPI timing for the EK79007 + EK73217 panel module.  Values match
 * the Espressif EK79007_1024_600_PANEL_60HZ_CONFIG() reference:
 *   refresh = 52 MHz / (1024+10+160+160) / (600+1+23+12) ~= 60.4 Hz
 */

#define BOARD_MIPI_DSI_H_RES              1024
#define BOARD_MIPI_DSI_V_RES              600
#define BOARD_MIPI_DSI_DPI_CLK_MHZ        52
#define BOARD_MIPI_DSI_HSYNC_PULSE_WIDTH  10
#define BOARD_MIPI_DSI_HSYNC_BACK_PORCH   160
#define BOARD_MIPI_DSI_HSYNC_FRONT_PORCH  160
#define BOARD_MIPI_DSI_VSYNC_PULSE_WIDTH  1
#define BOARD_MIPI_DSI_VSYNC_BACK_PORCH   23
#define BOARD_MIPI_DSI_VSYNC_FRONT_PORCH  12
#define BOARD_MIPI_DSI_LANES              2
#define BOARD_MIPI_DSI_LANE_BITRATE_MBPS  900  /* EK79007_PANEL_BUS_DSI_2CH */

#define BOARD_LCD_PANEL_NAME              "EK79007"

/* I2C shared system bus ****************************************************/

/* I2C0 is the board's shared control bus: GT911 touch controller (0x5d, with
 * 0x14 as the alternate strapping), ES8311 audio codec and the SC2336 camera
 * control port all hang off it.
 */

#define BOARD_I2C0_SDA_GPIO     7
#define BOARD_I2C0_SCL_GPIO     8

/* GT911 capacitive touch controller ****************************************/

#define BOARD_GT911_I2C_ADDR        0x5d /* Primary address (INT pin high) */
#define BOARD_GT911_I2C_ADDR_ALT    0x14 /* Alternate address (INT pin low) */
#define BOARD_GT911_I2C_FREQUENCY   400000

#endif /* __BOARDS_RISCV_ESP32P4_ESP32P4_FUNCTION_EV_BOARD_INCLUDE_BOARD_H */
