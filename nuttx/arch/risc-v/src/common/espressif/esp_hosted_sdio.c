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

#include <soc/io_mux_reg.h>
#include <soc/sdmmc_pins.h>
#include <soc/soc.h>
#include <soc/gpio_sig_map.h>
#include <soc/gpio_struct.h>
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

/* Probe window.
 *
 * The companion's own log shows its SDIO slave is not up until roughly 960 ms
 * after release, and that wait is already spent in the settle delay below, so
 * only a short confirmation window is needed here. Keeping it short matters
 * because this runs during boot: a companion that never answers should cost a
 * couple of seconds, not the four that twelve probes would add.
 */

#define ESP_HOSTED_SDIO_PROBE_TRIES       4
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
  int attempt;
  int ret;

  if (g_hosted_state == ESP_HOSTED_SDIO_ENUMERATED ||
      g_hosted_state == ESP_HOSTED_SDIO_HANDSHAKE ||
      g_hosted_state == ESP_HOSTED_SDIO_UP)
    {
      return OK;
    }

  /* Before anything drives these lines, find out what the board itself is
   * doing with them. A line held low against its own pull-up is a wiring
   * problem, and no driver change can fix that.
   */

  esp_sdmmc_sample_idle();
  esp_sdmmc_identify_pins();

  /* Step 0a: put the pads in a defined state *before* the companion boots.
   *
   * The order matters. If CLK is still floating when the companion comes out
   * of reset, it can latch edges that were never driven. Routing first and
   * resetting second is what the working implementation for this board does.
   */

  esp_sdmmc_route_pins();

  /* Step 0b: bring the companion module out of reset.
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

      /* Reset the companion, then hold it enabled.
       *
       * Driving the enable line low holds the module in reset and releasing it
       * gives the module a defined start, so the host knows the state it is
       * talking to instead of inheriting whatever the module happened to be
       * doing before. This is what esp-hosted's own bring-up does.
       *
       * (An earlier round removed this pulse on the theory that a mid-boot
       * module was wedging the host controller. That theory was wrong -- the
       * -EBUSY came from an inverted START predicate in this driver -- so the
       * pulse is back, and it is also what lets the module's boot log be
       * captured from the host.)
       */

      esp_gpiowrite(CONFIG_ESPRESSIF_HOSTED_SDIO_EN_GPIO, false);
      nxsig_usleep(50000);

      /* Pulse the companion for this run so its boot log can be captured at the
       * same time as the host enumerates.
       */

      esp_gpiowrite(CONFIG_ESPRESSIF_HOSTED_SDIO_EN_GPIO, false);
      nxsig_usleep(50000);
      esp_gpiowrite(CONFIG_ESPRESSIF_HOSTED_SDIO_EN_GPIO, true);
      syslog(LOG_INFO,
             "%s: companion enable held high (GPIO%d), settling %d ms\n",
             HOSTED_TAG, CONFIG_ESPRESSIF_HOSTED_SDIO_EN_GPIO,
             CONFIG_ESPRESSIF_HOSTED_SDIO_EN_DELAY_MS);

      /* Wait out the companion's own boot.
       *
       * Its log shows the SDIO slave is not ready until roughly 960 ms after
       * release, so this is the one delay that genuinely cannot be shortened.
       * It comes from Kconfig rather than being hardcoded here, so a board with
       * a different companion can tune it without touching the driver.
       */

      nxsig_usleep(CONFIG_ESPRESSIF_HOSTED_SDIO_EN_DELAY_MS * 1000);
    }
#endif

  /* The reference-implementation cross-check that used to sit here has been
   * removed: it hard-coded the slot-1 pins and its conclusion (both
   * implementations fail identically) is now explained -- the companion is
   * wired to slot 0, so nothing was ever driven on the slot-1 pads.
   */

  /* Let the clock path issue the update_clk_reg command.
   *
   * It used to be skipped here because it never completed, and a controller
   * reset was used to commit CLKDIV/CLKSRC/CLKENA instead. But the register
   * description says those three are transferred into the card clock domain
   * only by a command carrying update_clk_reg, and Espressif's own path always
   * issues it -- so the reset was committing them on an assumption. The
   * analyser shows the result of that assumption: a CLK pad that never moves.
   *
   * The command is now built exactly as sd_host_slot_clock_update_command()
   * builds it (card_num, update_clk_reg, wait_complete, nothing else), and the
   * driver reports whether it completes.
   */

  esp_sdmmc_skip_clk_update(false);

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
  esp_sdmmc_report_routing();

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

  /* One concise snapshot of the controller and the clock tree.
   *
   * These are bring-up diagnostics rather than something a working system
   * needs, but on this board the failure mode is "every register reads back
   * correct and nothing happens", so having the values in the boot log is what
   * makes that visible without attaching a debugger.
   */

  {
    FAR struct esp_sdmmc_debug_s dbg;

    esp_sdmmc_debug(&dbg);
    syslog(LOG_INFO, "%s: verid=%08" PRIx32 " status=%08" PRIx32
           " cdetect=%08" PRIx32 " clkena=%08" PRIx32 "\n",
           HOSTED_TAG, dbg.verid, dbg.status, dbg.cdetect, dbg.clkena);
    syslog(LOG_INFO, "%s: ctrl=%08" PRIx32 " bmod=%08" PRIx32
           " rintsts=%08" PRIx32 " clkdiv=%08" PRIx32 "\n",
           HOSTED_TAG, dbg.ctrl, dbg.bmod, dbg.rintsts, dbg.clkdiv);

    /* CTRL.controller_reset is cleared by the CIU clock, so this one bit says
     * whether the controller is being clocked at all. A controller that
     * accepts every register write but sits with its reset bit set is not
     * "waiting for a card" -- it is not running.
     */

    syslog(LOG_INFO, "%s: CIU reset bit %s (bit0 of ctrl: %d)\n", HOSTED_TAG,
           (dbg.ctrl & 1u) ? "*** STILL SET -- CIU is not clocked ***"
                           : "clear -- CIU is running", (int)(dbg.ctrl & 1u));
    syslog(LOG_INFO, "%s: clkrst sys=%08" PRIx32 " peri01=%08" PRIx32
           " peri02=%08" PRIx32 " ref2=%08" PRIx32 "\n",
           HOSTED_TAG, dbg.sys_clk_ctrl1, dbg.peri_clk_ctrl01,
           dbg.peri_clk_ctrl02, dbg.ref_clk_ctrl2);
  }

  /* Ask repeatedly, for a bounded window.
   *
   * The companion boots its own RTOS and brings up its SDIO slave before it can
   * answer CMD5, so a single probe right after the enable delay measures how
   * fast it boots rather than whether the link works. There is deliberately no
   * controller reset between attempts: re-running the clock and reset sequence
   * is disruptive while the companion is still settling.
   */

  for (attempt = 1; attempt <= ESP_HOSTED_SDIO_PROBE_TRIES; attempt++)
    {
      /* Halfway through, reset the companion again -- this time with the card
       * clock already running.
       *
       * The companion is released from reset before esp_sdmmc_initialize()
       * starts the clock, so it boots with CLK idle. An SDIO slave that latches
       * its interface state at boot would then be waiting for something that
       * never comes, and no later command could revive it. Releasing it a second
       * time now that CLK is running is the cheapest way to test that.
       */

      if (attempt == 3 && CONFIG_ESPRESSIF_HOSTED_SDIO_EN_GPIO != -1)
        {
          syslog(LOG_INFO, "%s: re-releasing the companion with the clock "
                 "already running\n", HOSTED_TAG);

          esp_gpiowrite(CONFIG_ESPRESSIF_HOSTED_SDIO_EN_GPIO, false);
          nxsig_usleep(50000);
          esp_gpiowrite(CONFIG_ESPRESSIF_HOSTED_SDIO_EN_GPIO, true);
          nxsig_usleep(1500000);
        }

      ret = sdio_probe(sdio);
      if (ret >= 0)
        {
          break;
        }

      syslog(LOG_INFO, "%s: probe %d/%d -> %d, retrying\n",
             HOSTED_TAG, attempt, ESP_HOSTED_SDIO_PROBE_TRIES, ret);

      nxsig_usleep(ESP_HOSTED_SDIO_PROBE_INTERVAL_MS * 1000);
    }

  /* NuttX returns OK (zero) on success. Testing for a negative value instead
   * silently accepted a non-zero success-path return -- sdio_probe() came back
   * as 53 and was treated as "enumerated" even though it had bailed out before
   * CMD3/CMD7, leaving the card unselected and every later CMD52 unanswered.
   */

  if (ret != OK)
    {
      g_hosted_state   = ESP_HOSTED_SDIO_FAILED;
      g_hosted_errcode = ret;
      syslog(LOG_ERR, "%s: no SDIO response from the companion module: %d\n",
             HOSTED_TAG, ret);
      return ret;
    }

  g_hosted_state = ESP_HOSTED_SDIO_ENUMERATED;
  syslog(LOG_INFO, "%s: companion enumerated (sdio_probe returned %d)\n",
         HOSTED_TAG, ret);

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

  /* Stay in 1-bit mode.
   *
   * The reference implementation for this board runs the link in 1-bit mode
   * (D1/D2 unused on the host side) and reaches full Wi-Fi operation that way,
   * so 4-bit is not required and may itself be part of what is not coming up.
   */

  ret = -ENOSYS;
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

  /* Read a few of the companion's slave registers.
   *
   * Now that the card answers, this is the cheapest way to prove the register
   * channel really carries data in both directions: the slave keeps its
   * packet-length and token registers in SCRATCH space, and a read that comes
   * back with a plausible value (rather than all-ones or all-zeros) means CMD52
   * is exchanging real bytes with the companion's own logic, not just talking
   * to a card that acknowledges and forgets.
   *
   * Addresses are the low 17 bits of the slave's SLCHOST window (0x3FF55000).
   */

  {
    static const struct
    {
      const char *name;
      uint32_t    addr;
    } regs[] =
    {
      { "PACKET_LEN", 0x00060 },
      { "TOKEN_RDATA", 0x00044 },
      { "SCRATCH0", 0x0006c },
      { "SCRATCH7", 0x0008c },
    };

    int i;

    for (i = 0; i < (int)(sizeof(regs) / sizeof(regs[0])); i++)
      {
        uint8_t val = 0;
        int r = sdio_io_rw_direct(sdio, false,
                                  /* function 1 = the companion's data function */
                                  1, regs[i].addr, 0, &val);
        syslog(LOG_INFO, "%s: slave %-12s [%05" PRIx32 "] = %02x (ret %d)\n",
               HOSTED_TAG, regs[i].name, regs[i].addr, val, r);
      }

    /* Now drive one: SCRATCH 14 is not part of the packet protocol, so writing
     * it cannot confuse the companion, and reading the same value back proves
     * the host really does push bytes into the companion's register file rather
     * than a card that merely acknowledges writes.
     */

    {
      uint8_t written = 0x5a;
      uint8_t readback = 0;
      int w = sdio_io_rw_direct(sdio, true, 1, 0x000b4, written, NULL);
      int rb = sdio_io_rw_direct(sdio, false, 1, 0x000b4, 0, &readback);

      syslog(LOG_INFO,
             "%s: scratch14 loopback: wrote %02x (ret %d), read %02x (ret %d) %s\n",
             HOSTED_TAG, written, w, readback, rb,
             readback == written ? "-- MATCHES" : "-- differs");
    }

    /* Finally exercise CMD53: one 512-byte block read over function 1.
     *
     * This is the piece the frame layer will ride on, and it is the first test
     * of the driver's own data path -- the IDMAC descriptor chain, the block
     * setup and the transfer-complete interrupt handling. A read that comes
     * back with a clean status and a plausible first word says that path works.
     */

    {
      static uint8_t block[512] __attribute__((aligned(64)));
      int r;

      memset(block, 0, sizeof(block));

      r = sdio_io_rw_extended(sdio, false, 1, 0, true, block, 512, 1);

      syslog(LOG_INFO,
             "%s: CMD53 read 1x512 from FN1: ret %d, first word %08" PRIx32 "\n",
             HOSTED_TAG, r,
             ((uint32_t)block[0] << 24) | ((uint32_t)block[1] << 16) |
             ((uint32_t)block[2] << 8) | (uint32_t)block[3]);
    }

    /* Tell the companion the host is up.
     *
     * The slave will not queue anything until it sees this: esp-hosted's host
     * writes HOST_TO_SLAVE_INTR (SCRATCH 7) to hand its interrupt mask over,
     * and only then does the slave start raising NEW_PACKET. Without it
     * PACKET_LEN stays zero forever, which is what the earlier register dump
     * showed.
     */

    {
      uint8_t mask = 0x01;   /* BIT(ESP_OPEN_DATA_PATH) -- see adapter.h */
      int w = sdio_io_rw_direct(sdio, true, 1, 0x0008c, mask, NULL);
      uint8_t rb = 0;
      int rr = sdio_io_rw_direct(sdio, false, 1, 0x0008c, 0, &rb);

      syslog(LOG_INFO, "%s: host-ready mask %02x written (ret %d), readback %02x (ret %d)\n",
             HOSTED_TAG, mask, w, rb, rr);
    }

    /* Watch PACKET_LEN for a while after announcing readiness. A companion that
     * has events queued should raise it; a register that stays zero says it
     * either has nothing to say or is waiting for something else.
     */

    {
      int i;

      for (i = 0; i < 20; i++)
        {
          uint8_t len_lo = 0;
          uint8_t len_hi = 0;
          uint8_t len_x = 0;
          uint8_t st = 0;

          nxsig_usleep(300000);

          sdio_io_rw_direct(sdio, false, 1, 0x00060, 0, &len_lo);
          sdio_io_rw_direct(sdio, false, 1, 0x00061, 0, &len_hi);
          sdio_io_rw_direct(sdio, false, 1, 0x00062, 0, &len_x);
          sdio_io_rw_direct(sdio, false, 1, 0x00050, 0, &st);

          {
            uint8_t stst = 0;
            uint8_t tok[4];
            uint32_t token;

            sdio_io_rw_direct(sdio, false, 1, 0x00058, 0, &stst);
            sdio_io_rw_direct(sdio, false, 1, 0x00044, 0, &tok[0]);
            sdio_io_rw_direct(sdio, false, 1, 0x00045, 0, &tok[1]);
            sdio_io_rw_direct(sdio, false, 1, 0x00046, 0, &tok[2]);
            sdio_io_rw_direct(sdio, false, 1, 0x00047, 0, &tok[3]);
            token = (uint32_t)tok[0] | ((uint32_t)tok[1] << 8) |
                    ((uint32_t)tok[2] << 16) | ((uint32_t)tok[3] << 24);

            syslog(LOG_INFO,
                   "%s: t=%dms LEN=%02x %02x %02x INT_RAW=%02x INT_ST=%02x "
                   "TOKEN=%08" PRIx32 "\n",
                   HOSTED_TAG, (i + 1) * 100, len_lo, len_hi, len_x, st,
                   stst, token);
          }
        }

      /* Read the queued packet.
       *
       * esp-hosted hands the payload over at the END of the CMD53 window and
       * works downwards: address = ESP_SLAVE_CMD53_END_ADDR - length. The length
       * register's low 20 bits are the byte count; the upper bits carry flags.
       */

      {
        uint8_t  lenb[4] = { 0, 0, 0, 0 };
        uint32_t raw;
        uint32_t len;
        /* Must hold the whole advertised transfer (1536) plus the offset
         * from the window base to the payload, which is why it is larger
         * than one transfer.
         */
        static uint8_t payload[2048] __attribute__((aligned(64)));
        int r;

        sdio_io_rw_direct(sdio, false, 1, 0x00060, 0, &lenb[0]);
        sdio_io_rw_direct(sdio, false, 1, 0x00061, 0, &lenb[1]);
        sdio_io_rw_direct(sdio, false, 1, 0x00062, 0, &lenb[2]);
        sdio_io_rw_direct(sdio, false, 1, 0x00063, 0, &lenb[3]);

        raw = (uint32_t)lenb[0] | ((uint32_t)lenb[1] << 8) |
              ((uint32_t)lenb[2] << 16) | ((uint32_t)lenb[3] << 24);
        len = raw & 0xfffffu;

        syslog(LOG_INFO, "%s: packet queued: raw=%08" PRIx32 " len=%" PRIu32 "\n",
               HOSTED_TAG, raw, len);

        if (len > 0 && len <= sizeof(payload))
          {
            uint32_t blk = (len + 511u) & ~511u;
            uint32_t addr = 0x1f800u - len;

            /* Pre-fill with a pattern so the read's effect is visible.
             *
             * Zeros in the buffer are ambiguous: they mean either "the DMA
             * wrote zeros" or "the DMA never touched this memory". A buffer
             * that still holds the pattern afterwards proves the transfer went
             * somewhere else entirely.
             */

            {
              uint8_t t[4];
              uint32_t tk;

              sdio_io_rw_direct(sdio, false, 1, 0x00044, 0, &t[0]);
              sdio_io_rw_direct(sdio, false, 1, 0x00045, 0, &t[1]);
              sdio_io_rw_direct(sdio, false, 1, 0x00046, 0, &t[2]);
              sdio_io_rw_direct(sdio, false, 1, 0x00047, 0, &t[3]);
              tk = (uint32_t)t[0] | ((uint32_t)t[1] << 8) |
                   ((uint32_t)t[2] << 16) | ((uint32_t)t[3] << 24);
              syslog(LOG_INFO, "%s: TOKEN before read = %08" PRIx32
                     " (buffers %" PRIu32 ")\n", HOSTED_TAG, tk,
                     (tk >> 16) & 0xffffu);
            }

            memset(payload, 0xaa, sizeof(payload));

            /* Block-only transfer, matching the reference: the payload lives at
             * a fixed offset from the end of the window, so the read length is
             * rounded up and the companion pads the tail.
             */

            /* Read whole, aligned blocks covering the payload.
             *
             * A block-mode CMD53 addresses the function in units of its block
             * size, so the start has to be block aligned: reading from
             * 0x1F800-len directly returned zeros. Rounding down to the block
             * that contains the payload and taking the tail of it gives the
             * same bytes with an address the card can act on.
             */

            {
              uint32_t aligned = addr & ~511u;
              uint32_t skip    = addr - aligned;
              uint32_t nblk    = ((skip + len) + 511u) / 512u;

              /* The companion advertises a transfer size of 1536 bytes
               * (3 blocks) in its own log. Read the full advertised size from
               * the bottom of that window instead of a single block: if the
               * slave only starts its data phase once the host asks for a whole
               * advertised transfer, a 512-byte request would leave it silent
               * while still accepting the command -- which is what has been
               * happening.
               */

              if (nblk < 3u)
                {
                  nblk    = 3u;
                  aligned = 0x1f800u - 1536u;
                  skip    = addr - aligned;
                }

              memset(payload, 0, sizeof(payload));

              /* Byte mode, with the length rounded up to a whole block.
               *
               * The reference splits transfers at the block boundary and sends
               * anything shorter than a block as a byte-mode CMD53. 60 bytes is
               * short, so byte mode is what it would use here -- and unlike a
               * block transfer it accepts the unaligned address directly.
               */

              /* Byte mode, exactly the advertised length. The companion knows
               * how much it queued, so asking for exactly that (rather than a
               * padded whole block) is the closest match to what it expects.
               */

              r = sdio_io_rw_extended(sdio, false, 1, addr, true, payload,
                                      512u, 1);

              syslog(LOG_INFO,
                     "%s: RX ret %d, addr %05" PRIx32 " (512-byte block read, the address actually sent)\n",
                     HOSTED_TAG, r, addr);

              {
                uint8_t t[4];
                uint32_t tk;

                sdio_io_rw_direct(sdio, false, 1, 0x00044, 0, &t[0]);
                sdio_io_rw_direct(sdio, false, 1, 0x00045, 0, &t[1]);
                sdio_io_rw_direct(sdio, false, 1, 0x00046, 0, &t[2]);
                sdio_io_rw_direct(sdio, false, 1, 0x00047, 0, &t[3]);
                tk = (uint32_t)t[0] | ((uint32_t)t[1] << 8) |
                     ((uint32_t)t[2] << 16) | ((uint32_t)t[3] << 24);
                syslog(LOG_INFO, "%s: TOKEN after read  = %08" PRIx32
                       " (buffers %" PRIu32 ")\n", HOSTED_TAG, tk,
                       (tk >> 16) & 0xffffu);
              }

              if (r == 0)
                {
                  uint8_t *pl = &payload[skip];
                  char    hex[3 * 24 + 1];
                  int     k;

                  for (k = 0; k < 20 && (uint32_t)k < len; k++)
                    {
                      snprintf(&hex[k * 3], 4, "%02x ", pl[k]);
                    }

                  hex[3 * (k < 20 ? k : 20)] = '\0';

                  syslog(LOG_INFO, "%s: payload[%" PRIu32 "] = %s\n",
                         HOSTED_TAG, len, hex);
                }

              /* Did the read consume the packet? And is the data somewhere else
               * in the window? Scan a few block-aligned addresses and report any
               * that hold something other than zeros.
               */

              {
                uint8_t  pl2[4];
                uint32_t after = 0;
                uint32_t scan;

                sdio_io_rw_direct(sdio, false, 1, 0x00060, 0, &pl2[0]);
                sdio_io_rw_direct(sdio, false, 1, 0x00061, 0, &pl2[1]);
                sdio_io_rw_direct(sdio, false, 1, 0x00062, 0, &pl2[2]);
                sdio_io_rw_direct(sdio, false, 1, 0x00063, 0, &pl2[3]);
                after = (uint32_t)pl2[0] | ((uint32_t)pl2[1] << 8) |
                        ((uint32_t)pl2[2] << 16) | ((uint32_t)pl2[3] << 24);

                syslog(LOG_INFO, "%s: PACKET_LEN after read = %08" PRIx32 "\n",
                       HOSTED_TAG, after);

                /* Write a known pattern into the CMD53 window and read it back.
                 *
                 * This tests the data path and the address semantics on their
                 * own, with no dependence on the companion's packet protocol: if
                 * a block written to an address comes back from that same
                 * address, both the transfer and the addressing are sound and
                 * the empty receive must be the companion's doing. If it does
                 * not, the fault is here.
                 */

                {
                  static uint8_t out[512] __attribute__((aligned(64)));
                  static uint8_t in[512]  __attribute__((aligned(64)));
                  uint32_t target = 0x1f000u;
                  int rw;
                  int rr;
                  int k;
                  int match = 0;

                  for (k = 0; k < 512; k++)
                    {
                      out[k] = (uint8_t)(0xa5 ^ (k & 0xff));
                    }

                  memset(in, 0, sizeof(in));

                  rw = sdio_io_rw_extended(sdio, true, 1, target, true, out,
                                           512u, 1);
                  rr = sdio_io_rw_extended(sdio, false, 1, target, true, in,
                                           512u, 1);

                  for (k = 0; k < 512; k++)
                    {
                      if (in[k] == out[k])
                        {
                          match++;
                        }
                    }

                  syslog(LOG_INFO,
                         "%s: CMD53 loopback @%05" PRIx32 ": write ret %d, "
                         "read ret %d, matching bytes %d/512 "
                         "in[0..3]=%02x %02x %02x %02x\n",
                         HOSTED_TAG, target, rw, rr, match,
                         in[0], in[1], in[2], in[3]);
                }

                /* Find where the payload actually lives.
                 *
                 * Each read dequeues the packet, so ask the companion for a
                 * fresh one before every attempt: re-write the host-ready
                 * register, wait for PACKET_LEN to rise, then read that
                 * address. Sampling several offsets shows which region of the
                 * window carries data.
                 */

                for (scan = 0; scan < 8; scan++)
                  {
                    static uint8_t s2[512] __attribute__((aligned(64)));
                    uint32_t off = (uint32_t)scan * 512u;
                    uint32_t a   = 0x1f800u - 512u - off;
                    uint8_t  l4[4];
                    uint32_t rl;
                    uint8_t  ready = 0x03;
                    int k;
                    int nz = 0;
                    int pr;

                    sdio_io_rw_direct(sdio, true, 1, 0x0008c, ready, NULL);
                    nxsig_usleep(200000);

                    sdio_io_rw_direct(sdio, false, 1, 0x00060, 0, &l4[0]);
                    sdio_io_rw_direct(sdio, false, 1, 0x00061, 0, &l4[1]);
                    sdio_io_rw_direct(sdio, false, 1, 0x00062, 0, &l4[2]);
                    sdio_io_rw_direct(sdio, false, 1, 0x00063, 0, &l4[3]);
                    rl = (uint32_t)l4[0] | ((uint32_t)l4[1] << 8) |
                         ((uint32_t)l4[2] << 16) | ((uint32_t)l4[3] << 24);

                    memset(s2, 0xaa, sizeof(s2));

                    /* Read the whole block that ends at the window end. */

                    pr = sdio_io_rw_extended(sdio, false, 1, a, true, s2,
                                             512u, 1);

                    for (k = 0; k < 512; k++)
                      {
                        if (s2[k] != 0xaa)
                          {
                            nz++;
                          }
                      }

                    syslog(LOG_INFO,
                           "%s: scan addr %05" PRIx32 " len %06" PRIx32
                           ": ret %d, written %d, head %02x %02x %02x %02x %02x %02x\n",
                           HOSTED_TAG, a, rl, pr, nz,
                           s2[0], s2[1], s2[2], s2[3], s2[4], s2[5]);
                  }

                /* Try the transmit direction.
                 *
                 * The companion answers reads only while it has a packet queued,
                 * and it has exactly one to give, so the receive side cannot be
                 * probed further. Writes have no such precondition: the host
                 * offers a buffer, and the companion takes it when it is ready.
                 *
                 * The token register says how many transmit buffers the
                 * companion is offering (its upper half), so read that first and
                 * only write when it reports room.
                 */

                /* Configure the interrupt registers before touching the data
                 * path.
                 *
                 * The reference host clears the slave's interrupt latches and
                 * enables the host-side interrupt register as part of coming up;
                 * this driver never did either. The companion may well be holding
                 * its data path closed until it sees that configuration -- which
                 * would explain a queued packet that reads as zeros and a token
                 * register that offers no transmit buffers.
                 */

                {
                  uint8_t v;
                  int i;

                  for (i = 0; i < 4; i++)
                    {
                      sdio_io_rw_direct(sdio, true, 1, 0x000d4 + i, 0xff, NULL);
                    }

                  for (i = 0; i < 4; i++)
                    {
                      sdio_io_rw_direct(sdio, true, 1, 0x000dc + i, 0xff, NULL);
                    }

                  /* Also enable the slave's own interrupt output. */

                  for (i = 0; i < 4; i++)
                    {
                      sdio_io_rw_direct(sdio, true, 1, 0x00050 + i, 0x00, NULL);
                    }

                  v = 0;
                  sdio_io_rw_direct(sdio, false, 1, 0x000dc, 0, &v);
                  syslog(LOG_INFO, "%s: interrupts configured, HOST_INT_ENA now %02x\n",
                         HOSTED_TAG, v);
                }

                {
                  uint8_t  tok[4];
                  uint32_t token;
                  static uint8_t tx[512] __attribute__((aligned(64)));
                  int k;
                  int wr;

                  sdio_io_rw_direct(sdio, false, 1, 0x00044, 0, &tok[0]);
                  sdio_io_rw_direct(sdio, false, 1, 0x00045, 0, &tok[1]);
                  sdio_io_rw_direct(sdio, false, 1, 0x00046, 0, &tok[2]);
                  sdio_io_rw_direct(sdio, false, 1, 0x00047, 0, &tok[3]);
                  token = (uint32_t)tok[0] | ((uint32_t)tok[1] << 8) |
                          ((uint32_t)tok[2] << 16) | ((uint32_t)tok[3] << 24);

                  syslog(LOG_INFO, "%s: TX token reg = %08" PRIx32
                         " (buffers offered = %" PRIu32 ")\n",
                         HOSTED_TAG, token, (token >> 16) & 0xffu);

                  for (k = 0; k < 512; k++)
                    {
                      tx[k] = (uint8_t)(0x5a ^ (k & 0xff));
                    }

                  /* Reference semantics: the payload goes at
                   * ESP_SLAVE_CMD53_END_ADDR - length, and the host does no
                   * token write-back -- the token register is the companion's
                   * own count of buffers it is offering.
                   */

                  wr = sdio_io_rw_extended(sdio, true, 1, 0x1f800u - 512u, true,
                                           tx, 512u, 1);

                  {
                    uint8_t t2[4];
                    uint32_t tok2;

                    sdio_io_rw_direct(sdio, false, 1, 0x00044, 0, &t2[0]);
                    sdio_io_rw_direct(sdio, false, 1, 0x00045, 0, &t2[1]);
                    sdio_io_rw_direct(sdio, false, 1, 0x00046, 0, &t2[2]);
                    sdio_io_rw_direct(sdio, false, 1, 0x00047, 0, &t2[3]);
                    tok2 = (uint32_t)t2[0] | ((uint32_t)t2[1] << 8) |
                           ((uint32_t)t2[2] << 16) | ((uint32_t)t2[3] << 24);

                    syslog(LOG_INFO,
                           "%s: TX write 512 @1f600: ret %d; TOKEN after = %08" PRIx32
                           " (buffers %" PRIu32 ")\n",
                           HOSTED_TAG, wr, tok2, (tok2 >> 16) & 0xffffu);
                  }

                  /* Hand the buffer over by writing the token back, which is how
                   * the companion learns a frame is waiting.
                   */

                  {
                    uint8_t ack[4];
                    uint32_t newtok = (token & 0xffff0000u) | 0u;

                    ack[0] = (uint8_t)(newtok & 0xff);
                    ack[1] = (uint8_t)((newtok >> 8) & 0xff);
                    ack[2] = (uint8_t)((newtok >> 16) & 0xff);
                    ack[3] = (uint8_t)((newtok >> 24) & 0xff);

                    sdio_io_rw_direct(sdio, true, 1, 0x00044, ack[0], NULL);
                    sdio_io_rw_direct(sdio, true, 1, 0x00045, ack[1], NULL);
                    sdio_io_rw_direct(sdio, true, 1, 0x00046, ack[2], NULL);
                    sdio_io_rw_direct(sdio, true, 1, 0x00047, ack[3], NULL);

                    syslog(LOG_INFO, "%s: TX token ack written %08" PRIx32 "\n",
                           HOSTED_TAG, newtok);
                  }
                }

                (void)scan;
                syslog(LOG_INFO, "%s: window scan done\n", HOSTED_TAG);
              }
            }
          }
      }
    }
  }

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
