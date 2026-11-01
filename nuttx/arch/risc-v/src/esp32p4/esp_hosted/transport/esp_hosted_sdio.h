/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_hosted_sdio.h
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

#ifndef __ARCH_RISCV_SRC_COMMON_ESPRESSIF_ESP_HOSTED_SDIO_H
#define __ARCH_RISCV_SRC_COMMON_ESPRESSIF_ESP_HOSTED_SDIO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Lifecycle state of the hosted-transport link.
 *
 * The names describe what has been *proven* on hardware, not what has been
 * attempted, so a caller (or a log reader) can tell how far bring-up got.
 *
 *   RESET      - nothing done yet
 *   HOST_READY - the P4 SDIO host controller is initialised and its slot is
 *                registered with the SDIO stack
 *   ENUMERATED - a card responded to CMD5/CMD3 and the SDIO function blocks
 *                are visible through the common I/O area (CIA)
 *   HANDSHAKE  - the peer answered the ESP-Hosted protocol handshake
 *   UP         - the link carries traffic
 *   FAILED     - bring-up stopped; errcode says why
 */

enum esp_hosted_sdio_state_e
{
  ESP_HOSTED_SDIO_RESET = 0,
  ESP_HOSTED_SDIO_HOST_READY,
  ESP_HOSTED_SDIO_ENUMERATED,
  ESP_HOSTED_SDIO_HANDSHAKE,
  ESP_HOSTED_SDIO_UP,
  ESP_HOSTED_SDIO_FAILED
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef CONFIG_ESPRESSIF_HOSTED_SDIO

#undef EXTERN
#if defined(__cplusplus)
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Name: esp_hosted_sdio_initialize
 *
 * Description:
 *   Bring up the transport that carries the ESP-Hosted link between this
 *   ESP32-P4 and the board's companion radio module (ESP32-C5-MINI-1 on the
 *   ESP32-P4X-C5-Function-EV-Board).
 *
 *   The ESP32-P4 has no radio of its own -- SOC_WIFI_SUPPORTED is not
 *   defined for it -- so Wi-Fi on this board is inherently a *remote*
 *   capability reached over SDIO.  This entry point owns only the lower half
 *   of that path:
 *
 *     1. initialise the P4 SDMMC controller in SDIO mode on slot 1
 *        (SD2_CLK=GPIO18, SD2_CMD=GPIO19, SD2_D0..D3=GPIO14..17) and
 *        register it with the generic SDIO stack
 *     2. enumerate the peer's SDIO function blocks through the common I/O
 *        area
 *     3. leave the link in a state where the ESP-Hosted protocol layer can
 *        start talking
 *
 *   Everything above that -- the ESP-Hosted framing/RPC and the mapping of
 *   the remote radio onto a NuttX net device -- is a separate layer and is
 *   deliberately not implemented here, so the two can be built and debugged
 *   independently.
 *
 *   It is idempotent.
 *
 * Input Parameters:
 *   None.
 *
 * Return Value:
 *   Zero (OK) once the host side is up; a negated errno value otherwise.
 *   A non-zero return is reported for any step that is not yet available in
 *   this tree, so the caller can distinguish "no driver yet" (-ENOSYS) from
 *   "hardware did not answer" (-ENODEV / -ETIMEDOUT).
 *
 ****************************************************************************/

int esp_hosted_sdio_initialize(void);

/****************************************************************************
 * Name: esp_hosted_sdio_state
 *
 * Description:
 *   Report how far esp_hosted_sdio_initialize() got.  Intended for bring-up
 *   logging and for tests that need to assert a milestone without parsing
 *   the log.
 *
 * Input Parameters:
 *   None.
 *
 * Return Value:
 *   The current link state.
 *
 ****************************************************************************/

enum esp_hosted_sdio_state_e esp_hosted_sdio_state(void);

#undef EXTERN
#if defined(__cplusplus)
}
#endif

#endif /* CONFIG_ESPRESSIF_HOSTED_SDIO */

#endif /* __ARCH_RISCV_SRC_COMMON_ESPRESSIF_ESP_HOSTED_SDIO_H */
