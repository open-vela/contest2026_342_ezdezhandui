/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_lcd_ek79007.h
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

#ifndef __BOARDS_RISCV_ESP32P4_ESP32P4_FUNCTION_EV_BOARD_SRC_ESP32P4_LCD_EK79007_H
#define __BOARDS_RISCV_ESP32P4_ESP32P4_FUNCTION_EV_BOARD_SRC_ESP32P4_LCD_EK79007_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/video/mipi_dsi.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifndef __ASSEMBLY__

#ifdef __cplusplus
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Name: board_ek79007_initialize
 *
 * Description:
 *   Register the EK79007 panel as a mipi_dsi_device, attach it to the
 *   Espressif DSI host, and send the vendor DCS initialization table.
 *   Does not start video; the board calls dcs_display_on() after
 *   esp_mipi_dsi_video_start().
 *
 * Input Parameters:
 *   host - Registered MIPI-DSI host (esp_mipi_dsi_host_get()).
 *
 * Returned Value:
 *   Pointer to the registered device on success; NULL on failure.
 *
 ****************************************************************************/

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_LCD
FAR struct mipi_dsi_device *board_ek79007_initialize(
      FAR struct mipi_dsi_host *host);
#endif

#undef EXTERN
#ifdef __cplusplus
}
#endif

#endif /* __ASSEMBLY__ */
#endif /* __BOARDS_RISCV_ESP32P4_ESP32P4_FUNCTION_EV_BOARD_SRC_ESP32P4_LCD_EK79007_H */
