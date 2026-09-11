/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_camera.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <syslog.h>

#include <nuttx/i2c/i2c_master.h>
#include <nuttx/video/imgdata.h>
#include <nuttx/video/imgsensor.h>
#include <nuttx/video/sc2336.h>
#include <nuttx/video/v4l2_cap.h>

#include <arch/board/board.h>

#include "espressif/esp_i2c.h"
#include "espressif/esp_csi.h"

#include "esp32p4-function-ev-board.h"

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_CAMERA

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* MIPI-CSI capture device node. */

#ifndef CONFIG_ESP32P4_FUNCTION_EV_BOARD_CAMERA_DEVPATH
#  define CONFIG_ESP32P4_FUNCTION_EV_BOARD_CAMERA_DEVPATH "/dev/video0"
#endif

/* SCCB address of the AS-AG638A32M2-50 camera module (SC2336 sensor).  The
 * SCCB interface shares I2C0 with the GT911 touch controller, so the bus has
 * to be initialized by whichever of the two comes up first.
 */

#ifndef CONFIG_SC2336_I2CADDR
#  define ESP32P4_CAMERA_I2C_ADDR  SC2336_I2C_ADDR
#else
#  define ESP32P4_CAMERA_I2C_ADDR  CONFIG_SC2336_I2CADDR
#endif

#define ESP32P4_CAMERA_I2C_PORT   ESPRESSIF_I2C0

/****************************************************************************
 * Private Data
 ****************************************************************************/

static FAR struct imgsensor_s *g_camera_sensors[1];
static FAR struct imgdata_s   *g_camera_imgdata;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp32p4_camera_initialize
 *
 * Description:
 *   Bring up the MIPI-CSI camera path of the ESP32-P4X-C5-Function-EV-Board
 *   and register it as a V4L2 capture device.
 *
 *   The board carries an AS-AG638A32M2-50 module (SmartSens SC2336, 1/3"
 *   1920x1080 CMOS) on a 2-lane MIPI CSI-2 link.  The module has its own
 *   24 MHz oscillator and its reset line is hard pulled to 3.3 V, so there
 *   is no clock or reset GPIO to drive; only the MIPI PHY supply (on-chip
 *   LDO channel 3 at 2500 mV, shared with the DSI PHY) has to be up, which
 *   esp_csi_imgdata_initialize() takes care of.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure.
 *
 ****************************************************************************/

int esp32p4_camera_initialize(void)
{
  FAR struct i2c_master_s *i2c;
  struct esp_csi_config_s csi_config;
  int chipid;
  int ret;

  /* The camera SCCB interface and the touch controller share I2C0 on this
   * board.  esp_i2cbus_initialize() is reference counted, so initializing
   * it here as well is safe whichever driver runs first.
   */

  i2c = esp_i2cbus_initialize(ESP32P4_CAMERA_I2C_PORT);
  if (i2c == NULL)
    {
      syslog(LOG_ERR,
             "ERROR: failed to get I2C%u bus for the camera SCCB\n",
             ESP32P4_CAMERA_I2C_PORT);
      return -ENODEV;
    }

  /* Probe the sensor before touching the capture controller so that a
   * missing or unpowered camera is reported clearly.
   */

  chipid = sc2336_chipid(i2c, ESP32P4_CAMERA_I2C_ADDR);
  if (chipid < 0)
    {
      syslog(LOG_ERR, "ERROR: no SC2336 at 0x%02x: %d\n",
             ESP32P4_CAMERA_I2C_ADDR, chipid);
      ret = chipid;
      goto errout_i2c;
    }

  syslog(LOG_INFO, "SC2336 chip ID: 0x%04x\n", chipid);

  /* Sensor half: SCCB control. */

  g_camera_sensors[0] = sc2336_initialize(i2c, ESP32P4_CAMERA_I2C_ADDR);
  if (g_camera_sensors[0] == NULL)
    {
      ret = -errno;
      syslog(LOG_ERR, "ERROR: sc2336_initialize failed: %d\n", ret);
      goto errout_i2c;
    }

  /* Data half: MIPI-CSI controller (MIPI PHY LDO, CSI host, CSI bridge and
   * DW-GDMA).
   */

  esp_csi_get_default_config(&csi_config);

  g_camera_imgdata = esp_csi_imgdata_initialize(&csi_config);
  if (g_camera_imgdata == NULL)
    {
      ret = -errno;
      syslog(LOG_ERR, "ERROR: esp_csi_imgdata_initialize failed: %d\n",
             ret);
      goto errout_sensor;
    }

  ret = capture_register(CONFIG_ESP32P4_FUNCTION_EV_BOARD_CAMERA_DEVPATH,
                         g_camera_imgdata, g_camera_sensors, 1);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: capture_register(%s) failed: %d\n",
             CONFIG_ESP32P4_FUNCTION_EV_BOARD_CAMERA_DEVPATH, ret);
      goto errout_csi;
    }

  syslog(LOG_INFO, "Camera registered on %s\n",
         CONFIG_ESP32P4_FUNCTION_EV_BOARD_CAMERA_DEVPATH);

  return OK;

errout_csi:
  esp_csi_imgdata_uninitialize(g_camera_imgdata);
  g_camera_imgdata = NULL;

errout_sensor:
  sc2336_uninitialize();
  g_camera_sensors[0] = NULL;

errout_i2c:
  esp_i2cbus_uninitialize(i2c);
  return ret;
}

/****************************************************************************
 * Name: esp32p4_camera_uninitialize
 ****************************************************************************/

int esp32p4_camera_uninitialize(void)
{
  int ret = OK;

  if (g_camera_imgdata != NULL)
    {
      ret = capture_unregister(
              CONFIG_ESP32P4_FUNCTION_EV_BOARD_CAMERA_DEVPATH);
      esp_csi_imgdata_uninitialize(g_camera_imgdata);
      g_camera_imgdata = NULL;
    }

  if (g_camera_sensors[0] != NULL)
    {
      sc2336_uninitialize();
      g_camera_sensors[0] = NULL;
    }

  return ret;
}

#endif /* CONFIG_ESP32P4_FUNCTION_EV_BOARD_CAMERA */
