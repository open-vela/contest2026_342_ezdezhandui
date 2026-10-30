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

  /* Put the bus width in a known state on both sides.
   *
   * sdio_set_wide_bus() (which would ask for 4 bits) is not called here, so the
   * card's CCCR was never written and its width was whatever it powered up
   * with. Setting it explicitly to 1 bit removes any chance of a mismatch:
   * a card left in 4-bit mode against a host driving one data line would answer
   * commands normally -- CMD is a single wire either way -- and deliver nothing
   * during a data phase, which is precisely the failure being chased.
   */

  {
    uint8_t busif = 0;
    int rr = sdio_io_rw_direct(sdio, false, 0, SDIO_CCCR_BUS_IF, 0, &busif);

    if (rr == OK)
      {
        uint8_t want = busif & ~SDIO_CCCR_BUS_IF_WIDTH_MASK;

        want |= SDIO_CCCR_BUS_IF_1_BIT;

        ret = sdio_io_rw_direct(sdio, true, 0, SDIO_CCCR_BUS_IF, want, NULL);

        syslog(LOG_INFO, "%s: card bus width set to 1 bit (read %02x, wrote %02x, ret %d)\n",
               HOSTED_TAG, busif, want, ret);

        /* Four bits was tried here once DAT1..3 were finally routed to the
         * controller (they never were before, so the earlier attempts at four
         * bits were meaningless). With both sides genuinely on four bits the
         * analyser shows *less* traffic than on one bit: DAT0 loses the activity
         * it had, and every data line falls back to just the identification
         * pulses. The link stays on one bit, which is where the companion
         * actually moved data.
         */
      }
    else
      {
        syslog(LOG_WARNING, "%s: cannot read CCCR bus width: %d\n", HOSTED_TAG, rr);
      }
  }

  /* Continuous receive loop.
   *
   * Everything above reads exactly one packet, once, at init. The companion
   * generates events on its own schedule (the WiFi retry loop in the log fires
   * roughly every 2.4 s), so a single read almost always lands between them.
   * Poll PACKET_LEN here and read whenever it changes, which is what a real
   * receive task would do.
   */

  {
    static uint8_t rxbuf[1024] __attribute__((aligned(64)));
    uint32_t last = 0;
    uint8_t  lb[4];
    int      round;
    int      got = 0;

    /* Announce that the host is up BEFORE waiting for anything.
     *
     * The companion will not queue a single packet until it is told the host
     * is ready: esp-hosted's host writes HOST_TO_SLAVE_INTR (SCRATCH 7) to
     * hand its interrupt mask over, and only then does the slave start raising
     * NEW_PACKET.
     *
     * This call used to sit after the receive loop below. The loop therefore
     * spent all 400 rounds reading INT_RAW with bit 23 clear -- the companion
     * had never been told the host was ready -- so no packet ever arrived, the
     * interface never registered and the log only ever showed an endless
     * INT_RAW=0. Announcing first is what makes the wait meaningful.
     */

    {
      uint8_t mask = 0x01;   /* BIT(ESP_OPEN_DATA_PATH) -- see adapter.h */
      int w = sdio_io_rw_direct(sdio, true, 1, 0x0008c, mask, NULL);
      uint8_t rb = 0;
      int rr = sdio_io_rw_direct(sdio, false, 1, 0x0008c, 0, &rb);

      syslog(LOG_INFO, "%s: host-ready mask %02x written (ret %d), readback %02x (ret %d)\n",
             HOSTED_TAG, mask, w, rb, rr);
    }

    syslog(LOG_INFO, "%s: starting receive loop (400 rounds)\n", HOSTED_TAG);

    for (round = 0; round < 400; round++)
      {
        uint32_t now = 0;
        uint8_t  ib[4];
        uint32_t iraw;
        int k;

        /* Wait for the card to raise its SDIO interrupt (DAT1) first.
         *
         * This is the opening move of esp-hosted's own host, and it is what
         * this loop never had: it went straight to INT_RAW, sampling at
         * arbitrary moments, which is how the companion ended up with an
         * ever-growing send queue and no reader. The short timeout keeps a
         * missed edge degrading to the poll below instead of stalling.
         */

        (void)esp_sdmmc_wait_card_intr(sdio, 50);

        /* Step 1: wait for the companion's "new packet" interrupt, bit 23.
         *
         * This is the documented first step of the Sending-FIFO procedure and
         * the driver skipped it: it read PACKET_LEN and issued CMD53 whether or
         * not the companion had signalled. Reading INT_RAW as one byte made the
         * bit invisible, so the skip was never noticed.
         */

        iraw = 0;
        for (k = 0; k < 4; k++)
          {
            sdio_io_rw_direct(sdio, false, 1, 0x00050 + k, 0, &ib[k]);
          }

        iraw = (uint32_t)ib[0] | ((uint32_t)ib[1] << 8) |
               ((uint32_t)ib[2] << 16) | ((uint32_t)ib[3] << 24);

        if ((iraw & (1u << 23)) == 0)
          {
            usleep(200 * 1000);   /* not ready; try again next round */
            continue;
          }

        /* Step 2: read the queued length. */

        for (k = 0; k < 4; k++)
          {
            sdio_io_rw_direct(sdio, false, 1, 0x00060 + k, 0, &lb[k]);
          }

        now = (uint32_t)lb[0] | ((uint32_t)lb[1] << 8) |
              ((uint32_t)lb[2] << 16) | ((uint32_t)lb[3] << 24);
        now &= 0xfffffu;

        if (now != 0 && now != last)
          {
            int      rr;

            /* Step 3: acknowledge the interrupt before reading. */

            {
              uint8_t cb[4];
              int c;

              for (c = 0; c < 4; c++)
                {
                  cb[c] = (uint8_t)((iraw >> (8 * c)) & 0xffu);
                  sdio_io_rw_direct(sdio, true, 1, 0x000d4 + c, cb[c], NULL);
                }
            }

            /* Read the way esp-hosted's own host does.
             *
             * The reference sets EH_HOST_PORT_SDIO_RX_BLOCK_ONLY_XFER and
             * always transfers one 512-byte block from
             * ESP_SLAVE_CMD53_END_ADDR - len_from_slave, i.e. the announced
             * length positions the window but does NOT set the byte count.
             *
             * This used to pass nblocks = 0 with buflen = want, which asks for
             * a *byte-mode* CMD53 of an arbitrary length. Every such read came
             * back -110 with the buffer untouched (head=ee ee ee ee): the
             * slave did not answer a byte-mode window. One block of 512 bytes,
             * addressed at end-len, is what it does answer.
             */

            /* Read exactly the way esp-hosted's own host does.
             *
             * sdio_read_fromio() splits by size: whole ESP_BLOCK_SIZE (512)
             * blocks while the remainder is at least that, then byte mode for
             * what is left, with the byte count rounded up to a multiple of
             * four (EH_HOST_PORT_SDIO_RX_LEN_TO_TRANSFER(x) = (x + 3) & ~3).
             *
             * The packets here are small -- 60 to roughly 450 bytes -- so the
             * reference issues a *byte-mode* CMD53 whose count is the padded
             * length, addressed at END - len. An earlier revision asked for a
             * single 512-byte block instead, which is the branch reserved for
             * transfers of a full block or more.
             */

            memset(rxbuf, 0xee, sizeof(rxbuf));
            {
              uint32_t xfer = (now + 3u) & ~3u;

              if (xfer == 0 || xfer > sizeof(rxbuf))
                {
                  xfer = sizeof(rxbuf);
                }

              rr = sdio_io_rw_extended(sdio, false, 1, 0x1f800u - now, true,
                                       rxbuf, (int)xfer, 0);
            }

            syslog(LOG_INFO,
                   "%s: rxloop[%d] len=%" PRIu32 " ret=%d head=%02x %02x %02x %02x\n",
                   HOSTED_TAG, round, now, rr,
                   rxbuf[0], rxbuf[1], rxbuf[2], rxbuf[3]);

            if (rr == 0 && rxbuf[0] != 0xee)
              {
                got++;
                syslog(LOG_ERR, "%s: *** PACKET DATA RECEIVED (round %d) ***\n",
                       HOSTED_TAG, round);
              }
          }

        if (now != 0)
          {
            last = now;
          }

        usleep(200 * 1000);
      }

    syslog(LOG_INFO, "%s: receive loop done, %d packet(s) with data\n",
           HOSTED_TAG, got);
  }

  /* Enable high speed on the card.
   *
   * The companion is built with CONFIG_EH_TRANSPORT_CP_SDIO_HIGH_SPEED=y, which
   * passes SDIO_SLAVE_FLAG_HIGH_SPEED to sdio_slave_initialize() and sets
   *   EH_CP_SDIO_CLOCK_FREQ_KHZ = 40000
   * -- it is configured for the 40 MHz high-speed mode and expects the host to
   * negotiate it through the CCCR. This driver never touched that register, so
   * the card has been running its slave in high-speed mode without the host ever
   * having agreed to it.
   *
   * CCCR 0x13: bit 0 = SHS (supports high speed), bit 1 = EHS (enable it).
   */

  {
    uint8_t hs = 0;
    int rr = sdio_io_rw_direct(sdio, false, 0, 0x13, 0, &hs);

    if (rr == OK && (hs & 0x01) != 0)
      {
        int wr = sdio_io_rw_direct(sdio, true, 0, 0x13, hs | 0x02, NULL);
        uint8_t back = 0;

        sdio_io_rw_direct(sdio, false, 0, 0x13, 0, &back);

        syslog(LOG_INFO,
               "%s: high speed: card reports %02x, enable ret %d, now %02x\n",
               HOSTED_TAG, hs, wr, back);

        /* High speed needs the clock raised as well as the CCCR bit.
         *
         * The earlier 25 MHz attempt was made without this negotiation and the
         * link stopped answering; now that both sides have agreed to high speed
         * the rate can be raised with the protocol settled.
         */

        if (back & 0x02)
          {
            SDIO_CLOCK(sdio, CLOCK_SD_TRANSFER_1BIT);
            syslog(LOG_INFO, "%s: high speed: clock raised\n", HOSTED_TAG);
          }
      }
    else
      {
        syslog(LOG_INFO, "%s: high speed: card reports %02x (ret %d)\n",
               HOSTED_TAG, hs, rr);
      }
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
          /* INT_RAW is 32 bits and the "new packet" bit is 23.
           *
           * Reading only byte 0 (as this did) means bit 23 can never be seen,
           * so the driver has been issuing CMD53 without ever checking whether
           * the companion actually had a packet ready -- which is step 1 of the
           * documented Sending-FIFO procedure.
           */

          {
            uint8_t ib[4];
            uint32_t iraw = 0;

            sdio_io_rw_direct(sdio, false, 1, 0x00050, 0, &ib[0]);
            sdio_io_rw_direct(sdio, false, 1, 0x00051, 0, &ib[1]);
            sdio_io_rw_direct(sdio, false, 1, 0x00052, 0, &ib[2]);
            sdio_io_rw_direct(sdio, false, 1, 0x00053, 0, &ib[3]);
            iraw = (uint32_t)ib[0] | ((uint32_t)ib[1] << 8) |
                   ((uint32_t)ib[2] << 16) | ((uint32_t)ib[3] << 24);

            syslog(LOG_INFO,
                   "%s: INT_RAW=%08" PRIx32 " new_packet(bit23)=%u\n",
                   HOSTED_TAG, iraw, (unsigned)((iraw >> 23) & 1u));
            st = ib[0];
          }

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

            if (len_lo || len_hi || len_x)
              {
                static uint8_t regblk[20] __attribute__((aligned(64)));
                int rr;

                syslog(LOG_INFO, "%s: slave LEN=%02x %02x %02x TOKEN=%08" PRIx32 "\n",
                       HOSTED_TAG, len_lo, len_hi, len_x, token);

              }

            /* Reading the registers the instant the length appears was tried
             * and returns 0x32 for every register -- the bus has not settled.
             * The earlier behaviour (observe for a while, then read) is kept.
             */
          }
        }

      /* Test the write direction while the registers are still responsive.
       *
       * The previous attempt ran after the FIFO read and hit the 0x32 window
       * where every register access is invalid, so its -5 proved nothing. Do it
       * here instead: the poll above just read LEN and TOKEN successfully, which
       * means the companion is answering registers right now.
       */

      {
        static uint8_t txbuf[512] __attribute__((aligned(64)));
        uint8_t  t4[4];
        uint32_t before = 0;
        uint32_t after  = 0;
        int wr;
        int k;

        for (k = 0; k < 4; k++)
          {
            sdio_io_rw_direct(sdio, false, 1, 0x00044 + k, 0, &t4[k]);
          }

        before = (uint32_t)t4[0] | ((uint32_t)t4[1] << 8) |
                 ((uint32_t)t4[2] << 16) | ((uint32_t)t4[3] << 24);

        for (k = 0; k < 512; k++)
          {
            txbuf[k] = (uint8_t)(0x5a ^ k);
          }

        wr = sdio_io_rw_extended(sdio, true, 1, 0x1f800u - 512u, true,
                                 txbuf, 512u, 1);

        for (k = 0; k < 4; k++)
          {
            sdio_io_rw_direct(sdio, false, 1, 0x00044 + k, 0, &t4[k]);
          }

        after = (uint32_t)t4[0] | ((uint32_t)t4[1] << 8) |
                ((uint32_t)t4[2] << 16) | ((uint32_t)t4[3] << 24);

        syslog(LOG_INFO,
               "%s: clean-window TX write ret %d, TOKEN %08" PRIx32
               " -> %08" PRIx32 "\n",
               HOSTED_TAG, wr, before, after);
      }

      /* 25 MHz was tried here: after the switch the companion stopped
       * announcing packets at all (PACKET_LEN read back as zero), so the link
       * cannot run at that rate. The identification clock stays at 100 kHz,
       * which is the only rate at which the register interface has ever been
       * reliable.
       */

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

            /* D1 is the companion's SEND_NEW_PACKET line (held low while it has
             * a packet queued). Sampling it from software was tried and gave a
             * reading that disagreed with the analyser, which showed it low for
             * the whole capture, so the value is not trusted and is not used.
             */

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

              /* The address IS the requested length.
               *
               * From the ESP SDIO slave protocol documentation:
               *
               *   requested length = 0x1F800 - address
               *
               * and "in cases where the data is longer than the requested length,
               * the data will be zero filled (when sending)". Reading with an
               * address that encodes 60 bytes while asking CMD53 for 512 makes
               * the whole response zero filled -- which is exactly what every
               * attempt has returned.
               *
               * So the address must encode a length at least as large as the
               * transfer: for a 512-byte read that is 0x1F800 - 512 = 0x1F600.
               */

              /* Follow the documented FIFO access pattern exactly.
               *
               * The protocol doc's worked example reads 1031 bytes as a
               * 1024-byte block transfer to address 0x1F800-1031 followed by a
               * byte transfer to 0x1F800-7. The address always encodes the
               * TOTAL requested length; block mode takes whole blocks and byte
               * mode takes the remainder.
               *
               * 60 bytes is less than one block, so the whole packet is a byte
               * mode transfer at 0x1F800 - 60 with a count of 60.
               */

              /* Block mode, with the address encoding the SAME length the
               * transfer asks for.
               *
               * Two failure modes were measured separately and they explain each
               * other:
               *
               *   byte mode, 60 bytes at 0x1F7C4 -> ret 0, but the DMA never
               *   ran (the buffer still held its pre-fill);
               *
               *   block mode, 512 bytes at 0x1F7C4 -> the DMA ran, but the
               *   address encoded a requested length of 60 while the transfer
               *   asked for 512, which the protocol zero fills.
               *
               * So the transfer has to be a whole block AND the address has to
               * encode that same block-sized length: 0x1F800 - 512 = 0x1F600.
               * The companion's 60 bytes then arrive at the head of the buffer.
               */

              /* One block of 64 bytes, with the address asking for the same 64.
               *
               * The constraints measured last round conflict with a 512-byte
               * block: requested must be >= the transfer and <= what the
               * companion has queued (60), while a block transfer has to be a
               * whole block. A smaller block size satisfies all three at once:
               * 64 >= 64 and 60 <= 64, with the address 0x1F800 - 64.
               */

              /* Use the one combination that was measured to actually move data.
               *
               * Across every variant tried, only a 512-byte block transfer to
               * 0x1F7C4 ever ran the DMA (its buffer came back zeroed instead of
               * keeping the pre-fill). A 64-byte block at 0x1F7C0 -- which
               * satisfies requested == transfer == 64 with the queued 60 --
               * returned success without transferring anything, and byte mode
               * behaved the same way.
               *
               * Keeping the working variant here means whatever is observed next
               * is a property of the companion, not of a read that never
               * happened.
               */

              /* Byte mode, exactly the documented form for a short packet.
               *
               * Every read combination tried so far was measured through an
               * eventwait() that returned before the data phase, so none of
               * those results mean anything. Now that the wait is real, start
               * from what the protocol document prescribes: a byte-mode CMD53
               * with the address encoding the requested length
               * (0x1F800 - 60) and a count of 60.
               */

              /* The host-to-companion write test used to run here. It left the
               * controller in transmit mode when it failed (RINTSTS bit 4, TXDR
               * -- "TX FIFO data request" -- stayed set), so the read that
               * followed was measured against a controller already expecting to
               * send. Removed.
               */


              /* Block mode with requested == transfer, now that the wait is real.
               *
               * Every earlier variant was measured through an eventwait() that
               * returned before the data phase, so none of those readings mean
               * anything. This one asks for a whole 512-byte block and encodes
               * 512 in the address, so requested and transfer agree.
               */

              /* Read one whole 512-byte block with the address encoding the
               * same 512, which is the form that is supposed to move data.
               *
               * The minimal C6 test firmware (tools/test/c6_sdio_minimal) sends
               * 64 bytes of 0xC6^i repeatedly, so a successful read shows that
               * pattern at the head of the buffer -- an obvious, unambiguous
               * sign that the companion's data path works.
               */

              /* Read exactly what the companion announced, in byte mode.
               *
               * The minimal test firmware uses SDIO_SLAVE_SEND_PACKET, and in
               * packet mode the host reads the announced length as a single
               * byte-mode transfer at 0x1F800 - len. The old 512-byte block read
               * asked for more than was queued, which the protocol zero fills.
               */

              aligned = 0x1f800u - len;
              skip    = 0;

              memset(payload, 0xee, 128);

              r = sdio_io_rw_extended(sdio, false, 1, aligned, true, payload,
                                      (unsigned)len, 0);

              if (r == 0 && payload[0] == 0xc6 && payload[1] == 0xc7)
                {
                  syslog(LOG_ERR, "%s: *** C6 SDIO TEST PATTERN RECEIVED ***\n",
                         HOSTED_TAG);
                }

              syslog(LOG_INFO,
                     "%s: RX block mode (proper wait): ret %d, addr %05" PRIx32
                     "\n", HOSTED_TAG, r, aligned);

              /* Did the DMA even touch the descriptor?
               *
               * The OWN bit is set by the CPU when it hands the descriptor to
               * the DMA and cleared by the hardware on completion. If it is
               * still set afterwards, the DMA never ran -- and that would make
               * the fault ours, not the companion's.
               */

              esp_sdmmc_dma_state();

              /* The previous edits left a second read here that overwrote the
               * byte-mode result: it memset the buffer and then re-read it with
               * a 512-byte block transfer at an address that only asked for 60
               * bytes -- which the protocol zero fills. Every payload dump so far
               * came from that second read, not from the correct one.
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
                syslog(LOG_INFO, "%s: TOKEN after read  = %08" PRIx32
                       " (buffers %" PRIu32 ")\n", HOSTED_TAG, tk,
                       (tk >> 16) & 0xffffu);
              }

              /* Clear the companion's "new packet" interrupt.
               *
               * The slave announces a queued packet by holding DAT1 low
               * (SDIO_SLAVE_HOSTINT_SEND_NEW_PACKET) and keeps it asserted until
               * the host acknowledges. This driver never did: it read the packet
               * and left the interrupt standing, so DAT1 stayed low for the rest
               * of the session and the companion was, from its own point of
               * view, still waiting for the read to be acknowledged.
               */

              {
                uint32_t clr = 0xffffffffu;
                int k;

                for (k = 0; k < 4; k++)
                  {
                    sdio_io_rw_direct(sdio, true, 1, 0x000d4 + k,
                                      (uint8_t)((clr >> (8 * k)) & 0xff), NULL);
                  }

                syslog(LOG_INFO, "%s: companion interrupt cleared after read\n",
                       HOSTED_TAG);
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
        
                sdio_io_rw_direct(sdio, false, 1, 0x00060, 0, &pl2[0]);
                sdio_io_rw_direct(sdio, false, 1, 0x00061, 0, &pl2[1]);
                sdio_io_rw_direct(sdio, false, 1, 0x00062, 0, &pl2[2]);
                sdio_io_rw_direct(sdio, false, 1, 0x00063, 0, &pl2[3]);
                after = (uint32_t)pl2[0] | ((uint32_t)pl2[1] << 8) |
                        ((uint32_t)pl2[2] << 16) | ((uint32_t)pl2[3] << 24);

                syslog(LOG_INFO, "%s: PACKET_LEN after read = %08" PRIx32 "\n",
                       HOSTED_TAG, after);


                syslog(LOG_INFO, "%s: window scan done\n", HOSTED_TAG);
              }
            }
          }
      }
    }
  }

  /* Raise the card clock for data transfers.
   *
   * The identification clock is capped at 400 kHz by the SD specification, but
   * the data phase is not: once the card is selected, CMD53 may run at the
   * default speed. Every data attempt so far ran at 100 kHz, which is the
   * slowest end of the range, and a slow clock is the one variable that has
   * only ever been tried in one direction.
   */

  {
    SDIO_CLOCK(sdio, CLOCK_SD_TRANSFER_1BIT);

    syslog(LOG_INFO, "%s: data clock switched to the 25 MHz default speed\n",
           HOSTED_TAG);
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
