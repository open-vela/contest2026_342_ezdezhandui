/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_hosted_sdio.c
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
 * The Wi-Fi transport of an ESP32-P4 board
 *
 * An ESP32-P4 has no radio: SOC_WIFI_SUPPORTED is not defined for it (compare
 * soc/esp32c6/include/soc/soc_caps.h, which does define it).  Wi-Fi on a P4
 * board is therefore always a *remote* capability, reached over a physical
 * link to a companion radio module -- on the ESP32-P4X-C5-Function-EV-Board
 * that companion is an ESP32-C5-MINI-1 wired to the P4 over 4-bit SDIO.
 *
 * That makes the bring-up a three-layer problem, and this file is only the
 * bottom layer:
 *
 *   +---------------------------------------------------------------+
 *   | remote Wi-Fi net device  (a NuttX netdev backed by the peer)   |  layer 3
 *   +---------------------------------------------------------------+
 *   | ESP-Hosted framing / RPC over the SDIO data path               |  layer 2
 *   +---------------------------------------------------------------+
 *   | P4 SDMMC controller in SDIO mode  <-- THIS FILE                |  layer 1
 *   +---------------------------------------------------------------+
 *
 * The split is deliberate: layer 1 can be built, flashed and debugged against
 * any SDIO card, and its progress is observable on its own, without layer 2
 * existing.  That is why esp_hosted_sdio_initialize() reports a state that
 * names the furthest *proven* milestone rather than a bare pass/fail.
 *
 * ## Current status of layer 1
 *
 * There is no SDMMC/SDIO host driver for esp32p4 in this tree yet.  The
 * directory has only esp_sdm.c (a sigma-delta modulator, unrelated to SD),
 * and the HAL exposes sdmmc_hal.c without any NuttX glue.  The esp32s3 port
 * does have a complete one (arch/xtensa/src/esp32s3/esp32s3_sdmmc.c, ~3000
 * lines implementing the full struct sdio_ops_s) and is the model to port,
 * but the two chips differ in register layout, clock tree, GDMA channel
 * allocation and interrupt routing (P4 uses CLIC rather than PLIC), so it is
 * a port and not a copy.
 *
 * Until that driver lands, this file reports -ENOSYS from the host step.
 * That is intentional and is not the same as claiming the link works: a
 * caller can distinguish
 *
 *   -ENOSYS   no host driver compiled in yet       (a tree/gap problem)
 *   -ENODEV   controller init failed               (a configuration problem)
 *   -ETIMEDOUT / -EIO  peer did not answer         (a hardware problem)
 *
 * The scaffolding exists now so that the port has a fixed interface to land
 * behind, and so that the layering above it can be written and reviewed
 * independently of the driver work.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <inttypes.h>
#include <syslog.h>

#include <nuttx/mmcsd.h>
#include <nuttx/signal.h>
#include <nuttx/sdio.h>

#include "esp_gpio.h"
#include "esp_hosted_sdio.h"
#include "esp_sdmmc.h"
#include "eh_frame.h"

#ifdef CONFIG_ESPRESSIF_HOSTED_SDIO

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Bring-up milestones are logged through syslog under one tag so that a test
 * can assert on them without depending on the message wording elsewhere.
 */

#define HOSTED_TAG "esp_hosted_sdio"

/* Block size esp-hosted uses on both functions (common/eh_common_sdio_cfg.h and
 * the host port both assume 512).
 */

#define ESP_HOSTED_SDIO_BLOCK_SIZE 512

/* How long to keep asking for the companion before giving up.
 *
 * The companion boots its own firmware before it can answer, so a single probe
 * right after the enable delay measures how fast it boots rather than whether
 * the link works -- hence a retry window. But this runs during boot, and every
 * millisecond here is a millisecond the console is not up, so the window is
 * deliberately short: ~1.5 s is comfortably more than a C6 needs to come up,
 * and an absent companion costs the user 1.5 s rather than 5.
 */

#define ESP_HOSTED_SDIO_PROBE_TRIES       12
#define ESP_HOSTED_SDIO_PROBE_INTERVAL_MS 250

/* Defaults for the companion enable line.
 *
 * These mirror ESPRESSIF_HOSTED_SDIO_EN_GPIO / _EN_DELAY_MS so that the driver
 * still behaves correctly in a tree whose generated config.h predates those
 * options (which is exactly what happens when Kconfig changes without a
 * reconfigure).  The Kconfig values win whenever they are present.
 */

#ifndef CONFIG_ESPRESSIF_HOSTED_SDIO_EN_GPIO
#  define CONFIG_ESPRESSIF_HOSTED_SDIO_EN_GPIO 54
#endif

#ifndef CONFIG_ESPRESSIF_HOSTED_SDIO_EN_DELAY_MS
#  define CONFIG_ESPRESSIF_HOSTED_SDIO_EN_DELAY_MS 200
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static enum esp_hosted_sdio_state_e g_hosted_state = ESP_HOSTED_SDIO_RESET;
static int g_hosted_errcode;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp_hosted_sdio_state
 *
 * Description:
 *   Report the furthest proven bring-up milestone.  See the header.
 *
 ****************************************************************************/

enum esp_hosted_sdio_state_e esp_hosted_sdio_state(void)
{
  return g_hosted_state;
}

/****************************************************************************
 * Name: esp_hosted_sdio_initialize
 *
 * Description:
 *   Layer 1 of the Wi-Fi transport: get the P4 SDIO host controller up and
 *   enumerate the peer's function blocks.  See the header for the contract.
 *
 ****************************************************************************/

int esp_hosted_sdio_initialize(void)
{
  FAR struct sdio_dev_s *sdio;
  int ret;

  if (g_hosted_state == ESP_HOSTED_SDIO_ENUMERATED ||
      g_hosted_state == ESP_HOSTED_SDIO_HANDSHAKE ||
      g_hosted_state == ESP_HOSTED_SDIO_UP)
    {
      return OK;
    }

  /* Step 0: bring the companion module out of reset.
   *
   * On this board the ESP32-C5 is gated by a GPIO from the ESP32-P4 (GPIO54 by
   * default: the J1 pin table describes it as "ESP32-P4 control of the
   * ESP32-C5 enable function").  While that line is low the module is held off
   * and there is nothing on the bus to answer a probe -- which is exactly what
   * the first attempts on hardware looked like: the controller came up fine and
   * sdio_probe() returned -EBUSY forever.
   *
   * The delay matters as much as the level: the companion boots its own
   * firmware after enable is asserted, and only then does it enumerate as an
   * SDIO card.
   */

#if CONFIG_ESPRESSIF_HOSTED_SDIO_EN_GPIO >= 0
  ret = esp_configgpio(CONFIG_ESPRESSIF_HOSTED_SDIO_EN_GPIO, OUTPUT);
  if (ret < 0)
    {
      syslog(LOG_ERR, "%s: cannot drive companion enable GPIO%d: %d\n",
             HOSTED_TAG, CONFIG_ESPRESSIF_HOSTED_SDIO_EN_GPIO, ret);
    }
  else
    {
      /* Pulse the enable line rather than merely raising it.
       *
       * The line is the companion's chip enable, so holding it low is how the
       * module is held in reset.  Leaving it high and expecting enumeration
       * only works if the module happens to have just powered on -- and at
       * boot it may instead be running (or stuck in) whatever state it was in
       * before the host restarted.  Driving low, waiting, then releasing gives
       * the module a definite reset and a clean start.
       *
       * This matters because the alternative explanation for "no card answers"
       * is a companion that never restarted, and that looks identical on the
       * bus.
       */

      /* Hold the enable high and do NOT pulse it.
       *
       * Pulsing reboots the companion, and a module that is mid-boot drives its
       * SDIO pins in an undefined state -- which on this bus shows up as the
       * host controller being unable to complete even the broadcast CMD0, i.e.
       * -EBUSY rather than the "no card answered" -ETIMEDOUT. Since the module
       * is already running, leaving it alone and simply asking is the right
       * first move; a reset is only wanted if the link is known wedged.
       */

      esp_gpiowrite(CONFIG_ESPRESSIF_HOSTED_SDIO_EN_GPIO, true);
      syslog(LOG_INFO,
             "%s: companion enable held high (GPIO%d), settling %d ms\n",
             HOSTED_TAG, CONFIG_ESPRESSIF_HOSTED_SDIO_EN_GPIO,
             CONFIG_ESPRESSIF_HOSTED_SDIO_EN_DELAY_MS);

      nxsig_usleep(CONFIG_ESPRESSIF_HOSTED_SDIO_EN_DELAY_MS * 1000);
    }
#endif

  /* Step 1: the host controller.
   *
   * Slot 1 is the companion radio.  Slot 0 is the microSD card, which is a
   * different electrical path (IO_MUX rather than GPIO matrix), so the two
   * cannot be confused for one another.
   */

  sdio = esp_sdmmc_initialize(CONFIG_ESPRESSIF_HOSTED_SDIO_SLOT);
  if (sdio == NULL)
    {
      g_hosted_state   = ESP_HOSTED_SDIO_FAILED;
      g_hosted_errcode = -ENODEV;
      syslog(LOG_ERR, "%s: SDMMC slot %d did not come up\n",
             HOSTED_TAG, CONFIG_ESPRESSIF_HOSTED_SDIO_SLOT);
      return -ENODEV;
    }

  /* Attach the interrupt and start the identification clock.
   *
   * Both are mandatory before any command can be issued:
   *
   *  - without SDIO_ATTACH the controller's completion interrupt is never
   *    installed, so nothing ever signals that a command finished;
   *  - without a clock the CIU cannot shift a command out at all, and the
   *    START bit never clears -- the next send then spins until it times out
   *    and reports -EBUSY, which is exactly what the first hardware runs did.
   *
   * mmcsd_sdio_initialize() does the same two steps (and itself returns
   * -EBUSY when attach fails), which is worth knowing when reading this error.
   */

  ret = SDIO_ATTACH(sdio);
  if (ret < 0)
    {
      g_hosted_state   = ESP_HOSTED_SDIO_FAILED;
      g_hosted_errcode = ret;
      syslog(LOG_ERR, "%s: cannot attach the SDIO interrupt: %d\n",
             HOSTED_TAG, ret);
      return ret;
    }

  SDIO_CLOCK(sdio, CLOCK_IDMODE);
  nxsig_usleep(20000);

  g_hosted_state = ESP_HOSTED_SDIO_HOST_READY;
  syslog(LOG_INFO, "%s: SDIO host ready on slot %d\n",
         HOSTED_TAG, CONFIG_ESPRESSIF_HOSTED_SDIO_SLOT);

  /* Step 2: enumeration.
   *
   * sdio_probe() runs the SDIO card identification sequence (CMD5, then
   * CMD3/CMD7) and reads the common I/O area.  Getting here means the peer
   * answered, which is the first point at which the link is known to be
   * electrically sound -- everything before this was the host talking to
   * itself.
   *
   * The CIA read yields the function count and per-function block size that
   * the ESP-Hosted layer above needs, so it is the natural boundary between
   * the two.
   */

  /* Report what the controller can see, then enumerate. */

  {
    FAR struct esp_sdmmc_debug_s dbg;

    esp_sdmmc_debug(&dbg);
    syslog(LOG_INFO, "%s: verid=%08" PRIx32 " status=%08" PRIx32
           " cdetect=%08" PRIx32 " clkena=%08" PRIx32 "\n",
           HOSTED_TAG, dbg.verid, dbg.status, dbg.cdetect, dbg.clkena);
  }

  /* Give the companion a real chance to come up before declaring failure.
   *
   * The module has to boot its own RTOS and start the SDIO slave before it can
   * answer CMD5, and that takes appreciably longer than the enable-to-first-
   * command gap. Probing once right after the enable delay therefore measures
   * how fast the companion boots rather than whether the link works at all.
   * Retry for a bounded window instead.
   */

  {
    int attempt;

    for (attempt = 1; attempt <= ESP_HOSTED_SDIO_PROBE_TRIES; attempt++)
      {
        ret = sdio_probe(sdio);
        if (ret >= 0)
          {
            break;
          }

        syslog(LOG_INFO, "%s: probe %d/%d -> %d, retrying\n",
               HOSTED_TAG, attempt, ESP_HOSTED_SDIO_PROBE_TRIES, ret);

        esp_sdmmc_bus_reset();
        nxsig_usleep(ESP_HOSTED_SDIO_PROBE_INTERVAL_MS * 1000);
      }
  }

  if (ret < 0)
    {
      g_hosted_state   = ESP_HOSTED_SDIO_FAILED;
      g_hosted_errcode = ret;
      syslog(LOG_ERR, "%s: no SDIO response from the companion module: %d\n",
             HOSTED_TAG, ret);
      return ret;
    }

  g_hosted_state = ESP_HOSTED_SDIO_ENUMERATED;
  syslog(LOG_INFO, "%s: companion enumerated\n", HOSTED_TAG);

  /* Card-function bring-up, following the sequence esp-hosted's own host port
   * performs (port/os/idf/src/eh_host_port_sdio.c, card_fn_init):
   *
   *   1. read CCCR FN_ENABLE / FN_READY
   *   2. enable function 1 and wait for it to report ready
   *   3. enable interrupts for function 1 plus the master enable
   *   4. set the FN0 and FN1 block size to 512
   *   5. switch the bus to 4 bits
   *
   * The ordering matters: the block-size registers and the bus width are only
   * meaningful once function 1 has been enabled and has come ready.
   *
   * NuttX's generators do the same CCCR accesses, so most of the sequence is
   * the generic helpers rather than raw CMD52 calls.
   */

  ret = sdio_set_blocksize(sdio, 0, ESP_HOSTED_SDIO_BLOCK_SIZE);
  if (ret < 0)
    {
      syslog(LOG_ERR, "%s: FN0 block size failed: %d\n", HOSTED_TAG, ret);
      goto bringup_failed;
    }

  ret = sdio_enable_function(sdio, 1);
  if (ret < 0)
    {
      syslog(LOG_ERR, "%s: function 1 did not enable: %d\n", HOSTED_TAG, ret);
      goto bringup_failed;
    }

  ret = sdio_enable_interrupt(sdio, 1);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "%s: function 1 interrupt enable failed: %d\n",
             HOSTED_TAG, ret);
    }

  ret = sdio_set_blocksize(sdio, 1, ESP_HOSTED_SDIO_BLOCK_SIZE);
  if (ret < 0)
    {
      syslog(LOG_ERR, "%s: FN1 block size failed: %d\n", HOSTED_TAG, ret);
      goto bringup_failed;
    }

  ret = sdio_set_wide_bus(sdio);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "%s: 4-bit bus not available, staying 1-bit: %d\n",
             HOSTED_TAG, ret);
    }

  g_hosted_state   = ESP_HOSTED_SDIO_ENUMERATED;
  g_hosted_errcode = 0;

  /* Bring up the frame layer, which is the wire format both sides speak.
   *
   * These are the settings esp-hosted's own host port uses for an MCU host on
   * SDIO: V1 headers with the checksum enabled. Initialising it here means the
   * layer is live and validated, not vendored dead code waiting for a caller.
   */

  {
    eh_frame_cfg_t ehcfg = EH_FRAME_CFG_HOST_MCU_SDIO_DEFAULT;

    if (eh_frame_init(&ehcfg) != ESP_OK)
      {
        syslog(LOG_ERR, "%s: frame layer init failed\n", HOSTED_TAG);
        goto bringup_failed;
      }

    syslog(LOG_INFO,
           "%s: card function ready (FN1, %u-byte blocks), frame layer up "
           "(hdr v%u, %u-byte headers, checksum %s)\n",
           HOSTED_TAG, (unsigned)ESP_HOSTED_SDIO_BLOCK_SIZE,
           ehcfg.hdr_version, (unsigned)eh_frame_hdr_size(),
           eh_frame_checksum_enabled() ? "on" : "off");
  }

  /* The data path itself is not wired yet: moving frames over SDIO needs the
   * CMD53 buffer protocol (packet-length and token registers) on top of the
   * card function brought up above. That work is deliberately left until the
   * companion answers on this bus -- see docs/08 appendix F.
   */

  g_hosted_state   = ESP_HOSTED_SDIO_ENUMERATED;
  g_hosted_errcode = 0;

  return OK;

bringup_failed:
  g_hosted_state   = ESP_HOSTED_SDIO_FAILED;
  g_hosted_errcode = ret;
  return ret;

  /* Layers 2 and 3 -- the ESP-Hosted framing and the mapping of the remote
   * radio onto a net device -- are not implemented yet.  Reporting the
   * transport as ready while saying so plainly is more useful than pretending
   * the link is complete.
   */

  return OK;
}

#endif /* CONFIG_ESPRESSIF_HOSTED_SDIO */
