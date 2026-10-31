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
  uint32_t cmd;                   /* Command register                     */
  uint32_t ctrl;                  /* Control                              */
  uint32_t rintsts;               /* Raw interrupt status                 */
  uint32_t bmod;                  /* Bus mode                             */
  uint32_t cmdarg;                /* Command argument as latched          */
  uint32_t sys_clk_ctrl1;         /* HP_SYS_CLKRST.soc_clk_ctrl1          */
  uint32_t peri_clk_ctrl01;       /* HP_SYS_CLKRST.peri_clk_ctrl01        */
  uint32_t peri_clk_ctrl02;       /* HP_SYS_CLKRST.peri_clk_ctrl02        */
  uint32_t ref_clk_ctrl2;         /* HP_SYS_CLKRST.ref_clk_ctrl2          */
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
 * Name: esp_sdmmc_route_pins
 *
 * Description:
 *   Attach the slot's pads to the controller, without resetting the controller
 *   or touching its clock tree.
 *
 *   The companion must be brought out of reset *after* the pads are already in
 *   a defined state: a CLK pin left floating while the companion boots can be
 *   read by it as edges that never happened. Routing first and resetting second
 *   is the order the working implementation for this board uses.
 *
 ****************************************************************************/

void esp_sdmmc_route_pins(void);

void esp_sdmmc_dma_state(void);

/****************************************************************************
 * Name: esp_sdmmc_try_cmd5_ocr
 *
 * Description:
 *   Issue CMD5 with a real OCR argument and report whether it was answered.
 *
 ****************************************************************************/

bool esp_sdmmc_try_cmd5_ocr(uint32_t ocr);

/****************************************************************************
 * Name: esp_sdmmc_report_routing
 *
 * Description:
 *   Read back each pad's GPIO-matrix output selection and report whether it
 *   really points at the controller signal it should.
 *
 ****************************************************************************/

void esp_sdmmc_report_routing(void);

/****************************************************************************
 * Name: esp_sdmmc_identify_pins
 *
 * Description:
 *   Emit 1..6 pulses on CLK..D3 so the analyser's channel-to-pin mapping can
 *   be read off directly instead of assumed.
 *
 ****************************************************************************/

void esp_sdmmc_identify_pins(void);

/****************************************************************************
 * Name: esp_sdmmc_sample_idle
 *
 * Description:
 *   Read the slot's pads as plain GPIOs, with and without the internal
 *   pull-ups, and log what each line is doing while the bus is idle.
 *
 ****************************************************************************/

void esp_sdmmc_sample_idle(void);

/****************************************************************************
 * Name: esp_sdmmc_wait_card_intr
 *
 * Description:
 *   Wait until the card raises its SDIO interrupt (DAT1 pulled low), i.e.
 *   until it has something queued for the host.
 *
 *   This is the step esp-hosted's own host performs before reading any
 *   register. The companion asserts the line as soon as it has data, so
 *   waiting for it is what turns "poll INT_RAW and hope" into a protocol.
 *
 * Input Parameters:
 *   dev        - The SDIO device to watch
 *   timeout_ms - How long to wait, in milliseconds
 *
 * Returned Value:
 *   true if the card signalled within the timeout, false otherwise.
 *
 ****************************************************************************/

bool esp_sdmmc_wait_card_intr(struct sdio_dev_s *dev, unsigned int timeout_ms);

/****************************************************************************
 * Name: esp_sdmmc_last_error_bits
 *
 * Description:
 *   The RINTSTS error bits seen during the most recent data transfer, as the
 *   driver's own wait loop recorded them. Zero when the last transfer reported
 *   none. Reading this from the diagnostic code puts the value in the same line
 *   as the transfer's result, where it cannot be lost between two log lines.
 *
 ****************************************************************************/

uint32_t esp_sdmmc_last_error_bits(void);

void esp_sdmmc_dump_state(FAR const char *where);

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
