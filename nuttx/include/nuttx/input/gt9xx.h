/****************************************************************************
 * include/nuttx/input/gt9xx.h
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

#ifndef __INCLUDE_NUTTX_INPUT_GT9XX_H
#define __INCLUDE_NUTTX_INPUT_GT9XX_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/compiler.h>
#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Callback for Board-Specific Operations */

struct gt9xx_board_s
{
  /* Attach the Interrupt Handler for Touch Panel */

  int (*irq_attach) (const struct gt9xx_board_s *state,
                     xcpt_t isr,
                     FAR void *arg);

  /* Enable or disable Interrupts for the Touch Panel */

  void (*irq_enable) (const struct gt9xx_board_s *state, bool enable);

  /* Power on or off the Touch Panel */

  int (*set_power) (const struct gt9xx_board_s *state, bool on);

  /* Report the touch panel's coordinate space when the controller is mounted
   * rotated 180 degrees relative to the display.
   *
   * The GT9xx reports coordinates in the resolution configured in its own
   * config block and its axes cannot be assumed to line up with the panel:
   * on the ESP32-P4 Function-EV board the module is mounted the other way
   * round, so a touch at the panel's top-left corner reads back as the
   * maximum coordinates.  When both of these are non-zero the driver mirrors
   * both axes into 0..xmirror-1 / 0..ymirror-1 before reporting, which puts
   * the coordinates back into panel space.
   *
   * Leave them zero to pass the controller's raw coordinates through.
   */

  uint16_t xmirror;
  uint16_t ymirror;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: gt9xx_register
 *
 * Description:
 *   Register the driver for Goodix GT9XX Touch Panel.  Attach the
 *   Interrupt Handler for the Touch Panel and disable Touch Interrupts.
 *
 * Input Parameters:
 *   devpath      - Device Path (e.g. "/dev/input0")
 *   dev          - I2C Bus
 *   i2c_devaddr  - I2C Address of Touch Panel
 *   board_config - Callback for Board-Specific Operations
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value is returned on any failure.
 *
 ****************************************************************************/

int gt9xx_register(FAR const char *devpath,
                   FAR struct i2c_master_s *dev,
                   uint8_t i2c_devaddr,
                   const struct gt9xx_board_s *board_config);

#endif /* __INCLUDE_NUTTX_INPUT_GT9XX_H */
