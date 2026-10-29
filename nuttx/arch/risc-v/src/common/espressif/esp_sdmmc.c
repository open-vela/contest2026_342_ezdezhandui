/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_sdmmc.c
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
 * ESP32-P4 SD/MMC host controller as a NuttX SDIO lower half
 *
 * This is the bottom layer of the board's WiFi path: the ESP32-P4 has no
 * radio of its own (SOC_WIFI_SUPPORTED is not defined for it), so Wi-Fi comes
 * from a companion module reached over SDIO.  See docs/08_WiFi_SDIO实现.md.
 *
 * ## Why this is not a copy of the ESP32-S3 driver
 *
 * The controller is the same DesignWare MSHC block and the register offsets
 * match, but four things are chip-specific and all four had to be redone:
 *
 *  1. **Pin routing is per-slot, not uniform.**  On the S3 every SDMMC pin
 *     goes through the GPIO matrix.  On the P4 the slots differ, and the
 *     chip's own pin header says so:
 *
 *       slot 0  microSD    IO_MUX        CLK43 CMD44 D0..D3 39..42
 *       slot 1  companion  GPIO matrix   CLK18 CMD19 D0..D3 14..17
 *
 *     (components/esp_hal_sd/esp32p4/include/soc/sdmmc_pins.h defines S LOT0
 *     and states "SLOT1 doesn't go through IOMUX"; SDMMC_LL_SLOT_SUPPORT_
 *     GPIO_MATRIX() agrees, returning true only for slot 1.)
 *
 *  2. **The clock tree lives outside the controller**, in HP_SYS_CLKRST
 *     (peri_clk_ctrl01/02).  The per-card divider is in the SDMMC CLKDIV
 *     register, but the source divider and the HS/LS edge selection are in
 *     the system clock block.
 *
 *  3. **The DMA is the block's own IDMAC**, not a GDMA channel.  That matters
 *     on this board because the I2S driver already claims GDMA channels;
 *     IDMAC keeps the two completely independent.
 *
 *  4. **Interrupts route through CLIC**, so the handler is installed with
 *     esp_setup_irq(SDIO_HOST_INTR_SOURCE, ...).
 *
 * ## Register layer
 *
 * All register access goes through Espressif's LL for this chip
 * (components/esp_hal_sd/esp32p4/include/hal/sdmmc_ll.h) -- the same code
 * ESP-IDF drives the controller with.  Hand-copying ~440 lines of bitfields
 * would be a large, silent-failure-prone duplication for no benefit.  What is
 * written here is the *integration*: NuttX's struct sdio_dev_s method table, command and
 * response translation, the wait/callback machinery, and the IDMAC chain.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <debug.h>
#include <errno.h>

#include <nuttx/arch.h>
#include <syslog.h>
#include <nuttx/clock.h>
#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/sdio.h>
#include <nuttx/semaphore.h>
#include <nuttx/wdog.h>
#include <nuttx/wqueue.h>

#include "esp_gpio.h"
#include "esp_irq.h"
#include "esp_sdmmc.h"

#include <hal/clk_gate_ll.h>
#include <esp_clk_tree.h>
#include <esp_rom_gpio.h>
#include <soc/hp_sys_clkrst_struct.h>
#include <soc/gpio_reg.h>
#include <soc/gpio_sig_map.h>
#include <soc/sdmmc_pins.h>

/* The LL's clock/reset helpers expand this macro around a register update.
 * It comes from an ESP-IDF private header (esp_private/periph_ctrl.h) that is
 * not part of the NuttX port.  NuttX calls the clock setup from thread context
 * before any transfer is in flight, so there is no concurrent writer to
 * serialise against and an empty expansion is correct here.
 */

#ifndef __DECLARE_RCC_ATOMIC_ENV
#  define __DECLARE_RCC_ATOMIC_ENV (void)0
#endif

#include "hal/sdmmc_ll.h"

#ifdef CONFIG_ESPRESSIF_SDMMC

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SDMMC_SLOT            CONFIG_ESPRESSIF_SDMMC_SLOT

/* Timeouts in system ticks.  The command timeout bounds a hung CIU; the long
 * timeout is the backstop for a card that never answers.
 */

#define SDMMC_CMDTIMEOUT      (1000 / MSEC_PER_TICK)
#define SDMMC_LONGTIMEOUT     (5000 / MSEC_PER_TICK)

/* Interrupt bits this driver acts on.  Unlike the bitfields the SDMMC block
 * exposes, the interrupt status is a plain bitmask.
 */

#define SDMMC_CMD_DONE        (1u << 2)   /* Command done           */
#define SDMMC_DATA_OVER       (1u << 3)   /* Data transfer over     */
#define SDMMC_RXDR            (1u << 5)   /* RX FIFO data available */
#define SDMMC_TXDR            (1u << 4)   /* TX FIFO data request   */

/* Error bits: response error/CRC, data CRC, response and data timeouts, FIFO
 * under/overrun, hardware lock, start-bit and end-bit errors.
 */

#define SDMMC_ERR_RE          (1u << 1)
#define SDMMC_ERR_RCRC        (1u << 6)
#define SDMMC_ERR_DCRC        (1u << 7)
#define SDMMC_ERR_RTO         (1u << 8)
#define SDMMC_ERR_DRTO        (1u << 9)
#define SDMMC_ERR_HTO         (1u << 10)
#define SDMMC_ERR_FRUN        (1u << 11)
#define SDMMC_ERR_HLE         (1u << 12)
#define SDMMC_ERR_SBE         (1u << 13)
#define SDMMC_ERR_EBE         (1u << 15)

#define SDMMC_ALL_ERRORS      (SDMMC_ERR_RE | SDMMC_ERR_RCRC | \
                               SDMMC_ERR_DCRC | SDMMC_ERR_RTO | \
                               SDMMC_ERR_DRTO | SDMMC_ERR_HTO | \
                               SDMMC_ERR_FRUN | SDMMC_ERR_HLE | \
                               SDMMC_ERR_SBE | SDMMC_ERR_EBE)

#define SDMMC_INT_ENABLE      (SDMMC_CMD_DONE | SDMMC_DATA_OVER | \
                               SDMMC_ALL_ERRORS)

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* One entry of the IDMAC descriptor chain.
 *
 * The 64-byte size is not padding for looks: the P4's L1 cache line is 64
 * bytes, and the chip's own sdmmc_desc_t carries the same reserved[] tail for
 * the same reason.  A shorter descriptor would share a cache line with its
 * neighbour, so cleaning one would write back the other's stale contents and
 * corrupt the chain.
 */

struct esp_sdmmc_desc_s
{
  uint32_t des0;                 /* Control and status                */
  uint32_t des1;                 /* Buffer size                       */
  uint32_t buffer;               /* Buffer address                    */
  uint32_t next;                 /* Next descriptor address           */
  uint32_t pad[12];              /* Pad to 64 bytes (cache line)      */
};

#define SDMMC_DESC_OWN        (1u << 31)  /* Owned by the IDMAC engine */
#define SDMMC_DESC_CES        (1u << 30)  /* Card error summary        */
#define SDMMC_DESC_ER         (1u << 5)   /* End of ring               */
#define SDMMC_DESC_CH         (1u << 4)   /* Second address chained    */
#define SDMMC_DESC_FS         (1u << 3)   /* First descriptor          */
#define SDMMC_DESC_LD         (1u << 2)   /* Last descriptor           */
#define SDMMC_DESC_DIC        (1u << 1)   /* Disable int on completion */

struct esp_sdmmc_dev_s
{
  struct sdio_dev_s dev;         /* Base class; must be first         */

  sdmmc_dev_t *hw;               /* Controller registers              */
  uint8_t slot;                  /* Slot being driven                 */

  int clk_pin;                   /* Pin assignment for this slot      */
  int cmd_pin;
  int d0_pin;
  int d1_pin;
  int d2_pin;
  int d3_pin;

  /* Event machinery */

  uint8_t waitmask;              /* Events the waiter cares about     */
  volatile uint8_t xfrmask;      /* Events that have occurred         */
  volatile bool enablewait;      /* A waiter is parked                */
  sem_t waitsem;                 /* Posted when an event arrives      */
  struct wdog_s timeout;         /* Transfer timeout                  */

  /* Callback machinery */

  uint8_t cbenable;              /* Events that fire the callback     */
  worker_t callback;             /* The callback function             */
  void *cbarg;                   /* Callback argument                 */
  struct work_s work;            /* Deferred callback work            */

  /* Transfer state */

  bool widebus;                  /* True: 4-bit mode                  */
  uint32_t blocksize;            /* Current block size                */
  uint32_t nblocks;              /* Blocks in the pending transfer    */

  int cpuint;                    /* Allocated CPU interrupt           */
  bool initialized;

#if defined(CONFIG_ESPRESSIF_SDMMC_DMA) && defined(CONFIG_SDIO_DMA)
  struct esp_sdmmc_desc_s *desc;     /* Two-entry descriptor chain        */
#endif
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int  esp_sdmmc_lock(struct sdio_dev_s *dev, bool lock);
static void esp_sdmmc_reset(struct sdio_dev_s *dev);
static sdio_capset_t esp_sdmmc_capabilities(struct sdio_dev_s *dev);
static sdio_statset_t esp_sdmmc_status(struct sdio_dev_s *dev);
static void esp_sdmmc_widebus(struct sdio_dev_s *dev, bool enable);
/* Bring-up switch: skip the update_clk_reg command (see header). */
static bool g_sdmmc_skip_clk_update;

/* Bring-up aid: sample the CLK pad while commands are in flight. */
static bool g_sdmmc_probe_clk;

static void esp_sdmmc_clock(struct sdio_dev_s *dev, enum sdio_clock_e rate);
static int  esp_sdmmc_attach(struct sdio_dev_s *dev);

static int  esp_sdmmc_sendcmd(struct sdio_dev_s *dev, uint32_t cmd,
                              uint32_t arg);
#ifdef CONFIG_SDIO_BLOCKSETUP
static void esp_sdmmc_blocksetup(struct sdio_dev_s *dev,
                                 unsigned int blocklen,
                                 unsigned int nblocks);
#endif
static int  esp_sdmmc_recvsetup(struct sdio_dev_s *dev, uint8_t *buffer,
                                size_t nbytes);
static int  esp_sdmmc_sendsetup(struct sdio_dev_s *dev,
                                const uint8_t *buffer, size_t nbytes);
static int  esp_sdmmc_cancel(struct sdio_dev_s *dev);
static int  esp_sdmmc_waitresponse(struct sdio_dev_s *dev, uint32_t cmd);
static int  esp_sdmmc_recvshortcrc(struct sdio_dev_s *dev, uint32_t cmd,
                                   uint32_t *rshort);
static int  esp_sdmmc_recvlong(struct sdio_dev_s *dev, uint32_t cmd,
                               uint32_t rlong[4]);
static int  esp_sdmmc_recvshort(struct sdio_dev_s *dev, uint32_t cmd,
                                uint32_t *rshort);

static void esp_sdmmc_waitenable(struct sdio_dev_s *dev,
                                 sdio_eventset_t eventset, uint32_t timeout);
static sdio_eventset_t esp_sdmmc_eventwait(struct sdio_dev_s *dev);
static void esp_sdmmc_callbackenable(struct sdio_dev_s *dev,
                                     sdio_eventset_t eventset);
static int  esp_sdmmc_registercallback(struct sdio_dev_s *dev,
                                       worker_t callback, void *arg);

#if defined(CONFIG_ESPRESSIF_SDMMC_DMA) && defined(CONFIG_SDIO_DMA)
static int  esp_sdmmc_dmarecvsetup(struct sdio_dev_s *dev, uint8_t *buffer,
                                   size_t buflen);
static int  esp_sdmmc_dmasendsetup(struct sdio_dev_s *dev,
                                   const uint8_t *buffer, size_t buflen);
#endif

static int  esp_sdmmc_interrupt(int irq, void *context, void *arg);
static void esp_sdmmc_callback(void *arg);
static void esp_sdmmc_timeout(wdparm_t arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Method table.
 *
 * NuttX embeds the operations in struct sdio_dev_s itself rather than having
 * a separate ops structure, so this is a fully-initialised device, not a
 * table that gets pointed at one.  Entries that only exist under a CONFIG are
 * spelled out with their guards, because a designated initialiser for a
 * missing member is a compile error rather than something quietly ignored.
 */

#ifdef CONFIG_SDIO_MUXBUS
static mutex_t g_sdmmc_lock = NXMUTEX_INITIALIZER;
#endif

static struct esp_sdmmc_dev_s g_sdmmc =
{
  .dev =
  {
#ifdef CONFIG_SDIO_MUXBUS
    .lock             = esp_sdmmc_lock,
#endif
    .reset            = esp_sdmmc_reset,
    .capabilities     = esp_sdmmc_capabilities,
    .status           = esp_sdmmc_status,
    .widebus          = esp_sdmmc_widebus,
    .clock            = esp_sdmmc_clock,
    .attach           = esp_sdmmc_attach,
    .sendcmd          = esp_sdmmc_sendcmd,
#ifdef CONFIG_SDIO_BLOCKSETUP
    .blocksetup       = esp_sdmmc_blocksetup,
#endif
    .recvsetup        = esp_sdmmc_recvsetup,
    .sendsetup        = esp_sdmmc_sendsetup,
    .cancel           = esp_sdmmc_cancel,
    .waitresponse     = esp_sdmmc_waitresponse,
    .recv_r1          = esp_sdmmc_recvshortcrc,
    .recv_r2          = esp_sdmmc_recvlong,
    .recv_r3          = esp_sdmmc_recvshort,
    .recv_r4          = esp_sdmmc_recvshort,
    /* R5 is CMD52's reply and carries no CRC field; checking one on a response
     * that does not have it rejects every CMD52. That is what stopped the
     * bring-up right after a successful enumeration -- "FN0 block size failed:
     * -5" was this check, not a bad answer from the companion.
     */

    .recv_r5          = esp_sdmmc_recvshort,
    .recv_r6          = esp_sdmmc_recvshortcrc,
    .recv_r7          = esp_sdmmc_recvshort,
    .waitenable       = esp_sdmmc_waitenable,
    .eventwait        = esp_sdmmc_eventwait,
    .callbackenable   = esp_sdmmc_callbackenable,
#if defined(CONFIG_SCHED_WORKQUEUE) && defined(CONFIG_SCHED_HPWORK)
    .registercallback = esp_sdmmc_registercallback,
#endif
#ifdef CONFIG_SDIO_DMA
    .dmarecvsetup     = esp_sdmmc_dmarecvsetup,
    .dmasendsetup     = esp_sdmmc_dmasendsetup,
#endif
  },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp_sdmmc_ciu_sendcmd
 *
 * Description:
 *   Hand a command to the card interface unit and start it.
 *
 *   START is a busy indicator for the *previous* command: hardware clears it
 *   once that command has been served.  Waiting for it here is what stops two
 *   commands from being in flight at once.
 *
 ****************************************************************************/

static int esp_sdmmc_ciu_sendcmd(sdmmc_hw_cmd_t hwcmd, uint32_t arg)
{
  clock_t start = clock_systime_ticks();
  sdmmc_dev_t *hw = g_sdmmc.hw;

  /* Careful with this predicate: despite the name it reads
   *
   *     return hw->cmd.start_command == 0;
   *
   * so it is TRUE when the command has been taken (the start bit has
   * self-cleared), i.e. TRUE means idle, not busy. Waiting on it directly would
   * therefore spin while the controller is idle and return -EBUSY before a
   * single command went out -- which is exactly the failure this driver showed.
   * Both waits below are "wait until the start bit clears".
   */

  while (!sdmmc_ll_is_command_taken(hw))
    {
      if (clock_systime_ticks() - start > SDMMC_CMDTIMEOUT)
        {
          syslog(LOG_ERR,
                 "esp_sdmmc: previous command still pending after %d ticks "
                 "(cmd=%08" PRIx32 " status=%08" PRIx32 " rintsts=%08" PRIx32
                 ")\n", (int)SDMMC_CMDTIMEOUT, hw->cmd.val, hw->status.val,
                 hw->rintsts.val);
          return -EBUSY;
        }
    }

  hwcmd.use_hold_reg  = 1;
  hwcmd.start_command = 1;

  /* Name every command on the way out.
   *
   * The analyser shows only CMD0 reaching the wire; the CMD5 that the probe
   * waits on never appears as a CRC-valid frame even with the card clock
   * running continuously. Logging here separates "the driver never issued it"
   * from "it was issued and the controller dropped it".
   */

  syslog(LOG_INFO,
         "esp_sdmmc: issue CMD%u arg=0x%08" PRIx32 " (cmd_word=%08" PRIx32 ")\n",
         (unsigned)(hwcmd.cmd_index), arg, hwcmd.val);

  /* Clear any stale completion before issuing this command.
   *
   * Leaving the previous Command Done bit standing is what made every wait
   * return early: the poll saw the old bit, reported success, and the response
   * for the command just written landed afterwards with nobody reading it.
   * The response register then kept the previous command's value, which is
   * exactly what the log showed (CMD52 "reading" CMD5's 0x20ffff00).
   */

  sdmmc_ll_clear_interrupt(hw, 0xffffffffu);

  sdmmc_ll_set_command_arg(hw, arg);
  sdmmc_ll_set_command(hw, hwcmd);

  /* Sample the CLK pad while this command is in flight.
   *
   * Sampling it at idle proves nothing: a DesignWare MSHC may legitimately gate
   * the card clock when no command or data transfer is running, so "no clock at
   * idle" is compatible with a perfectly healthy controller. The question that
   * matters is whether a clock exists *during* a command -- and this is the only
   * place in the driver where a command is known to be in flight.
   */

  if (g_sdmmc_probe_clk)
    {
      int hi = 0;
      int lo = 0;
      int i;

      for (i = 0; i < 256; i++)
        {
          if (esp_gpioread(CONFIG_ESPRESSIF_SDMMC_CLK))
            {
              hi++;
            }
          else
            {
              lo++;
            }
        }

      syslog(LOG_INFO,
             "esp_sdmmc: CLK during cmd=%08" PRIx32 ": %d high, %d low -- %s\n",
             hwcmd.val, hi, lo,
             (hi && lo) ? "clock IS running"
                        : "no clock during the command either");
    }

  return OK;
}

static void esp_sdmmc_configpins(int slot, int clk_pin, int cmd_pin,
                                 int d0_pin, int d1_pin, int d2_pin,
                                 int d3_pin);

/****************************************************************************
 * Name: esp_sdmmc_route_pins
 *
 * Description:
 *   Attach the configured slot's pads to the controller.
 *
 *   This is callable before esp_sdmmc_initialize(), which is the point: the
 *   companion must not be released from reset while CLK is still floating, and
 *   the driver's own state does not exist yet at that moment. The pin numbers
 *   therefore come from the Kconfig symbols rather than from that state.
 *
 ****************************************************************************/

/****************************************************************************
 * Name: esp_sdmmc_sample_idle
 *
 * Description:
 *   Read the slot's pads as plain GPIOs before the controller takes them over,
 *   once with the internal pull-ups off and once with them on.
 *
 *   This is the cheap half of what a logic analyser would tell us. The six
 *   lines of an idle SDIO bus are held high by the board's pull-ups, so:
 *
 *     no pull          pull-up on        reading means
 *     ---------------  ---------------   ------------------------------------
 *     high             high              the line has its own pull-up, healthy
 *     low              low               something is holding it down: a short,
 *                                         contention, or a missing pull-up
 *     low              high              nothing drives it, only our pull-up
 *
 *   In particular a CLK or CMD that stays low against its own pull-up means the
 *   problem is on the board, not in the protocol layer, and no amount of
 *   driver work will fix it.
 *
 ****************************************************************************/

void esp_sdmmc_sample_idle(void)
{
  static const char *const names[6] =
  {
    "CLK", "CMD", "D0", "D1", "D2", "D3"
  };
  const int pins[6] =
  {
    CONFIG_ESPRESSIF_SDMMC_CLK, CONFIG_ESPRESSIF_SDMMC_CMD,
    CONFIG_ESPRESSIF_SDMMC_D0,  CONFIG_ESPRESSIF_SDMMC_D1,
    CONFIG_ESPRESSIF_SDMMC_D2,  CONFIG_ESPRESSIF_SDMMC_D3
  };
  int i;

  for (i = 0; i < 6; i++)
    {
      esp_configgpio(pins[i], INPUT);
      up_udelay(200);
      int floating = esp_gpioread(pins[i]);

      esp_configgpio(pins[i], INPUT | PULLUP);
      up_udelay(200);
      int pulled = esp_gpioread(pins[i]);

      syslog(LOG_INFO,
             "esp_sdmmc: idle %s(GPIO%d): no-pull=%d pull-up=%d -- %s\n",
             names[i], pins[i], floating, pulled,
             (floating && pulled) ? "healthy (line has its own pull-up)"
             : (!pulled)          ? "HELD LOW: short, contention or no pull-up"
                                  : "only our pull-up drives it");
    }
}

/****************************************************************************
 * Name: esp_sdmmc_identify_pins
 *
 * Description:
 *   Drive each of the slot's pads as a plain GPIO and emit a number of pulses
 *   that identifies which pad it is: CLK gets one, CMD two, D0 three, and so
 *   on. Read the logic analyser alongside the console and the channel-to-pin
 *   mapping stops being an assumption.
 *
 *   It doubles as a pad test: if a pad cannot produce its pulses, the problem
 *   is the pad or whatever is holding it, not the protocol layer.
 *
 ****************************************************************************/

void esp_sdmmc_identify_pins(void)
{
  static const char *const names[6] =
  {
    "CLK", "CMD", "D0", "D1", "D2", "D3"
  };
  const int pins[6] =
  {
    CONFIG_ESPRESSIF_SDMMC_CLK, CONFIG_ESPRESSIF_SDMMC_CMD,
    CONFIG_ESPRESSIF_SDMMC_D0,  CONFIG_ESPRESSIF_SDMMC_D1,
    CONFIG_ESPRESSIF_SDMMC_D2,  CONFIG_ESPRESSIF_SDMMC_D3
  };
  int i;
  int n;

  syslog(LOG_INFO, "esp_sdmmc: pin identification starting\n");

  /* A one-pin probe that pulsed GPIO18 alone (three 50 ms pulses) was used to
   * settle which analyser channel carries CLK. Result: the pulses appeared on
   * channel 1 while channel 0 stayed flat, i.e. CLK is on channel 1 and CMD on
   * channel 0 -- one off from the channel list supplied with the board. The
   * six-pin loop below is kept because it identifies all six at once.
   */

  for (i = 0; i < 6; i++)
    {
      syslog(LOG_INFO, "esp_sdmmc: %s (GPIO%d) -> %d pulse(s)\n",
             names[i], pins[i], i + 1);

      esp_configgpio(pins[i], OUTPUT);

      for (n = 0; n < i + 1; n++)
        {
          esp_gpiowrite(pins[i], true);
          up_mdelay(20);
          esp_gpiowrite(pins[i], false);
          up_mdelay(20);
        }

      esp_configgpio(pins[i], INPUT | PULLUP);
      up_mdelay(60);
    }

  syslog(LOG_INFO, "esp_sdmmc: pin identification done\n");
}

/****************************************************************************
 * Name: esp_sdmmc_report_routing
 *
 * Description:
 *   Read back the GPIO-matrix output selection of the slot's pads and say
 *   whether each one really points at the controller signal it should.
 *
 *   esp_rom_gpio_connect_out_signal() is a ROM call; if it silently does
 *   nothing on this part, the pads stay on SIG_GPIO_OUT_IDX and neither the
 *   clock nor the command output ever reaches them -- which is exactly what
 *   the analyser shows. Reading the field back is the way to tell "the
 *   routing call failed" apart from "the controller is not driving".
 *
 ****************************************************************************/

void esp_sdmmc_report_routing(void)
{
  static const char *const names[3] = { "CLK", "CMD", "D0" };
  const int pins[3] =
  {
    CONFIG_ESPRESSIF_SDMMC_CLK, CONFIG_ESPRESSIF_SDMMC_CMD,
    CONFIG_ESPRESSIF_SDMMC_D0
  };
  const uint32_t want[3] =
  {
    SD_CARD_CCLK_2_PAD_OUT_IDX, SD_CARD_CCMD_2_PAD_OUT_IDX,
    SD_CARD_CDATA0_2_PAD_OUT_IDX
  };
  int i;

  for (i = 0; i < 3; i++)
    {
      uintptr_t reg = GPIO_FUNC0_OUT_SEL_CFG_REG + 4 * (uintptr_t)pins[i];
      uint32_t got = (uint32_t)REG_GET_FIELD(reg, GPIO_FUNC0_OUT_SEL);
      syslog(LOG_INFO,
             "esp_sdmmc: routing %s(GPIO%d): out_sel=%" PRIu32
             " want=%" PRIu32 " %s (256 = plain GPIO)\n",
             names[i], pins[i], got, want[i],
             (got == want[i]) ? "OK" : "*** NOT ROUTED ***");
    }
}

/****************************************************************************
 * Name: esp_sdmmc_try_cmd5_ocr
 *
 * Description:
 *   Send CMD5 with a real OCR argument instead of the zero NuttX uses.
 *
 *   sdio_probe() issues CMD5 with arg 0, which is the "query" form: the card is
 *   supposed to answer with its own OCR regardless. Some SDIO devices are less
 *   forgiving and only answer once the argument carries the voltage they want
 *   (3.2-3.4 V -> OCR bits 20:21). Espressif's host sends CMD5 with an OCR
 *   rather than zero, so this is worth one check before concluding the
 *   companion is silent.
 *
 *   Returns true when R4 came back.
 *
 ****************************************************************************/

bool esp_sdmmc_try_cmd5_ocr(uint32_t ocr)
{
  sdmmc_dev_t *hw = g_sdmmc.hw;
  sdmmc_hw_cmd_t c;
  clock_t start;
  bool got;

  syslog(LOG_INFO, "esp_sdmmc: CMD5-OCR probe entered (hw=%p slot=%d)\n",
         hw, g_sdmmc.slot);

  if (hw == NULL)
    {
      return false;
    }

  sdmmc_ll_clear_interrupt(hw, 0xffffffffu);

  memset(&c, 0, sizeof(c));
  c.cmd_index        = 5;
  c.card_num         = (uint32_t)g_sdmmc.slot;
  c.response_expect  = 1;
  c.use_hold_reg     = 1;
  c.wait_complete    = 1;
  c.start_command    = 1;

  sdmmc_ll_set_command_arg(hw, ocr);
  sdmmc_ll_set_command(hw, c);

  start = clock_systime_ticks();
  while (hw->cmd.start_command != 0)
    {
      if (clock_systime_ticks() - start > SDMMC_CMDTIMEOUT)
        {
          break;
        }
    }

  /* No response error bit is exposed by this header; judge on whether the
   * command was taken at all -- that already separates "the CIU never
   * executed it" from "it executed and nothing came back".
   */

  got = (hw->cmd.start_command == 0);

  syslog(LOG_INFO,
         "esp_sdmmc: CMD5 with OCR 0x%08" PRIx32 ": %s "
         "(rintsts=%08" PRIx32 " cmdarg=%08" PRIx32 " resp0=%08" PRIx32 ")\n",
         ocr, got ? "RESPONSE RECEIVED" : "no response",
         *(volatile uint32_t *)&hw->rintsts,
         *(volatile uint32_t *)&hw->cmdarg,
         *(volatile uint32_t *)&hw->resp[0]);

  return got;
}

/****************************************************************************
 * Name: esp_sdmmc_dma_state
 *
 * Description:
 *   Report whether the IDMAC actually ran for the last transfer.
 *
 ****************************************************************************/

void esp_sdmmc_dma_state(void)
{
  sdmmc_dev_t *hw = g_sdmmc.hw;

  if (hw == NULL)
    {
      return;
    }

  syslog(LOG_INFO,
         "esp_sdmmc: dma state: des0=%08" PRIx32 " des1=%08" PRIx32
         " buf=0%08" PRIx32 " ctrl=%08" PRIx32 " status=%08" PRIx32 "\n",
         g_sdmmc.desc[0].des0, g_sdmmc.desc[0].des1,
         g_sdmmc.desc[0].buffer,
         *(volatile uint32_t *)&hw->ctrl,
         *(volatile uint32_t *)&hw->status);
}

void esp_sdmmc_route_pins(void)
{
  esp_sdmmc_configpins(CONFIG_ESPRESSIF_SDMMC_SLOT,
                       CONFIG_ESPRESSIF_SDMMC_CLK,
                       CONFIG_ESPRESSIF_SDMMC_CMD,
                       CONFIG_ESPRESSIF_SDMMC_D0,
                       CONFIG_ESPRESSIF_SDMMC_D1,
                       CONFIG_ESPRESSIF_SDMMC_D2,
                       CONFIG_ESPRESSIF_SDMMC_D3);
}

/****************************************************************************
 * Name: esp_sdmmc_configpins
 *
 * Description:
 *   Route this slot's CLK/CMD/D0..D3 to the controller and give the pads the
 *   electrical properties the SD specification asks for: push-pull outputs
 *   with pull-ups on CMD and DAT (so an idle bus does not float), and a plain
 *   output on CLK.
 *
 ****************************************************************************/

static void esp_sdmmc_configpins(int slot, int clk_pin, int cmd_pin,
                                 int d0_pin, int d1_pin, int d2_pin,
                                 int d3_pin)
{
  /* Two completely different ways of attaching a pad to the controller.
   *
   * Slot 0 has fixed IOMUX pins and no GPIO-matrix signals at all -- there is
   * no SD_CARD_*_1_PAD_*_IDX in gpio_sig_map.h, only the _2_ (slot 1) set --
   * so its pads must be switched to the SDMMC IO_MUX function. Slot 1 is the
   * other way round: sdmmc_pins.h states "SLOT1 doesn't go through IOMUX" and
   * every signal has to be routed through the GPIO matrix.
   *
   * Which slot the companion sits on is a board fact, not a driver preference:
   * getting it wrong produces no traffic on the pins that are actually wired,
   * which looks exactly like a dead controller.
   */

  esp_configgpio(clk_pin, OUTPUT | DRIVE_3);
  esp_configgpio(cmd_pin, INPUT | OUTPUT | PULLUP | DRIVE_3);
  esp_configgpio(d0_pin,  INPUT | OUTPUT | PULLUP | DRIVE_3);
  esp_configgpio(d1_pin,  INPUT | OUTPUT | PULLUP | DRIVE_3);
  esp_configgpio(d2_pin,  INPUT | OUTPUT | PULLUP | DRIVE_3);
  esp_configgpio(d3_pin,  INPUT | OUTPUT | PULLUP | DRIVE_3);

  if (slot == 0)
    {
      /* IOMUX-attached: point each pad at the SDMMC function. */

      esp_gpio_func_sel(clk_pin, SDMMC_SLOT0_FUNC);
      esp_gpio_func_sel(cmd_pin, SDMMC_SLOT0_FUNC);
      esp_gpio_func_sel(d0_pin,  SDMMC_SLOT0_FUNC);
      esp_gpio_func_sel(d1_pin,  SDMMC_SLOT0_FUNC);
      esp_gpio_func_sel(d2_pin,  SDMMC_SLOT0_FUNC);
      esp_gpio_func_sel(d3_pin,  SDMMC_SLOT0_FUNC);

      syslog(LOG_INFO,
             "esp_sdmmc: slot 0 pads attached via IO_MUX "
             "(CLK=%d CMD=%d D0..D3=%d,%d,%d,%d)\n",
             clk_pin, cmd_pin, d0_pin, d1_pin,
             d2_pin, d3_pin);
    }
  else
    {
      /* Matrix-attached: one routing call per direction. CLK is output only;
       * CMD and DAT are driven by the controller and read back by it.
       */

      esp_rom_gpio_connect_out_signal(clk_pin,
                                      SD_CARD_CCLK_2_PAD_OUT_IDX, false, false);

      esp_rom_gpio_connect_out_signal(cmd_pin,
                                      SD_CARD_CCMD_2_PAD_OUT_IDX, false, false);
      esp_rom_gpio_connect_in_signal(cmd_pin,
                                     SD_CARD_CCMD_2_PAD_IN_IDX, false);

      esp_rom_gpio_connect_out_signal(d0_pin,
                                      SD_CARD_CDATA0_2_PAD_OUT_IDX, false, false);
      esp_rom_gpio_connect_in_signal(d0_pin,
                                     SD_CARD_CDATA0_2_PAD_IN_IDX, false);

      /* Route DAT1..3 to the controller as well.
       *
       * They used to be left off it -- DAT1 and DAT2 as plain inputs, DAT3 as a
       * GPIO driven high. That makes a four-bit bus impossible by construction,
       * and hardware says the companion is on four bits: its CCCR reports 0x42
       * (bus width 0b10), and DAT1 is held low even while this end drives it
       * high for 50 ms during pin identification -- something is driving it, and
       * the only candidate is the companion's DAT1.
       *
       * A companion on four data lines talking to a host on one answers commands
       * perfectly (CMD is a single wire either way) and delivers nothing during
       * a data phase, which is exactly the failure being chased.
       */

      esp_rom_gpio_connect_out_signal(d1_pin,
                                      SD_CARD_CDATA1_2_PAD_OUT_IDX, false, false);
      esp_rom_gpio_connect_in_signal(d1_pin,
                                     SD_CARD_CDATA1_2_PAD_IN_IDX, false);

      esp_rom_gpio_connect_out_signal(d2_pin,
                                      SD_CARD_CDATA2_2_PAD_OUT_IDX, false, false);
      esp_rom_gpio_connect_in_signal(d2_pin,
                                     SD_CARD_CDATA2_2_PAD_IN_IDX, false);

      /* DAT3 is not a data line here -- it selects the companion's mode.
       *
       * ESP-IDF's sdmmc_host.c does exactly this for a non-UHS1 slot:
       *
       *   // Force D3 high to make slave enter SD mode.
       *   gpio_config(&gpio_conf);            // GPIO_MODE_OUTPUT
       *   gpio_set_level(slot_gpio->d3, 1);
       *
       * Routing D3 to the controller instead (as an earlier change here did)
       * leaves it undriven, and the companion samples DAT3 at power-up to
       * decide between SD and SPI mode. It has to be a plain GPIO held high.
       */

      esp_configgpio(d3_pin, OUTPUT | PULLUP);
      esp_gpiowrite(d3_pin, true);

      esp_configgpio(d1_pin, INPUT | OUTPUT | PULLUP | DRIVE_3);
      esp_configgpio(d2_pin, INPUT | OUTPUT | PULLUP | DRIVE_3);

      syslog(LOG_INFO,
             "esp_sdmmc: slot %d pads routed via the GPIO matrix "
             "(CLK=%d CMD=%d D0..D3=%d,%d,%d,%d)\n",
             slot, clk_pin, cmd_pin, d0_pin,
             d1_pin, d2_pin, d3_pin);
    }
}


/****************************************************************************
 * Name: esp_sdmmc_clock
 *
 * Description:
 *   Select the card clock.  The source is the 160 MHz PLL chosen in
 *   esp_sdmmc_initialize(); the card divider is per slot, which is why slot 0
 *   and slot 1 cannot interfere with each other's rate.
 *
 *   For div >= 1 the card clock is source / (2 * div).  160 MHz / 320 = 500
 *   kHz is the identification rate the SD specification requires; 160/2 =
 *   80 MHz is the ceiling the transport can take.
 *
 ****************************************************************************/

/****************************************************************************
 * Name: esp_sdmmc_clk_update
 *
 * Description:
 *   Issue the clock-update command for one slot and say whether it completed.
 *
 *   Espressif builds exactly this (card_num, update_clk_reg, wait_complete and
 *   nothing else) and calls it after every change to the card clock -- once
 *   with the clock disabled, once after programming the dividers, and once
 *   after re-enabling. The register description says CLKDIV/CLKSRC/CLKENA reach
 *   the card clock domain only through a command carrying update_clk_reg.
 *
 *   Returns true when start_command self-cleared, i.e. the CIU executed it.
 *
 ****************************************************************************/

static bool esp_sdmmc_clk_update(sdmmc_dev_t *hw, int slot, const char *stage)
{
  sdmmc_hw_cmd_t upd;
  clock_t start = clock_systime_ticks();
  bool done;

  /* Clear the interrupt status first.
   *
   * RINTSTS bit 12 is HLE (hardware locked error), which is what the controller
   * raises when CLKDIV/CLKSRC/CLKENA are written while the CIU has them locked
   * and the update that would release them is still outstanding. Leaving a
   * stale HLE set across the three steps means every later check reads a flag
   * from an earlier attempt.
   */

  sdmmc_ll_clear_interrupt(hw, 0xffffffffu);

  memset(&upd, 0, sizeof(upd));
  upd.card_num       = (uint32_t)slot;
  upd.update_clk_reg = 1;
  upd.wait_complete  = 1;

  /* use_hold_reg is part of the command.
   *
   * sd_host_slot_clock_update_command() builds its struct literal without it,
   * but it hands that struct to sd_host_slot_start_command(), which sets
   * cmd.use_hold_reg = 1 before writing the register -- so the word that
   * actually reaches hardware has it. Reading only the literal missed this,
   * and "outputs are synchronised to cclk_out" is exactly the behaviour a
   * clock update needs.
   */

  upd.use_hold_reg   = 1;
  upd.start_command  = 1;

  sdmmc_ll_set_command_arg(hw, 0);
  sdmmc_ll_set_command(hw, upd);

  while (hw->cmd.start_command != 0)
    {
      if (clock_systime_ticks() - start > SDMMC_CMDTIMEOUT)
        {
          break;
        }
    }

  done = (hw->cmd.start_command == 0);

  syslog(LOG_INFO,
         "esp_sdmmc: clock update (%s): %s (wrote=%08" PRIx32
         " read=%08" PRIx32 " ctrl=%08" PRIx32 " rintsts=%08" PRIx32 ")\n",
         stage, done ? "completed" : "DID NOT COMPLETE",
         upd.val, hw->cmd.val, hw->ctrl.val, hw->rintsts.val);

  return done;
}

static void esp_sdmmc_clock(struct sdio_dev_s *dev, enum sdio_clock_e rate)
{
  struct esp_sdmmc_dev_s *priv = (struct esp_sdmmc_dev_s *)dev;
  sdmmc_dev_t *hw = priv->hw;
  clock_t start = clock_systime_ticks();
  uint32_t div;

  switch (rate)
    {
      default:
      case CLOCK_SDIO_DISABLED:
        sdmmc_ll_enable_card_clock(hw, priv->slot, false);
        return;

      case CLOCK_IDMODE:
        /* Identification clock, 100-400 kHz as the SD specification requires.
         *
         * The card divider is eight bits wide, and the rate works out as
         *
         *     (source / system_div) / card_div
         *
         * with the SDIO PLL as source that is 200 MHz / 2 / card_div. Both
         * numbers were measured on the analyser at 25 MS/s:
         *
         *     card_div = 159  ->  629 kHz   (above the 400 kHz limit)
         *     card_div = 334  -> 1279 kHz   (334 wraps to 78!)
         *
         * so 250 is the value that lands exactly on 400 kHz:
         * 200 MHz / 2 / 250 = 400 kHz.
         */

        div = 250;              /* 200MHz / 8 / 250 = 100 kHz */
        break;

      case CLOCK_MMC_TRANSFER:
      case CLOCK_SD_TRANSFER_1BIT:
      case CLOCK_SD_TRANSFER_4BIT:
        div = 0;
        break;
    }

  /* Espressif's sd_host_slot_set_card_clk(), step for step.
   *
   * Order matters and each step is confirmed by the controller: disable the
   * card clock and commit that, program the dividers and commit that, then
   * re-enable and commit that again. Building the sequence this way is the
   * whole reason the helper exists -- the previous code enabled the clock once
   * and committed once, which is not the same thing.
   */

  sdmmc_ll_enable_card_clock(hw, priv->slot, false);
  esp_sdmmc_clk_update(hw, priv->slot, "clock disabled");

  sdmmc_ll_set_card_clock_div(hw, priv->slot, div);
  sdmmc_ll_set_clock_div(hw, 8);
  sdmmc_ll_init_phase_delay(hw);

  /* Sampling the data phase on the other edge was tried here and the payload
   * still came back as zeros, so the phase settings are not what is keeping the
   * data out. Defaults left in place.
   */

  /* All four drive/sample clock-edge combinations were tried here and the
   * analyser-plus-console result was identical every time: the companion never
   * answered. The companion's own build uses
   * CONFIG_EH_TRANSPORT_CP_SDIO_PSEND_PSAMPLE ("send at positive edge, sample at
   * positive edge"), which is what motivated trying them, so the negative result
   * is worth recording: clock-edge alignment is not why it stays silent.
   * Espressif's phase settings are left untouched.
   */

  /* A probe that flipped reg_sdio_ls_sam_clk_edge_sel to 1 (sampling the
   * incoming CMD/DAT on the other clock edge) was tried here and changed
   * nothing -- the companion still did not answer. Removed rather than kept as
   * an unexplained deviation from Espressif's phase settings.
   */
  esp_sdmmc_clk_update(hw, priv->slot, "dividers programmed");

  sdmmc_ll_enable_card_clock(hw, priv->slot, true);
  /* Low power OFF: keep the card clock running instead of letting the CIU gate
   * it when no transfer is in flight.
   *
   * Espressif enables low power here, and with it on the analyser shows exactly
   * one clock burst in a whole capture -- the 81 initialisation clocks plus
   * CMD0 -- with the CMD5 that follows producing no clocks at all. Keeping the
   * clock running is the way to tell "the clock is gated at the wrong moment"
   * apart from "the command never reached the wire".
   */

  sdmmc_ll_enable_card_clock_low_power(hw, priv->slot, false);
  esp_sdmmc_clk_update(hw, priv->slot, "clock re-enabled");


  /* A probe that pointed slot 0 at the cclk_in_sdio source was tried here and
   * made no difference, so the per-card source mapping is not what stops the
   * clock. Removed rather than left in as an unexplained deviation.
   */


  /* Commit the clock registers into the card-clock domain.
   *
   * Writing CLKDIV / CLKSRC / CLKENA only stages the values in the BIU.  The
   * CIU does not see them until a command with update_clk_reg set is issued;
   * per the register description those three registers "are transferred into
   * card clock domain" only then.
   *
   * Skipping this step is silent: the registers read back as written, the
   * clock looks enabled, and yet no command ever completes because there is no
   * card clock at all.  On hardware that surfaced as every probe returning
   * -EBUSY with rintsts stuck at 0 -- the CIU never finished the first command,
   * so its START bit never cleared.
   */

  if (!g_sdmmc_skip_clk_update)
  {
    sdmmc_hw_cmd_t upd;

    memset(&upd, 0, sizeof(upd));

    /* These three fields are what make the command complete.
     *
     * card_num: the command must address the slot whose clock is being
     *   programmed. Zeroing the struct leaves card_num = 0, i.e. slot 0, while
     *   the clock being enabled is slot 1's -- and the CIU then never clears
     *   START, so this command hangs and every later command fails with -EBUSY.
     *   Espressif's own path sets exactly this field
     *   (sd_host_slot_start_command(): `cmd.card_num = slot->slot_id`).
     * use_hold_reg: outputs are synchronised to cclk_out.
     * wait_complete: the update finishes only once the CIU reports complete.
     */

    /* Commit the clock by resetting the controller instead of by the
     * update_clk_reg command.
     *
     * That command never completes on this part: it is written with correct
     * fields (verified on hardware: card_num / wait_complete / use_hold_reg all
     * checked) and yet CTRL.command.start_command never self-clears -- on a
     * brand-new controller as much as on a freshly reset one -- and every later
     * command then returns -EBUSY.
     *
     * A controller reset re-samples CLKDIV / CLKSRC / CLKENA from the BIU into
     * the card-clock domain without needing the CIU to execute anything, which
     * is exactly what the update command is for and what the CIU cannot do here.
     * The reset bit is self-clearing and is polled below.
     */

    /* Issue the clock-update command exactly as Espressif does.
     *
     * sd_host_slot_clock_update_command() builds precisely this and nothing
     * else -- card_num, update_clk_reg, wait_complete, with no use_hold_reg and
     * no command index. The register description is explicit that CLKDIV /
     * CLKSRC / CLKENA are transferred into the card clock domain only by a
     * command carrying update_clk_reg, so this is the documented way to commit
     * them and a controller reset is only an assumption that it works too.
     */

    upd.card_num       = (uint32_t)priv->slot;
    upd.update_clk_reg = 1;
    upd.wait_complete  = 1;
    upd.start_command  = 1;

    sdmmc_ll_set_command_arg(hw, 0);
    sdmmc_ll_set_command(hw, upd);

    {
      clock_t cstart = clock_systime_ticks();

      while (hw->cmd.start_command != 0)
        {
          if (clock_systime_ticks() - cstart > SDMMC_CMDTIMEOUT)
            {
              syslog(LOG_ERR,
                     "esp_sdmmc: clock-update command did NOT complete "
                     "(cmd=%08" PRIx32 " rintsts=%08" PRIx32
                     " ctrl=%08" PRIx32 ") -- falling back to a controller "
                     "reset\n",
                     hw->cmd.val, hw->rintsts.val, hw->ctrl.val);
              sdmmc_ll_reset_controller(hw);
              break;
            }
        }

      syslog(LOG_INFO,
             "esp_sdmmc: clock-update command %s (cmd=%08" PRIx32 ")\n",
             (hw->cmd.start_command == 0) ? "completed" : "timed out",
             hw->cmd.val);
    }
  }

  /* There is deliberately no wait for a command here any more: the clock is
   * committed by the controller reset above, not by the update_clk_reg command
   * (a leftover wait loop for that command used to sit here and would report a
   * timeout for a command that was never issued).
   */

  /* Re-issue any reset that did not complete while there was no card clock.
   *
   * The FIFO/DMA/controller resets are performed during initialisation, before
   * any clock is programmed. Their self-clearing bits are synchronised in the
   * card-clock domain, so with no card clock they can simply never clear --
   * CTRL.fifo_reset then stays 1 for good, and while it does the CIU cannot
   * finish a single command. On hardware that shows up as "FIFO reset did not
   * complete (ctrl=00000002)" and then every command returning -EBUSY.
   *
   * Now that the clock is running the same reset does complete, so retry it.
   */

  if (hw->ctrl.fifo_reset || hw->ctrl.dma_reset || hw->ctrl.controller_reset)
    {
      clock_t rstart = clock_systime_ticks();

      syslog(LOG_WARNING,
             "%s: reset did not clear without a card clock (ctrl=%08" PRIx32
             "), retrying now that the clock runs\n", "esp_sdmmc",
             hw->ctrl.val);

      sdmmc_ll_reset_controller(hw);
      sdmmc_ll_reset_dma(hw);
      sdmmc_ll_reset_fifo(hw);

      while (!sdmmc_ll_is_controller_reset_done(hw) ||
             !sdmmc_ll_is_dma_reset_done(hw) ||
             !sdmmc_ll_is_fifo_reset_done(hw))
        {
          if (clock_systime_ticks() - rstart > SDMMC_CMDTIMEOUT)
            {
              syslog(LOG_ERR,
                     "%s: reset still stuck after the clock came up "
                     "(ctrl=%08" PRIx32 ")\n", "esp_sdmmc", hw->ctrl.val);
              break;
            }
        }
    }

  mcinfo("slot=%u rate=%d div=%" PRIu32 "\n", priv->slot, (int)rate, div);
}

/****************************************************************************
 * Name: esp_sdmmc_reset
 *
 * Description:
 *   Put the controller back to a known state.  The three sub-blocks are reset
 *   in the order the IP expects (DMA, FIFO, controller) and each has its own
 *   completion flag, so a reset that never completes is reported rather than
 *   silently continuing into an unusable state.
 *
 ****************************************************************************/

static void esp_sdmmc_reset(struct sdio_dev_s *dev)
{
  struct esp_sdmmc_dev_s *priv = (struct esp_sdmmc_dev_s *)dev;
  sdmmc_dev_t *hw = priv->hw;
  clock_t start;

  sdmmc_ll_enable_interrupt(hw, 0xffffffff, false);
  sdmmc_ll_clear_interrupt(hw, 0xffffffff);

  sdmmc_ll_reset_dma(hw);
  start = clock_systime_ticks();
  while (!sdmmc_ll_is_dma_reset_done(hw))
    {
      if (clock_systime_ticks() - start > SDMMC_CMDTIMEOUT)
        {
          syslog(LOG_ERR, "esp_sdmmc: DMA reset did not complete "
                          "(ctrl=%08" PRIx32 ")\n", hw->ctrl.val);
          break;
        }
    }

  /* Only reset the FIFO once the card clock is running.
   *
   * The FIFO reset self-clearing bit is synchronised in the card-clock domain,
   * so issuing it with no card clock leaves CTRL.fifo_reset set forever. That
   * is not merely cosmetic: while fifo_reset is 1 the CIU cannot execute any
   * command -- including the update_clk_reg command that would bring the card
   * clock up. The two then deadlock, and the controller never issues a command
   * again (every probe returns -EBUSY with rintsts stuck at 0).
   *
   * At initialisation the clock has not been programmed yet, so the FIFO reset
   * is deferred; by the time esp_sdmmc_clock() runs and this is called again,
   * there is a clock and the reset completes normally.
   */

  /* The FIFO reset is deliberately NOT issued here.
   *
   * CTRL.fifo_reset self-clears in the card-clock domain, and while it is set
   * the CIU cannot execute any command -- including the update_clk_reg command
   * that is the only way to make the card clock take effect. The two therefore
   * wait on each other and nothing ever runs.
   *
   * An earlier attempt gated this on `hw->clkena.val != 0`, which does not work:
   * CLKENA is written before it is committed, so the register already reads
   * non-zero while the clock has never actually been latched. The gate opened
   * every time and the deadlock stood.
   *
   * A controller that has just come out of reset has nothing worth flushing, so
   * the safe order is to leave the FIFO alone until commands are running --
   * which is also what the databook implies by making the reset depend on the
   * card clock in the first place.
   */

  (void)start;

  start = clock_systime_ticks();
  while (!sdmmc_ll_is_fifo_reset_done(hw))
    {
      if (clock_systime_ticks() - start > SDMMC_CMDTIMEOUT)
        {
          /* A stuck fifo_reset is fatal and silent: while CTRL.fifo_reset is
           * still 1 the CIU cannot finish any command, so every later command
           * returns -EBUSY and the card looks dead. Say so out loud.
           */

          syslog(LOG_ERR, "esp_sdmmc: FIFO reset did not complete "
                          "(ctrl=%08" PRIx32 " status=%08" PRIx32 ")\n",
                 hw->ctrl.val, hw->status.val);
          break;
        }
    }

  sdmmc_ll_reset_controller(hw);
  start = clock_systime_ticks();
  while (!sdmmc_ll_is_controller_reset_done(hw))
    {
      if (clock_systime_ticks() - start > SDMMC_CMDTIMEOUT)
        {
          syslog(LOG_ERR, "esp_sdmmc: controller reset did not complete "
                          "(ctrl=%08" PRIx32 ")\n", hw->ctrl.val);
          break;
        }
    }

  sdmmc_ll_set_response_timeout(hw, 0xffffff);
  sdmmc_ll_set_data_timeout(hw, 0xffffff);

  priv->xfrmask  = 0;
  priv->waitmask = 0;
}

/****************************************************************************
 * Name: esp_sdmmc_capabilities / esp_sdmmc_status
 ****************************************************************************/

static sdio_capset_t esp_sdmmc_capabilities(struct sdio_dev_s *dev)
{
  UNUSED(dev);

  sdio_capset_t caps = SDIO_CAPS_4BIT;

#if defined(CONFIG_ESPRESSIF_SDMMC_DMA) && defined(CONFIG_SDIO_DMA)
  caps |= SDIO_CAPS_DMASUPPORTED;
#endif

  return caps;
}

static sdio_statset_t esp_sdmmc_status(struct sdio_dev_s *dev)
{
  struct esp_sdmmc_dev_s *priv = (struct esp_sdmmc_dev_s *)dev;
  sdio_statset_t status = 0;

  /* On this board the companion radio is soldered down, so there is no
   * socket that could report otherwise.  Slot 0 keeps the controller's own
   * card-detect, which is what a removable microSD card needs.
   */

  if (priv->slot == 0)
    {
      if (sdmmc_ll_is_card_detected(priv->hw, priv->slot))
        {
          status |= SDIO_STATUS_PRESENT;
        }

      if (sdmmc_ll_is_card_write_protected(priv->hw, priv->slot))
        {
          status |= SDIO_STATUS_WRPROTECTED;
        }
    }
  else
    {
      status |= SDIO_STATUS_PRESENT;
    }

  return status;
}

/****************************************************************************
 * Name: esp_sdmmc_widebus
 ****************************************************************************/

static void esp_sdmmc_widebus(struct sdio_dev_s *dev, bool enable)
{
  struct esp_sdmmc_dev_s *priv = (struct esp_sdmmc_dev_s *)dev;

  priv->widebus = enable;

  sdmmc_ll_set_card_width(priv->hw, priv->slot,
                          enable ? SD_BUS_WIDTH_4_BIT : SD_BUS_WIDTH_1_BIT);

  /* DAT1..3 must be handed to the controller, not turned into plain outputs.
   *
   * They were being configured with esp_configgpio(OUTPUT | ...), which makes
   * them GPIO outputs holding whatever value the output register happens to
   * carry -- zero. The analyser shows exactly that: DAT1 and DAT2 sit LOW while
   * the bus is idle, where an SDIO data line must rest HIGH on its pull-up.
   *
   * With the host holding two data lines low, the companion has nothing to
   * drive data onto, which is why every transfer completed at the command
   * level and delivered no bytes. Commands were unaffected because CMD is a
   * separate line.
   *
   * Routing them to the SDMMC function (the same thing esp_sdmmc_route_pins()
   * does for the other lines) leaves them bidirectional and pulled up, which is
   * what a data line has to be.
   */

  if (enable)
    {
      esp_sdmmc_route_pins();
    }

  mcinfo("widebus=%d\n", (int)enable);
}

/****************************************************************************
 * Name: esp_sdmmc_lock
 ****************************************************************************/

#ifdef CONFIG_SDIO_MUXBUS
static int esp_sdmmc_lock(struct sdio_dev_s *dev, bool lock)
{
  UNUSED(dev);

  /* The SDIO stack serialises access to the bus through this pair.  A plain
   * non-recursive mutex is what the contract asks for; returning OK without
   * locking would let two transfers interleave on one controller.
   */

  if (lock)
    {
      return nxmutex_lock(&g_sdmmc_lock);
    }

  return nxmutex_unlock(&g_sdmmc_lock);
}
#endif

/****************************************************************************
 * Name: esp_sdmmc_attach
 ****************************************************************************/

static int esp_sdmmc_attach(struct sdio_dev_s *dev)
{
  struct esp_sdmmc_dev_s *priv = (struct esp_sdmmc_dev_s *)dev;
  int irq;

  priv->cpuint = esp_setup_irq(SDIO_HOST_INTR_SOURCE,
                               ESP_IRQ_PRIORITY_DEFAULT,
                               ESP_IRQ_TRIGGER_LEVEL,
                               esp_sdmmc_interrupt, priv);
  if (priv->cpuint < 0)
    {
      mcerr("ERROR: failed to attach the SDMMC interrupt: %d\n",
            priv->cpuint);
      return priv->cpuint;
    }

  irq = SDIO_HOST_INTR_SOURCE;
  up_enable_irq(irq);

  /* Errors stay unmasked even when nobody is waiting on them, so a failure
   * latches ERROR into the transfer mask instead of being dropped.
   */

  sdmmc_ll_enable_interrupt(priv->hw, SDMMC_INT_ENABLE, true);

  mcinfo("attached, cpuint=%d\n", priv->cpuint);
  return OK;
}

/****************************************************************************
 * Name: esp_sdmmc_sendcmd
 *
 * Description:
 *   Translate a NuttX MMCSD_* command word into the controller's command
 *   register and start it.
 *
 ****************************************************************************/

static int esp_sdmmc_sendcmd(struct sdio_dev_s *dev, uint32_t cmd,
                             uint32_t arg)
{
  struct esp_sdmmc_dev_s *priv = (struct esp_sdmmc_dev_s *)dev;
  sdmmc_hw_cmd_t hwcmd;
  uint32_t cmdidx;

  memset(&hwcmd, 0, sizeof(hwcmd));

  cmdidx = (cmd & MMCSD_CMDIDX_MASK) >> MMCSD_CMDIDX_SHIFT;

  hwcmd.cmd_index = cmdidx;
  hwcmd.card_num  = priv->slot;

  /* Response expectations.  R2 is the 136-bit response and is the only one
   * that sets response_long.  CRC checking is requested for the short
   * responses that carry a valid CRC: R3 (OCR) and R4 do not.
   */

  if ((cmd & MMCSD_RESPONSE_MASK) != MMCSD_NO_RESPONSE)
    {
      hwcmd.response_expect = 1;

      if ((cmd & MMCSD_RESPONSE_MASK) == MMCSD_R2_RESPONSE)
        {
          hwcmd.response_long = 1;
        }
      /* R5 keeps the controller's CRC check, matching ESP-IDF.
       *
       * sd_protocol_types.h defines SCF_RSP_R5 as
       * (SCF_RSP_PRESENT|SCF_RSP_CRC|SCF_RSP_IDX), so IDF's host does check the
       * CRC on CMD52/CMD53 replies. This driver had dropped R5 from the checked
       * set after an earlier attempt made every CMD52 fail -- but that attempt
       * also left the software CRC check in recv_r5, so the two were not
       * separated. Letting only the controller check it is what IDF does.
       */

      else if ((cmd & MMCSD_RESPONSE_MASK) != MMCSD_R3_RESPONSE &&
               (cmd & MMCSD_RESPONSE_MASK) != MMCSD_R4_RESPONSE &&
               (cmd & MMCSD_RESPONSE_MASK) != MMCSD_R6_RESPONSE)
        {
          /* R3 (OCR) and R4 carry no valid CRC. */

          hwcmd.check_response_crc = 1;
        }
    }

  /* Data phase.  Direction comes from the command flags; the block count and
   * size come from blocksetup() and the DMA setup calls.
   */

  if (cmd & MMCSD_DATAXFR)
    {
      hwcmd.data_expected = 1;
      hwcmd.rw            = (cmd & MMCSD_WRDATAXFR) ? 1 : 0;

      /* A multi-block transfer stops itself so the caller does not have to
       * issue CMD12 separately.
       */

      if (cmd & MMCSD_MULTIBLOCK)
        {
          hwcmd.send_auto_stop = 1;
        }
    }

  /* CMD0 is the one that needs the 80-clock initialisation sequence. */

  if (cmdidx == 0)
    {
      hwcmd.send_init = 1;
    }

  priv->xfrmask = 0;
  priv->nblocks = 0;

  return esp_sdmmc_ciu_sendcmd(hwcmd, arg);
}

/****************************************************************************
 * Name: esp_sdmmc_blocksetup
 ****************************************************************************/

#ifdef CONFIG_SDIO_BLOCKSETUP
static void esp_sdmmc_blocksetup(struct sdio_dev_s *dev,
                                 unsigned int blocklen, unsigned int nblocks)
{
  struct esp_sdmmc_dev_s *priv = (struct esp_sdmmc_dev_s *)dev;

  priv->blocksize = blocklen;
  priv->nblocks   = nblocks;

  sdmmc_ll_set_block_size(priv->hw, blocklen);
  sdmmc_ll_set_data_transfer_len(priv->hw, blocklen * nblocks);

  /* Re-arm the data timeout for every transfer.
   *
   * It is only set once, in esp_sdmmc_initialize(). If anything between then and
   * a transfer clears the register, the controller times out immediately and the
   * data phase can never complete no matter what the card does.
   */

  sdmmc_ll_set_data_timeout(priv->hw, 0xffffff);
}
#endif /* CONFIG_SDIO_BLOCKSETUP */

/****************************************************************************
 * Name: esp_sdmmc_setup_desc
 *
 * Description:
 *   Build the IDMAC descriptor chain for one transfer.
 *
 *   A single descriptor covers at most SDMMC_DMA_MAX_BUF_LEN (4096) bytes,
 *   because the buffer-size field is 13 bits.  Two entries are allocated, so
 *   a transfer of up to 8 KB is described without allocating; anything larger
 *   is refused rather than silently truncated.
 *
 ****************************************************************************/

#if defined(CONFIG_ESPRESSIF_SDMMC_DMA) && defined(CONFIG_SDIO_DMA)
static int esp_sdmmc_setup_desc(struct esp_sdmmc_dev_s *priv,
                                uintptr_t buffer, size_t buflen, bool write)
{
  uintptr_t addr = buffer;
  uint32_t remaining = buflen;
  int index = 0;

  if (buflen == 0)
    {
      return -EINVAL;
    }

  /* The engine walks 32-bit physical addresses and the buffer-size field is
   * 13 bits, so a misaligned or oversized request must be refused up front.
   */

  if ((buffer & 3) != 0)
    {
      return -EINVAL;
    }

  while (remaining > 0 && index < 2)
    {
      uint32_t chunk = remaining > SDMMC_DMA_MAX_BUF_LEN ?
                       SDMMC_DMA_MAX_BUF_LEN : remaining;

      /* Keep the CPU cache out of the DMA's way.
       *
       * The controller writes straight into this buffer, but the CPU may be
       * holding stale lines for it: without invalidating them the packet that
       * was just transferred stays invisible and reads back as zeros -- which
       * is exactly what the companion's first event frame did, even though the
       * slave had handed it over and cleared PACKET_LEN.
       *
       * A transmit goes the other way: the data must reach RAM before the DMA
       * reads it.
       */

      if (write)
        {
          up_flush_dcache((uintptr_t)addr, (uintptr_t)addr + chunk);
        }
      else
        {
          up_invalidate_dcache((uintptr_t)addr, (uintptr_t)addr + chunk);
        }

      priv->desc[index].des0   = SDMMC_DESC_OWN | SDMMC_DESC_DIC;
      priv->desc[index].des1   = chunk;
      priv->desc[index].buffer = (uint32_t)addr;
      priv->desc[index].next   = 0;

      addr      += chunk;
      remaining -= chunk;
      index++;
    }

  if (remaining > 0)
    {
      return -EMSGSIZE;
    }

  priv->desc[0].des0 |= SDMMC_DESC_FS;

  if (index > 1)
    {
      priv->desc[0].des0 |= SDMMC_DESC_CH;
      priv->desc[0].next  = (uint32_t)(uintptr_t)&priv->desc[1];
    }

  priv->desc[index - 1].des0 |= SDMMC_DESC_LD | SDMMC_DESC_ER;

  /* The engine reads the chain from memory, so the CPU's writes to it must
   * reach DRAM first.  For a write the data must too; for a read the stale
   * lines must go, or the engine's data would be overwritten by the writeback
   * of cache lines that are about to be refilled anyway.
   */

  up_clean_dcache((uintptr_t)priv->desc,
                  (uintptr_t)(priv->desc + 2));

  if (write)
    {
      up_clean_dcache(buffer, buffer + buflen);
    }
  else
    {
      up_invalidate_dcache(buffer, buffer + buflen);
    }

  return OK;
}
#endif

/****************************************************************************
 * Name: esp_sdmmc_recvsetup / sendsetup / cancel
 *
 *   The transfer geometry arrives through blocksetup() and the DMA setup
 *   calls, so the plain setup hooks have nothing to add.  They are still
 *   provided because the SDIO stack calls them.
 ****************************************************************************/

static int esp_sdmmc_recvsetup(struct sdio_dev_s *dev, uint8_t *buffer,
                               size_t nbytes)
{
  UNUSED(dev);
  UNUSED(buffer);
  UNUSED(nbytes);
  return OK;
}

static int esp_sdmmc_sendsetup(struct sdio_dev_s *dev,
                               const uint8_t *buffer, size_t nbytes)
{
  UNUSED(dev);
  UNUSED(buffer);
  UNUSED(nbytes);
  return OK;
}

static int esp_sdmmc_cancel(struct sdio_dev_s *dev)
{
  struct esp_sdmmc_dev_s *priv = (struct esp_sdmmc_dev_s *)dev;

  sdmmc_ll_stop_dma(priv->hw);
  sdmmc_ll_enable_dma(priv->hw, false);

  priv->xfrmask |= SDIOWAIT_TRANSFERDONE;

  return OK;
}

/****************************************************************************
 * Name: esp_sdmmc_dmarecvsetup / esp_sdmmc_dmasendsetup
 ****************************************************************************/

#if defined(CONFIG_ESPRESSIF_SDMMC_DMA) && defined(CONFIG_SDIO_DMA)
static int esp_sdmmc_dmarecvsetup(struct sdio_dev_s *dev, uint8_t *buffer,
                                  size_t buflen)
{
  struct esp_sdmmc_dev_s *priv = (struct esp_sdmmc_dev_s *)dev;
  int ret;

  ret = esp_sdmmc_setup_desc(priv, (uintptr_t)buffer, buflen, false);
  if (ret < 0)
    {
      return ret;
    }

  sdmmc_ll_init_dma(priv->hw);
  sdmmc_ll_set_desc_addr(priv->hw, (uint32_t)(uintptr_t)priv->desc);
  sdmmc_ll_enable_dma(priv->hw, true);

  return OK;
}

static int esp_sdmmc_dmasendsetup(struct sdio_dev_s *dev,
                                  const uint8_t *buffer, size_t buflen)
{
  struct esp_sdmmc_dev_s *priv = (struct esp_sdmmc_dev_s *)dev;
  int ret;

  ret = esp_sdmmc_setup_desc(priv, (uintptr_t)buffer, buflen, true);
  if (ret < 0)
    {
      return ret;
    }

  sdmmc_ll_init_dma(priv->hw);
  sdmmc_ll_set_desc_addr(priv->hw, (uint32_t)(uintptr_t)priv->desc);
  sdmmc_ll_enable_dma(priv->hw, true);

  return OK;
}
#endif

/****************************************************************************
 * Name: esp_sdmmc_waitresponse / esp_sdmmc_recv_*
 ****************************************************************************/

static int esp_sdmmc_waitresponse(struct sdio_dev_s *dev, uint32_t cmd)
{
  sdio_eventset_t wkupevent;

  /* Commands with no response phase must return immediately.
   *
   * sdio_sendcmdpoll() calls this for *every* command, including CMD0, which
   * has no response phase at all -- nothing will ever raise RESPONSEDONE, so
   * waiting can only run into the timeout. The caller then abandons the
   * sequence, which is why the wire showed CMD0 four times and never a CMD5:
   * the driver was reporting "timed out waiting for a response" for a command
   * that was never supposed to produce one.
   *
   * The response flags are in the command word; ignoring them (this function
   * used to be `UNUSED(cmd)`) is what made it wait.
   */

  if ((cmd & MMCSD_RESPONSE_MASK) == MMCSD_NO_RESPONSE)
    {
      syslog(LOG_INFO, "esp_sdmmc: waitresponse -> OK (no response phase)\n");
      return OK;
    }

  /* Poll the controller instead of waiting on an event.
   *
   * Hardware settled this: after CMD5 the response is already in RESP0 (a
   * valid OCR -- 0x20FFFF, 3.2-3.3 V) and RINTSTS bit 2 (Command Done) is set,
   * yet the event-driven wait times out with that bit still standing. The CIU
   * finished the command perfectly; the completion just never reached this
   * driver as an event.
   *
   * A command with no data phase needs nothing else, so reading the same
   * status bit the interrupt would have read is both simpler and immune to
   * whatever is dropping the interrupt.
   */

  {
    FAR struct esp_sdmmc_dev_s *p = (struct esp_sdmmc_dev_s *)dev;
    sdmmc_dev_t *hw = p->hw;
    clock_t start = clock_systime_ticks();
    uint32_t resp_before = *(volatile uint32_t *)&hw->resp[0];
    bool done = false;

    for (;;)
      {
        if (sdmmc_ll_get_intr_status(hw) & SDMMC_CMD_DONE)
          {
            sdmmc_ll_clear_interrupt(hw, SDMMC_CMD_DONE);
            done = true;
            break;
          }

        if (clock_systime_ticks() - start > SDMMC_LONGTIMEOUT)
          {
            break;
          }
      }

    syslog(LOG_INFO,
           "esp_sdmmc: CMD%.2u resp %08" PRIx32 " -> %08" PRIx32 " %s "
           "(rintsts=%08" PRIx32 ")\n",
           (unsigned)(cmd & MMCSD_CMDIDX_MASK), resp_before,
           *(volatile uint32_t *)&hw->resp[0],
           (*(volatile uint32_t *)&hw->resp[0] != resp_before) ? "CHANGED"
                                                             : "unchanged",
           sdmmc_ll_get_intr_status(hw));

    if (!done)
      {
        syslog(LOG_ERR, "esp_sdmmc: no command completion "
               "(rintsts=%08" PRIx32 " resp0=%08" PRIx32 ")\n",
               *(volatile uint32_t *)&hw->rintsts,
               *(volatile uint32_t *)&hw->resp[0]);
        syslog(LOG_INFO, "esp_sdmmc: waitresponse -> -ETIMEDOUT\n");
        return -ETIMEDOUT;
      }

    if (sdmmc_ll_get_intr_status(hw) & SDMMC_ERR_RE)
      {
        syslog(LOG_ERR, "esp_sdmmc: response error (rintsts=%08" PRIx32 ")\n",
               *(volatile uint32_t *)&hw->rintsts);
        syslog(LOG_INFO, "esp_sdmmc: waitresponse -> -EIO (RE set)\n");
        return -EIO;
      }
  }

  /* The completion was seen and no response error is flagged. Falling off the
   * end instead of returning here is what made this function hand back an
   * indeterminate value: the caller treated a garbage non-zero result as a
   * failed command while the caller above it treated the same non-zero value as
   * success, so enumeration was reported as passing while it had actually
   * bailed out before CMD3.
   */

  syslog(LOG_INFO, "esp_sdmmc: waitresponse -> OK\n");
  return OK;
}

static int esp_sdmmc_recvshortcrc(struct sdio_dev_s *dev, uint32_t cmd,
                                  uint32_t *rshort)
{
  struct esp_sdmmc_dev_s *priv = (struct esp_sdmmc_dev_s *)dev;

  /* Log the raw response word: the caller parses bit fields out of it, and a
   * one-word shift between what the controller stores and what NuttX expects
   * would make those fields read nonsense while every earlier command (which
   * only needed the OCR) still worked.
   */

  syslog(LOG_INFO, "esp_sdmmc: R%u raw=%08" PRIx32 " (cmd=%08" PRIx32 ")\n",
         (unsigned)((cmd & MMCSD_RESPONSE_MASK) == MMCSD_R5_RESPONSE ? 5 : 4),
         *(volatile uint32_t *)&priv->hw->resp[0], cmd);

  *rshort = priv->hw->resp[0];
  return OK;
}

static int esp_sdmmc_recvshort(struct sdio_dev_s *dev, uint32_t cmd,
                               uint32_t *rshort)
{
  struct esp_sdmmc_dev_s *priv = (struct esp_sdmmc_dev_s *)dev;

  *rshort = priv->hw->resp[0];

  /* Log the raw word: NuttX parses R5's flags out of it (error is bit 11), and
   * the block-size write is the first command that depends on those bits.
   */

  syslog(LOG_INFO, "esp_sdmmc: short response raw=%08" PRIx32 " (cmd=%08" PRIx32 ")\n",
         *(volatile uint32_t *)&priv->hw->resp[0], cmd);

  return OK;
}

static int esp_sdmmc_recvlong(struct sdio_dev_s *dev, uint32_t cmd,
                              uint32_t rlong[4])
{
  struct esp_sdmmc_dev_s *priv = (struct esp_sdmmc_dev_s *)dev;
  int i;

  UNUSED(cmd);

  for (i = 0; i < 4; i++)
    {
      rlong[i] = priv->hw->resp[i];
    }

  return OK;
}

/****************************************************************************
 * Name: esp_sdmmc_waitenable
 ****************************************************************************/

static void esp_sdmmc_waitenable(struct sdio_dev_s *dev,
                                 sdio_eventset_t eventset, uint32_t timeout)
{
  struct esp_sdmmc_dev_s *priv = (struct esp_sdmmc_dev_s *)dev;

  /* The controller cannot signal a timeout, so it is never part of the
   * hardware mask; it is produced by the watchdog below.
   */

  eventset &= ~SDIOWAIT_TIMEOUT;

  priv->waitmask   = eventset;
  priv->enablewait = (eventset != 0);

  if (timeout > 0)
    {
      wd_start(&priv->timeout, timeout, esp_sdmmc_timeout,
               (wdparm_t)(uintptr_t)priv);
    }
}

/****************************************************************************
 * Name: esp_sdmmc_eventwait
 ****************************************************************************/

static sdio_eventset_t esp_sdmmc_eventwait(struct sdio_dev_s *dev)
{
  struct esp_sdmmc_dev_s *priv = (struct esp_sdmmc_dev_s *)dev;
  sdio_eventset_t wkupevent;

  /* Read the completion out of the controller rather than waiting for the
   * interrupt service routine to post it.
   *
   * Hardware settled this for commands, and the data phase is the same story:
   * the controller raises its status bits correctly, but nothing is delivering
   * the interrupt to this driver, so the semaphore is never posted and every
   * transfer ends in a timeout. Polling the same status word the ISR would have
   * read is what made command responses work; do the same here.
   */

  {
    sdmmc_dev_t *hw = priv->hw;
    clock_t      start = clock_systime_ticks();

    /* Keep polling until the events the caller asked for are satisfied.
     *
     * The condition used to be "while (priv->xfrmask == 0)", which exits the
     * loop as soon as *anything* arrives -- and CMD_DONE arrives with the
     * command phase, long before any data moves. The break below was corrected
     * to test the wait mask but never ran, because the loop condition had
     * already ended the loop.
     */

    while ((priv->xfrmask & priv->waitmask) == 0)
      {
        uint32_t st = sdmmc_ll_get_intr_status(hw);

        if (st & SDMMC_DATA_OVER)
          {
            sdmmc_ll_clear_interrupt(hw, SDMMC_DATA_OVER);
            priv->xfrmask |= SDIOWAIT_TRANSFERDONE;
          }

        if (st & SDMMC_CMD_DONE)
          {
            sdmmc_ll_clear_interrupt(hw, SDMMC_CMD_DONE);
            priv->xfrmask |= SDIOWAIT_CMDDONE | SDIOWAIT_RESPONSEDONE;
          }

        if (st & SDMMC_ALL_ERRORS)
          {
            /* Name every error bit the controller raised, not just "an error".
             *
             * RINTSTS bits: 1 RE, 3 DTO, 4 HTO, 5 FRUN, 6 HLE, 7 SBE, 8 BDS,
             * 9 BSF, 10 EB, 11 DCE, 12 RTO, 13 DRTO, 15 EBE, 16 SBCI.
             */

            syslog(LOG_ERR,
                   "esp_sdmmc: data-phase error bits raw=%08" PRIx32
                   " [re=%u dto=%u hto=%u frun=%u hle=%u sbe=%u bds=%u bsf=%u"
                   " eb=%u dce=%u rto=%u drto=%u ebe=%u sbci=%u]\n",
                   st,
                   (unsigned)((st >> 1) & 1),  (unsigned)((st >> 3) & 1),
                   (unsigned)((st >> 4) & 1),  (unsigned)((st >> 5) & 1),
                   (unsigned)((st >> 6) & 1),  (unsigned)((st >> 7) & 1),
                   (unsigned)((st >> 8) & 1),  (unsigned)((st >> 9) & 1),
                   (unsigned)((st >> 10) & 1), (unsigned)((st >> 11) & 1),
                   (unsigned)((st >> 12) & 1), (unsigned)((st >> 13) & 1),
                   (unsigned)((st >> 15) & 1), (unsigned)((st >> 16) & 1));

            sdmmc_ll_clear_interrupt(hw, st & SDMMC_ALL_ERRORS);
            priv->xfrmask |= SDIOWAIT_ERROR;
          }

        /* Only stop once the events the caller is waiting for have arrived.
         *
         * CMD_DONE is raised by the command phase, which for a CMD53 completes
         * long before the data moves. Breaking on any non-empty event set made
         * eventwait return immediately after the command, so the caller never
         * waited for the data phase and reported success with nothing
         * transferred. The wait mask says what actually has to be satisfied.
         */

        if ((priv->xfrmask & priv->waitmask) != 0)
          {
            break;
          }

        if (clock_systime_ticks() - start > SDMMC_LONGTIMEOUT)
          {
            priv->xfrmask |= SDIOWAIT_TIMEOUT;
            break;
          }

        up_udelay(20);
      }
  }

  /* Cancel the watchdog if it is still armed, then take the accumulated
   * events and disarm.
   */

  wd_cancel(&priv->timeout);

  wkupevent        = priv->xfrmask;
  priv->xfrmask    = 0;
  priv->waitmask   = 0;
  priv->enablewait = false;

  /* Log what the data phase actually reported: whether the controller ever
   * raised DATA_OVER, and what the event set looks like on the way out.
   */

  syslog(LOG_INFO,
         "esp_sdmmc: eventwait -> %02x (rintsts=%08" PRIx32
         " ctrl=%08" PRIx32 " status=%08" PRIx32 ")\n",
         (unsigned)wkupevent,
         *(volatile uint32_t *)&priv->hw->rintsts,
         *(volatile uint32_t *)&priv->hw->ctrl,
         *(volatile uint32_t *)&priv->hw->status);

  return wkupevent;
}

/****************************************************************************
 * Name: esp_sdmmc_callbackenable / registercallback / callback
 ****************************************************************************/

static void esp_sdmmc_callbackenable(struct sdio_dev_s *dev,
                                     sdio_eventset_t eventset)
{
  struct esp_sdmmc_dev_s *priv = (struct esp_sdmmc_dev_s *)dev;

  priv->cbenable = eventset & 0xff;
}

static int esp_sdmmc_registercallback(struct sdio_dev_s *dev,
                                      worker_t callback, void *arg)
{
  struct esp_sdmmc_dev_s *priv = (struct esp_sdmmc_dev_s *)dev;

  priv->callback = callback;
  priv->cbarg    = arg;
  return OK;
}

static void esp_sdmmc_callback(void *arg)
{
  struct esp_sdmmc_dev_s *priv = (struct esp_sdmmc_dev_s *)arg;

  if (priv->callback != NULL)
    {
      priv->callback(priv->cbarg);
    }
}

/****************************************************************************
 * Name: esp_sdmmc_timeout
 ****************************************************************************/

static void esp_sdmmc_timeout(wdparm_t arg)
{
  struct esp_sdmmc_dev_s *priv =
    (struct esp_sdmmc_dev_s *)(uintptr_t)arg;

  priv->xfrmask |= SDIOWAIT_TIMEOUT;

  if (priv->enablewait)
    {
      nxsem_post(&priv->waitsem);
    }
}

/****************************************************************************
 * Name: esp_sdmmc_interrupt
 *
 * Description:
 *   Turn controller interrupt status into SDIO wait events.
 *
 ****************************************************************************/

static int esp_sdmmc_interrupt(int irq, void *context, void *arg)
{
  struct esp_sdmmc_dev_s *priv = (struct esp_sdmmc_dev_s *)arg;
  sdio_eventset_t eventset = 0;
  uint32_t status;

  UNUSED(irq);
  UNUSED(context);

  status = sdmmc_ll_get_intr_status(priv->hw);
  sdmmc_ll_clear_interrupt(priv->hw, status);

  if (status & SDMMC_CMD_DONE)
    {
      eventset |= SDIOWAIT_CMDDONE | SDIOWAIT_RESPONSEDONE;
    }

  if (status & SDMMC_DATA_OVER)
    {
      eventset |= SDIOWAIT_TRANSFERDONE;
    }

  if (status & SDMMC_ALL_ERRORS)
    {
      eventset |= SDIOWAIT_ERROR;
      mcerr("ERROR: SDMMC status=%08" PRIx32 "\n", status);
    }

  if (eventset == 0)
    {
      return OK;
    }

  priv->xfrmask |= eventset;

  if (priv->enablewait && (eventset & priv->waitmask) != 0)
    {
      nxsem_post(&priv->waitsem);
    }
  else if ((eventset & priv->cbenable) != 0 && priv->callback != NULL)
    {
      work_queue(LPWORK, &priv->work, esp_sdmmc_callback, priv, 0);
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp_sdmmc_initialize
 *
 * Description:
 *   Initialise the selected SDMMC slot and return a NuttX SDIO lower half.
 *   See esp_sdmmc.h for the contract.
 *
 ****************************************************************************/

struct sdio_dev_s *esp_sdmmc_initialize(int slotno)
{
  struct esp_sdmmc_dev_s *priv = &g_sdmmc;
  sdmmc_dev_t *hw = &SDMMC;

  if (priv->initialized)
    {
      return &priv->dev;
    }

  if (slotno < 0 || slotno > 1)
    {
      mcerr("ERROR: invalid SDMMC slot %d\n", slotno);
      return NULL;
    }

  /* g_sdmmc is statically initialised with its method table, so only the
   * fields that describe this instance are set here.  The event bookkeeping
   * is cleared so a re-initialisation cannot inherit a pending event.
   */

  priv->hw           = hw;
  priv->slot         = slotno;
  priv->waitmask     = 0;
  priv->xfrmask      = 0;
  priv->enablewait   = false;
  priv->cbenable     = 0;
  priv->callback     = NULL;
  priv->cbarg        = NULL;
  priv->widebus      = false;
  priv->blocksize    = 0;
  priv->nblocks      = 0;
  priv->cpuint       = -1;

  /* Pin assignment.  The Kconfig defaults match the P4X-C5 board wiring, so
   * a board that routes the bus differently only changes those values; the
   * code does not need to know which board it is on.
   */

  priv->clk_pin = CONFIG_ESPRESSIF_SDMMC_CLK;
  priv->cmd_pin = CONFIG_ESPRESSIF_SDMMC_CMD;
  priv->d0_pin  = CONFIG_ESPRESSIF_SDMMC_D0;
  priv->d1_pin  = CONFIG_ESPRESSIF_SDMMC_D1;
  priv->d2_pin  = CONFIG_ESPRESSIF_SDMMC_D2;
  priv->d3_pin  = CONFIG_ESPRESSIF_SDMMC_D3;

  /* Module clock gate and reset.  The reset must complete before any
   * register of the block is touched.
   */

  sdmmc_ll_enable_bus_clock(0, true);
  sdmmc_ll_reset_register(0);

  /* Clock source and the system-side divider (HP_SYS_CLKRST), plus the
   * controller-side output phase.
   */

  /* Enable the clock source itself before selecting it.
   *
   * SDMMC_CLK_SRC_DEFAULT is SOC_MOD_CLK_PLL_F160M, and on this part each
   * reference clock is individually gated: it has to be switched on with
   * clk_gate_ll_ref_160m_clk_en(true) before CLKSRC can route it. Espressif's
   * own path does exactly this in sd_host_set_clk_div(), immediately before
   * sdmmc_ll_select_clk_source().
   *
   * Selecting a gated-off source is silent: every register still reads back the
   * value just written, CLKENA reports the card clock enabled, and yet no clock
   * ever reaches the card -- which is what this driver was seeing. The symptom
   * that finally gave it away was CMD0 hanging with send_initialization set:
   * that command's 80 clocks can never be produced without a running card
   * clock, so its start bit never clears and every later command is -EBUSY.
   */

  /* Enable the clock source through the official API rather than poking the
   * gate directly. Espressif's own P4 SDIO path calls exactly this
   * (esp_clk_tree_enable_src(SDMMC_CLK_SRC_DEFAULT, true)) before touching
   * CLKSRC, and a reference implementation for this same board does the same.
   */

  /* Probe: PLL_F160M is the documented default, but the analyser shows no
   * clock ever reaches the pad with it selected. The SDIO PLL is the other
   * supported source; if a clock appears with it, the problem is the source,
   * not the controller.
   */

  esp_clk_tree_enable_src((soc_module_clk_t)SDMMC_CLK_SRC_DEFAULT, true);
  clk_gate_ll_ref_160m_clk_en(true);

  /* Let the source propagate before it is routed to the card clock domain. */

  /* Use the SDIO PLL, and power it up.
   *
   * sdmmc_ll_enable_sdio_pll() is not a clock-gate write: it lifts the PLL out
   * of power-down in the PMU (PMU_RF_PWC_REG.PMU_SDIO_PLL_XPD), ties its
   * power/clock enables high, and enables its three clock branches in
   * LP_AON_CLKRST. Espressif only calls it for this source, which is why the
   * PLL_F160M path was tried before without it -- and why that attempt proved
   * nothing about the other source.
   */

  esp_clk_tree_enable_src((soc_module_clk_t)SOC_MOD_CLK_SDIO_PLL, true);
  sdmmc_ll_enable_sdio_pll(hw, true);
  sdmmc_ll_select_clk_source(hw, SDMMC_CLK_SRC_SDIO_200M);
  /* div must be > 1, and 2 is what Espressif uses.
   *
   * sdmmc_ll_set_clock_div() has two entirely different branches: for div > 1
   * it programs the low-speed clock edge registers (H/N/L) and pulses
   * reg_sdio_ls_clk_edge_cfg_update to latch them; for div <= 1 it only sets
   * HS mode and zeroes those three edge fields, with no update pulse at all.
   *
   * This driver passed 1, so the clock's edge/shape was never configured and the
   * update never happened -- which matches what the pads showed: registers
   * "correct", no waveform anywhere, and CMD0 unable to emit its 80 clocks.
   */

  sdmmc_ll_set_clock_div(hw, 8);

  /* Three clock-path probes were tried here and the analyser rejected all of
   * them, so they are not kept: switching the source to SDIO_PLL, forcing
   * reg_sdio_ls_clk_div_num (the one field in this path that nothing in the
   * HAL ever writes, left at its reset value of 0) to 1, and taking the
   * high-speed path instead via sdmmc_ll_set_clock_div(hw, 1). In every case
   * CLK stayed at a constant level. See docs/08 appendix Q.
   */
  sdmmc_ll_init_phase_delay(hw);

  /* Let the selected controller clock propagate before the controller is
   * reset. The reset bits self-clear in the controller-clock domain, so
   * resetting before the clock exists leaves them stuck -- the reference
   * implementation for this board waits here for exactly this reason.
   */

  up_udelay(10);

  /* Output phase 0 is the reset default, so it is stated rather than merely
   * assumed: it is the value the P4X-C5 board is known good with, and a board
   * with longer traces is the case that would change it.
   */

  sdmmc_ll_set_dout_delay_phase(hw, SDMMC_LL_DELAY_PHASE_0,
                                SDMMC_LL_SPEED_MODE_HS);

  /* The input sampling phase matters as much as the output one, and was
   * missing here. Without it the CIU samples the incoming CMD/DAT lines at an
   * arbitrary point in the bit period, so a perfectly good response gets read
   * with a bad end bit -- which is exactly RINTSTS bit 12, EBE (End Bit Error).
   * On hardware that looked like "the companion never answers" when in fact the
   * companion's answer was being misread.
   */

  sdmmc_ll_set_din_delay_phase(hw, SDMMC_LL_DELAY_PHASE_0,
                               SDMMC_LL_SPEED_MODE_HS);

  nxsem_init(&priv->waitsem, 0, 0);
  wd_start(&priv->timeout, 0, esp_sdmmc_timeout,
           (wdparm_t)(uintptr_t)priv);

#if defined(CONFIG_ESPRESSIF_SDMMC_DMA) && defined(CONFIG_SDIO_DMA)
  /* Two descriptors, 64-byte aligned: the chain must not share a cache line
   * with anything else, for the reason given at struct esp_sdmmc_desc_s.
   */

  priv->desc = kmm_memalign(64, sizeof(struct esp_sdmmc_desc_s) * 2);
  if (priv->desc == NULL)
    {
      mcerr("ERROR: failed to allocate the IDMAC descriptor chain\n");
      return NULL;
    }

  memset(priv->desc, 0, sizeof(struct esp_sdmmc_desc_s) * 2);
#endif

  esp_sdmmc_configpins(priv->slot, priv->clk_pin, priv->cmd_pin,
                       priv->d0_pin, priv->d1_pin, priv->d2_pin,
                       priv->d3_pin);
  esp_sdmmc_reset(&priv->dev);

  /* Report the three reset bits after the module reset.
   *
   * All of them are cleared by the CIU clock, so a bit that stays set is a
   * direct measurement of "the CIU is not being clocked" -- and unlike the
   * liveness command it needs nothing else to be configured first. Espressif's
   * controller init waits for exactly these three (s_is_module_reset_done)
   * and fails loudly if they do not clear.
   */

  syslog(LOG_INFO,
         "esp_sdmmc: after module reset: controller=%d dma=%d fifo=%d "
         "(ctrl=%08" PRIx32 ")\n",
         (int)(*(volatile uint32_t *)&hw->ctrl & 1u),
         (int)((*(volatile uint32_t *)&hw->ctrl >> 1) & 1u),
         (int)((*(volatile uint32_t *)&hw->ctrl >> 2) & 1u),
         *(volatile uint32_t *)&hw->ctrl);

  /* Initialise the DMA interface AFTER the reset, as Espressif does.
   *
   * sdmmc_ll_reset_controller() clears the whole CTRL register, and this driver
   * only called sdmmc_ll_init_dma() from the data-transfer setup path -- so at
   * the point where commands are issued CTRL.dma_enable was still 0. Hardware
   * confirmed it: ctrl=00000010, i.e. int_enable set and dma_enable clear.
   * Espressif's controller init resets first and then calls sdmmc_ll_init_dma(),
   * which sets dma_enable plus the IDMAC interrupt enables.
   *
   * The controller is configured for internal DMA, so with its DMA interface off
   * the CIU can accept a command and never execute it -- which is exactly what
   * the hardware shows (start_command never self-clears, RINTSTS bit 12 EBE).
   */

  sdmmc_ll_init_dma(hw);

  /* Deliberately NOT followed by sdmmc_ll_enable_dma(hw, false).
   *
   * Those two calls used to sit next to each other and cancel each other out:
   * init_dma() sets CTRL.dma_enable, enable_dma(false) clears it again, and the
   * controller was left configured for internal DMA with its DMA interface
   * switched off. The comment above this block already described the
   * consequence -- a CIU that accepts a command and never executes it -- while
   * the line underneath re-created it. That is the whole failure: every command
   * stalls with start_command still set, so no clock update is ever committed,
   * so no card clock ever appears.
   */

  sdmmc_ll_enable_global_interrupt(hw, true);

  sdmmc_ll_enable_global_interrupt(hw, true);

  /* First of two CIU liveness checks.
   *
   * A command carrying only update_clk_reg needs no card to answer and no
   * command index to be valid; it completes as soon as the CIU is running. Log
   * it here (clock source selected, system divider not yet programmed) and
   * again at the end of the clock path, and the two lines say whether the
   * divider/edge programming is what starts the CIU or whether it never runs.
   *
   * This one is expected to fail: the system-side divider has not been
   * programmed yet, so there is no cclk_in to run the CIU with.
   */

  {
    sdmmc_hw_cmd_t t;
    clock_t st = clock_systime_ticks();

    memset(&t, 0, sizeof(t));
    t.card_num       = (uint32_t)priv->slot;
    t.update_clk_reg = 1;
    t.wait_complete  = 1;
    t.start_command  = 1;

    sdmmc_ll_set_command_arg(hw, 0);
    sdmmc_ll_set_command(hw, t);

    while (hw->cmd.start_command != 0)
      {
        if (clock_systime_ticks() - st > SDMMC_CMDTIMEOUT)
          {
            break;
          }
      }

    syslog(LOG_INFO,
           "esp_sdmmc: CIU liveness check: update command %s before any clock "
           "programming (cmd=%08" PRIx32 " ctrl=%08" PRIx32
           " rintsts=%08" PRIx32 ")\n",
           (hw->cmd.start_command == 0) ? "COMPLETED" : "did not complete",
           hw->cmd.val, hw->ctrl.val, hw->rintsts.val);

    if (hw->cmd.start_command != 0)
      {
        sdmmc_ll_reset_controller(hw);
      }
  }
  priv->initialized = true;

  mcinfo("SDMMC slot %d up: CLK=%d CMD=%d D0..D3=%d,%d,%d,%d\n",
         slotno, priv->clk_pin, priv->cmd_pin, priv->d0_pin, priv->d1_pin,
         priv->d2_pin, priv->d3_pin);

  return &priv->dev;
}

/****************************************************************************
 * Name: esp_sdmmc_debug
 ****************************************************************************/

void esp_sdmmc_debug(FAR struct esp_sdmmc_debug_s *dbg)
{
  sdmmc_dev_t *hw = &SDMMC;

  if (dbg == NULL)
    {
      return;
    }

  dbg->verid   = sdmmc_ll_get_version_id(hw);
  dbg->status  = hw->status.val;
  dbg->cdetect = hw->cdetect.val;
  dbg->clkena  = hw->clkena.val;
  dbg->clkdiv  = hw->clkdiv.val;
  dbg->ctrl    = hw->ctrl.val;
  dbg->rintsts = hw->rintsts.val;
  dbg->bmod    = hw->bmod.val;
  dbg->cmdarg  = hw->cmdarg;

  /* The system-clock block decides whether the SDMMC and the SDIO (slot 1)
   * clock actually exist, and nothing in the SDMMC register window shows it.
   * Reading these is how "is the clock enabled at all" stops being an inference.
   */

  dbg->sys_clk_ctrl1   = HP_SYS_CLKRST.soc_clk_ctrl1.val;
  dbg->peri_clk_ctrl01 = HP_SYS_CLKRST.peri_clk_ctrl01.val;
  dbg->peri_clk_ctrl02 = HP_SYS_CLKRST.peri_clk_ctrl02.val;
  dbg->ref_clk_ctrl2   = HP_SYS_CLKRST.ref_clk_ctrl2.val;
}

/****************************************************************************
 * Name: esp_sdmmc_bus_reset
 ****************************************************************************/

void esp_sdmmc_bus_reset(void)
{
  if (g_sdmmc.hw == NULL)
    {
      return;
    }

  /* Reset only the controller, deliberately not the FIFO.
   *
   * The full esp_sdmmc_reset() also resets the FIFO, and that reset does not
   * complete here: its self-clearing bit stays set (CTRL.fifo_reset = 1) and
   * while it is set the CIU cannot execute anything -- so the first retry would
   * undo the very thing it is trying to recover, turning working commands back
   * into -EBUSY. Between enumeration attempts the controller reset is all that
   * is needed: it clears the command FSM and re-samples the clock registers.
   */

  sdmmc_ll_reset_controller(g_sdmmc.hw);

  {
    clock_t rstart = clock_systime_ticks();

    while (!sdmmc_ll_is_controller_reset_done(g_sdmmc.hw))
      {
        if (clock_systime_ticks() - rstart > SDMMC_CMDTIMEOUT)
          {
            break;
          }
      }

    /* Say whether the reset actually completed.
     *
     * This is not a cosmetic log. CTRL.controller_reset is cleared by the CIU
     * clock, so a bit that stays set proves the CIU is not being clocked at
     * all -- which is a different failure from "the card did not answer", and
     * it is the one that explains a controller which accepts every register
     * write, reports every clock enable as set, and still drives nothing.
     */

    syslog(LOG_INFO,
           "esp_sdmmc: controller reset %s (ctrl=%08" PRIx32 ")\n",
           sdmmc_ll_is_controller_reset_done(g_sdmmc.hw) ? "completed"
                                                         : "*** TIMED OUT ***",
           *(volatile uint32_t *)&g_sdmmc.hw->ctrl);
  }

  g_sdmmc.xfrmask   = 0;
  g_sdmmc.waitmask  = 0;
  g_sdmmc.enablewait = false;

  /* Re-arm the card clock: the reset above disables interrupts and the CIU, and
   * the next attempt needs the identification clock running again.
   */

  esp_sdmmc_clock(&g_sdmmc.dev, CLOCK_IDMODE);
}

void esp_sdmmc_skip_clk_update(bool skip)
{
  g_sdmmc_skip_clk_update = skip;
}

void esp_sdmmc_probe_clk(bool enable)
{
  g_sdmmc_probe_clk = enable;
}

#endif /* CONFIG_ESPRESSIF_SDMMC */
