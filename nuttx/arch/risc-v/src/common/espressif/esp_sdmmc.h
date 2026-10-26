/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_sdmmc.h
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

#ifndef __ARCH_RISCV_SRC_COMMON_ESPRESSIF_ESP_SDMMC_H
#define __ARCH_RISCV_SRC_COMMON_ESPRESSIF_ESP_SDMMC_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#ifndef __ASSEMBLY__

#undef EXTERN
#if defined(__cplusplus)
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

struct sdio_dev_s;

/* Snapshot of the controller registers, for bring-up diagnostics.
 *
 * Kept as a small struct rather than exposing sdmmc_dev_t so that a caller does
 * not start reaching into the register block.
 */

struct esp_sdmmc_debug_s
{
  uint32_t verid;                 /* Controller version                   */
  uint32_t status;                /* CIU status                           */
  uint32_t cdetect;               /* Card detect                          */
  uint32_t clkena;                /* Card clock enable                    */
  uint32_t clkdiv;                /* Clock dividers                       */
  uint32_t ctrl;                  /* Control                              */
  uint32_t rintsts;               /* Raw interrupt status                 */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef CONFIG_ESPRESSIF_SDMMC

/****************************************************************************
 * Name: esp_sdmmc_initialize
 *
 * Description:
 *   Initialise one of the ESP32-P4's two SDMMC slots and return a NuttX SDIO
 *   lower half for it.
 *
 *   The two slots are not equivalent on this chip, which is why the slot is
 *   a parameter and not a build-time constant:
 *
 *     slot 0   microSD card   IO_MUX pins        CLK43 CMD44 D0..D3 39..42
 *     slot 1   companion      GPIO matrix pins   CLK18 CMD19 D0..D3 14..17
 *
 *   The pin numbers come from CONFIG_ESPRESSIF_SDMMC_*; the Kconfig defaults
 *   match the ESP32-P4X-C5-Function-EV-Board, so a board that routes them
 *   differently only needs to change those values.
 *
 *   The call is idempotent.  A second call returns the same instance, because
 *   there is a single controller: two instances would fight over it, exactly
 *   as two audio devices would fight over one I2S port.
 *
 * Input Parameters:
 *   slotno - Slot to drive: 0 (microSD) or 1 (companion radio).
 *
 * Returned Value:
 *   A valid SDIO device reference on success; NULL on failure.
 *
 ****************************************************************************/

struct sdio_dev_s *esp_sdmmc_initialize(int slotno);

/****************************************************************************
 * Name: esp_sdmmc_debug
 *
 * Description:
 *   Snapshot the controller registers for a bring-up log.
 *
 ****************************************************************************/

void esp_sdmmc_debug(FAR struct esp_sdmmc_debug_s *dbg);

/****************************************************************************
 * Name: esp_sdmmc_bus_reset
 *
 * Description:
 *   Reset the controller between enumeration attempts, so a failed probe does
 *   not leave the CIU holding a stale command or a half-finished transfer.
 *
 ****************************************************************************/

void esp_sdmmc_bus_reset(void);

/****************************************************************************
 * Name: esp_sdmmc_skip_clk_update
 *
 * Description:
 *   Bring-up aid: run without the update_clk_reg command. See the comment on
 *   g_sdmmc_skip_clk_update in esp_sdmmc.c.
 *
 ****************************************************************************/

void esp_sdmmc_skip_clk_update(bool skip);

/****************************************************************************
 * Name: esp_sdmmc_probe_clk
 *
 * Description:
 *   Bring-up aid: sample the CLK pad while commands are in flight.
 *
 ****************************************************************************/

void esp_sdmmc_probe_clk(bool enable);

#endif /* CONFIG_ESPRESSIF_SDMMC */

#undef EXTERN
#if defined(__cplusplus)
}
#endif

#endif /* __ASSEMBLY__ */
#endif /* __ARCH_RISCV_SRC_COMMON_ESPRESSIF_ESP_SDMMC_H */
