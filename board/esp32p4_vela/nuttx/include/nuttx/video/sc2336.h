/****************************************************************************
 * include/nuttx/video/sc2336.h
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

#ifndef __INCLUDE_NUTTX_VIDEO_SC2336_H
#define __INCLUDE_NUTTX_VIDEO_SC2336_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include <nuttx/i2c/i2c_master.h>
#include <nuttx/video/imgsensor.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Default SCCB address, in 7-bit form. */

#define SC2336_I2C_ADDR  0x30

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Name: sc2336_initialize
 *
 * Description:
 *   Probe an SC2336 on the supplied SCCB/I2C bus and return its
 *   imgsensor_s interface, which is what capture_register() expects for the
 *   sensor half of a V4L2 capture device.
 *
 *   The returned interface stays valid until sc2336_uninitialize() is
 *   called; there is a single sensor instance, as on the reference board.
 *
 * Input Parameters:
 *   i2c  - I2C master bus carrying the sensor SCCB interface.  The caller
 *          keeps ownership of the bus.
 *   addr - 7-bit SCCB address of the sensor (SC2336_I2C_ADDR unless the
 *          module straps something else).
 *
 * Returned Value:
 *   A pointer to the imgsensor interface on success; NULL is returned on
 *   failure with the errno set to the specific error.
 *
 ****************************************************************************/

FAR struct imgsensor_s *sc2336_initialize(FAR struct i2c_master_s *i2c,
                                          uint8_t addr);

/****************************************************************************
 * Name: sc2336_uninitialize
 *
 * Description:
 *   Release the sensor instance created by sc2336_initialize().
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure.
 *
 ****************************************************************************/

int sc2336_uninitialize(void);

/****************************************************************************
 * Name: sc2336_chipid
 *
 * Description:
 *   Read and return the sensor identification register.  Useful for board
 *   bring-up diagnostics before the full capture path is enabled.
 *
 * Returned Value:
 *   The 16-bit value of the sensor ID registers on success; a negated errno
 *   on failure.
 *
 ****************************************************************************/

int sc2336_chipid(FAR struct i2c_master_s *i2c, uint8_t addr);

#undef EXTERN
#ifdef __cplusplus
}
#endif

#endif /* __INCLUDE_NUTTX_VIDEO_SC2336_H */
