/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_csi.h
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

#ifndef __ARCH_RISCV_SRC_COMMON_ESPRESSIF_ESP_CSI_H
#define __ARCH_RISCV_SRC_COMMON_ESPRESSIF_ESP_CSI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include <nuttx/video/imgdata.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* LDO channel that feeds the ESP32-P4 MIPI PHY.  On the
 * ESP32-P4X-C5-Function-EV-Board the camera (CSI) and display (DSI) share
 * this domain, so the channel is non-adjustable and may be acquired by both
 * consumers.  See the board schematic / Espressif docs.
 */

#define ESP_CSI_LDO_CHAN_ID   3
#define ESP_CSI_LDO_VOLTAGE   2500

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Static description of a MIPI-CSI capture.  All fields have board level
 * defaults supplied by esp_csi_get_default_config(); a board only has to
 * override what differs from the defaults.
 */

struct esp_csi_config_s
{
  /* Frame geometry, in pixels/lines.  These describe the CSI receiver
   * window, i.e. the size of one frame as it appears on the CSI link.
   */

  uint32_t h_res;
  uint32_t v_res;

  /* Link parameters. */

  uint8_t  data_lane_num;
  uint32_t lane_bit_rate_mbps;

  /* Color formats, expressed as the ESP-IDF cam_ctlr_color_t enumeration
   * (CAM_CTLR_COLOR_RAW8, CAM_CTLR_COLOR_RAW10, CAM_CTLR_COLOR_RGB565,
   * CAM_CTLR_COLOR_YUV422_*, ...).
   *
   * NOTE: the ESP32-P4 CSI bridge can only convert between RGB and YUV; a
   * RAW sensor stream has to be converted by the ISP block.  Keep input and
   * output equal (pass through) until an ISP stage is wired up.
   */

  uint32_t input_color;
  uint32_t output_color;

  /* Enable the CSI bridge byte swap.  The SC2336 transmits RAW10 in the
   * order the ISP expects, so this stays disabled by default.
   */

  bool     byte_swap_en;

  /* Enable the CSI bridge input bit/byte swaps (P4 rev3 and later only). */

  bool     input_8bit_swap_en;
  bool     input_16bit_swap_en;

  /* Bring up the MIPI PHY LDO as part of initialize().  Boards that already
   * power the PHY from another driver (for example a DSI panel driver) may
   * disable this; esp_ldo_channel_acquire() is reference counted, so the
   * safest default is to acquire it here as well.
   */

  bool     enable_ldo;
  uint8_t  ldo_chan_id;
  uint16_t ldo_voltage_mv;

  /* Keep the driver internal ("backup") frame buffer.  It is what makes a
   * missed buffer harmless: when the application has not returned a frame
   * buffer in time the CSI DMA lands in this buffer instead of corrupting a
   * buffer that is still being read.  Disabling it turns a late buffer into
   * a fatal error inside the HAL, so keep it enabled.
   */

  bool     keep_backup_buffer;

  /* Number of transactions the HAL transaction queue can hold.  The queue is
   * not used by this driver (buffers are handed over through the
   * on_get_new_trans callback) but the HAL still allocates it.
   */

  int      queue_items;
};

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
 * Name: esp_csi_get_default_config
 *
 * Description:
 *   Fill in a config structure with the defaults used by the
 *   ESP32-P4X-C5-Function-EV-Board: 1920x1080, 2 MIPI data lanes, RAW10 in
 *   and out, LDO channel 3 at 2500 mV.
 *
 * Input Parameters:
 *   config - Structure to fill in.
 *
 ****************************************************************************/

void esp_csi_get_default_config(FAR struct esp_csi_config_s *config);

/****************************************************************************
 * Name: esp_csi_imgdata_initialize
 *
 * Description:
 *   Create the MIPI-CSI controller and wrap it in a NuttX imgdata_s
 *   lower-half, which can be handed to capture_register() together with an
 *   imgsensor_s lower-half.
 *
 *   The returned instance owns the CSI controller until the matching call to
 *   esp_csi_imgdata_uninitialize().
 *
 * Input Parameters:
 *   config - Capture description.  A copy is kept, the caller may reuse or
 *            discard the structure afterwards.
 *
 * Returned Value:
 *   A pointer to the imgdata interface on success; NULL is returned on
 *   failure with the errno set to the specific error.
 *
 ****************************************************************************/

FAR struct imgdata_s *
esp_csi_imgdata_initialize(FAR const struct esp_csi_config_s *config);

/****************************************************************************
 * Name: esp_csi_imgdata_uninitialize
 *
 * Description:
 *   Stop capture (if running) and release the CSI controller and the MIPI
 *   PHY LDO reference taken by esp_csi_imgdata_initialize().
 *
 * Input Parameters:
 *   data - Interface returned by esp_csi_imgdata_initialize().
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno on failure.
 *
 ****************************************************************************/

int esp_csi_imgdata_uninitialize(FAR struct imgdata_s *data);

/****************************************************************************
 * Name: esp_csi_frames
 *
 * Description:
 *   Diagnostic helper: number of frames delivered to the upper half and
 *   number of frames that had to be dropped because the application did not
 *   have a free buffer available.
 *
 ****************************************************************************/

void esp_csi_frames(FAR struct imgdata_s *data, FAR uint32_t *delivered,
                    FAR uint32_t *dropped, FAR uint32_t *errors);

#undef EXTERN
#ifdef __cplusplus
}
#endif

#endif /* __ARCH_RISCV_SRC_COMMON_ESPRESSIF_ESP_CSI_H */
