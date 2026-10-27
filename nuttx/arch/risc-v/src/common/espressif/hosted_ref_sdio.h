/****************************************************************************
 * chips/esp32p4/include/esp_hosted_sdio.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_ESP32P4_INCLUDE_REFSDIO_H
#define __ARCH_RISCV_SRC_ESP32P4_INCLUDE_REFSDIO_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define REFSDIO_SLOT0             0
#define REFSDIO_SLOT1             1

#define REFSDIO_WIDTH1            1
#define REFSDIO_WIDTH4            4

#define REFSDIO_PROBING_CLOCK_KHZ 400
#define REFSDIO_MAX_CLOCK_KHZ     40000

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct refsdio_s;

struct refsdio_config_s
{
  uint8_t  slot;
  uint8_t  bus_width;
  uint32_t clock_khz;
};

struct refsdio_status_s
{
  uint32_t revision;
  uint32_t hardware_config;
  uint32_t clock_khz;
  uint8_t  slot;
  uint8_t  bus_width;
  bool     initialized;
};

/* The board provides its GPIO wiring while this chip-layer interface owns
 * the ESP32-P4 SDMMC slot 1 GPIO-matrix signal selection.  C6 reset and
 * wakeup remain board-owned and are deliberately absent from this type.
 */

struct refsdio_pins_s
{
  int clk_gpio;
  int cmd_gpio;
  int d0_gpio;
  int d1_gpio;
  int d2_gpio;
  int d3_gpio;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Name: refsdio_route_pins
 *
 * Description:
 *   Configure board-provided GPIOs for ESP32-P4 SDMMC slot 1 probing.  The
 *   function routes CLK, CMD and DAT0 to the SDMMC matrix, leaves DAT1 and
 *   DAT2 pulled up, and holds DAT3 high as GPIO so the attached C6 enters
 *   SDIO mode before the later four-bit bus-width negotiation.
 *
 ****************************************************************************/

int refsdio_route_pins(
  FAR const struct refsdio_pins_s *pins);

/****************************************************************************
 * Name: refsdio_initialize
 *
 * Description:
 *   Acquire and reset the single ESP32-P4 SDMMC controller for a
 *   board-routed ESP-Hosted SDIO device.  The call configures the clock and
 *   bus width, but it does not configure GPIOs, reset the coprocessor, or
 *   perform CMD52/CMD53 transfers.
 *
 ****************************************************************************/

int refsdio_initialize(
  FAR const struct refsdio_config_s *config,
  FAR struct refsdio_s **host);

/****************************************************************************
 * Name: refsdio_set_clock
 *
 * Description:
 *   Change the selected slot clock.  clock_khz must be in the range from
 *   REFSDIO_PROBING_CLOCK_KHZ through
 *   REFSDIO_MAX_CLOCK_KHZ.
 *
 ****************************************************************************/

int refsdio_set_clock(FAR struct refsdio_s *host,
                               uint32_t clock_khz);

/****************************************************************************
 * Name: refsdio_command
 *
 * Description:
 *   Submit one synchronous command without a data phase.  This primitive is
 *   intended for SDIO discovery commands such as CMD0 and CMD5.  If response
 *   is false, response_value may be NULL.  send_init emits the initial 80
 *   clocks and must be used only for the first command after a coprocessor
 *   reset.
 *
 ****************************************************************************/

int refsdio_command(FAR struct refsdio_s *host,
                             uint8_t command, uint32_t argument,
                             bool response, bool check_crc, bool send_init,
                             FAR uint32_t *response_value);

/* Synchronous CMD53.  Transfers up to four bytes use programmed I/O; larger
 * transfers use the ESP32-P4 SDMMC IDMAC.  Byte mode accepts 1..512 bytes;
 * block mode accepts multiples of the negotiated block size.  Callers split
 * byte-mode lengths that are not a multiple of four before calling this API.
 * A failed data phase makes this host unusable until deinitialized/reset.
 */

int refsdio_transfer(FAR struct refsdio_s *host,
                            uint32_t argument, FAR void *buffer,
                            size_t length, uint16_t block_size);

/****************************************************************************
 * Name: refsdio_get_status
 *
 * Description:
 *   Return the controller identification and active transport parameters for
 *   diagnostics.  The information describes only the P4 SDMMC controller;
 *   it does not imply that a C6 is attached or that ESP-Hosted has completed
 *   its protocol handshake.
 *
 ****************************************************************************/

int refsdio_get_status(FAR struct refsdio_s *host,
                                FAR struct refsdio_status_s *status);

/****************************************************************************
 * Name: refsdio_deinitialize
 *
 * Description:
 *   Stop the selected slot clock and release the P4 SDMMC controller.
 *
 ****************************************************************************/

int refsdio_deinitialize(FAR struct refsdio_s *host);

#endif /* __ARCH_RISCV_SRC_ESP32P4_INCLUDE_REFSDIO_H */
