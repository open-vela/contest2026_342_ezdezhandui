/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_hmi_power.c
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

/* Display power / reset / backlight path for the ESP32-P4X-C5-Function-EV-
 * Board with the AML070JGI50-07403L (EK79007 + EK73217) MIPI-DSI module.
 *
 * Power-on order (mirrors ESP-IDF: LDO -> esp_lcd_new_dsi_bus ->
 * esp_lcd_panel_reset -> esp_lcd_panel_init):
 *
 *   1. board_mipi_phy_power(true)  LDO ch3 @ 2500 mV = VDD_MIPI_DPHY.
 *                                  MUST be on before esp_mipi_dsi_initialize()
 *                                  because that call starts the DSI PHY PLL.
 *   2. board_lcd_reset()           GPIO27 low 10 ms, high 20 ms (active low).
 *   3. backlight held OFF until the panel has been initialized and video is
 *      streaming, so the user never sees an uninitialized raster.
 *
 * GPIO26 (backlight) and GPIO27 (reset) are NOT part of the LCD FPC pinout;
 * they only reach the panel adapter when the J1 -> J6 jumpers are fitted.
 * Both therefore have their own Kconfig switch and default to "not driven"
 * behaviour only if the user turns them off; when they are enabled but the
 * jumper is missing the panel simply stays dark / keeps its power-on state
 * and no error is reported (a floating input is not detectable in software).
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <syslog.h>

#include <nuttx/arch.h>

#include "espressif/esp_ldo.h"
#include "espressif/esp_gpio.h"

#include <arch/board/board.h>

#include "esp32p4-function-ev-board.h"

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_LCD_POWER

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* MIPI DSI/CSI PHY supply: on-chip LDO channel 3 (ESP_LDO_VO3) @ 2500 mV */

#define BOARD_MIPI_PHY_LDO_CHAN       3
#define BOARD_MIPI_PHY_LDO_VOLTAGE_MV 2500

/* EK79007 reset timing (see panel_ek79007_reset() in esp_lcd_ek79007.c) */

#define BOARD_LCD_RST_ASSERT_MS       10
#define BOARD_LCD_RST_RELEASE_MS      20

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct esp_ldo_config_t g_mipi_phy_ldo_config =
{
  .chan_id    = BOARD_MIPI_PHY_LDO_CHAN,
  .voltage_mv = BOARD_MIPI_PHY_LDO_VOLTAGE_MV,
  .handler    = NULL,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_mipi_phy_power
 *
 * Description:
 *   Acquire or release the MIPI PHY LDO (channel 3 @ 2500 mV).
 *
 * Input Parameters:
 *   on - True to enable the LDO, false to release it.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int board_mipi_phy_power(bool on)
{
  int ret;

  if (on)
    {
      ret = esp_ldo_channel_acquire(&g_mipi_phy_ldo_config);
      if (ret < 0)
        {
          syslog(LOG_ERR,
                 "ERROR: failed to acquire MIPI PHY LDO ch%d @ %dmV: %d\n",
                 BOARD_MIPI_PHY_LDO_CHAN, BOARD_MIPI_PHY_LDO_VOLTAGE_MV,
                 ret);
          return ret;
        }

      syslog(LOG_INFO, "MIPI PHY LDO ch%d @ %dmV enabled\n",
             BOARD_MIPI_PHY_LDO_CHAN, BOARD_MIPI_PHY_LDO_VOLTAGE_MV);
    }
  else
    {
      ret = esp_ldo_channel_release(&g_mipi_phy_ldo_config);
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: failed to release MIPI PHY LDO: %d\n",
                 ret);
          return ret;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: board_lcd_reset
 *
 * Description:
 *   Pulse GPIO27 (LCD_RST, active low, needs the J1 -> J6 jumper).
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int board_lcd_reset(void)
{
#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_LCD_RST
  int ret;

  ret = esp_configgpio(BOARD_LCD_GPIO_RST, OUTPUT);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: failed to configure LCD_RST GPIO%d: %d\n",
             BOARD_LCD_GPIO_RST, ret);
      return ret;
    }

  /* Assert reset (active low), hold, then release. */

  esp_gpiowrite(BOARD_LCD_GPIO_RST, false);
  up_mdelay(BOARD_LCD_RST_ASSERT_MS);
  esp_gpiowrite(BOARD_LCD_GPIO_RST, true);
  up_mdelay(BOARD_LCD_RST_RELEASE_MS);

  syslog(LOG_INFO, "LCD reset pulsed on GPIO%d (J1->J6 jumper required)\n",
         BOARD_LCD_GPIO_RST);
#else
  syslog(LOG_INFO,
         "LCD_RST GPIO%d not driven (CONFIG_ESP32P4_FUNCTION_EV_BOARD_LCD_RST"
         " disabled; jumper may be absent)\n", BOARD_LCD_GPIO_RST);
#endif

  return OK;
}

/****************************************************************************
 * Name: board_lcd_backlight
 *
 * Description:
 *   Drive GPIO26 (LCD backlight enable, needs the J1 -> J6 jumper).
 *
 * Input Parameters:
 *   on - True to enable the backlight, false to disable it.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int board_lcd_backlight(bool on)
{
#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_LCD_BL
  int ret;

  ret = esp_configgpio(BOARD_LCD_GPIO_BL, OUTPUT);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: failed to configure backlight GPIO%d: %d\n",
             BOARD_LCD_GPIO_BL, ret);
      return ret;
    }

  esp_gpiowrite(BOARD_LCD_GPIO_BL, on);

  syslog(LOG_INFO, "Backlight %s on GPIO%d (J1->J6 jumper required)\n",
         on ? "enabled" : "disabled", BOARD_LCD_GPIO_BL);
#else
  UNUSED(on);
  syslog(LOG_INFO,
         "Backlight GPIO%d not driven "
         "(CONFIG_ESP32P4_FUNCTION_EV_BOARD_LCD_BL disabled)\n",
         BOARD_LCD_GPIO_BL);
#endif

  return OK;
}

/****************************************************************************
 * Name: board_lcd_power_init
 *
 * Description:
 *   Display power-on sequence: MIPI PHY LDO -> panel reset pulse ->
 *   backlight off.  Called from esp_bringup() before the DSI host init.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int board_lcd_power_init(void)
{
  int ret;

  ret = board_mipi_phy_power(true);
  if (ret < 0)
    {
      return ret;
    }

  /* Reset the panel and keep the backlight dark until video is running. */

  ret = board_lcd_reset();
  if (ret < 0)
    {
      return ret;
    }

  ret = board_lcd_backlight(false);
  if (ret < 0)
    {
      return ret;
    }

  syslog(LOG_INFO, "LCD power/reset path ready (%s)\n",
         BOARD_LCD_PANEL_NAME);
  return OK;
}

#endif /* CONFIG_ESP32P4_FUNCTION_EV_BOARD_LCD_POWER */
