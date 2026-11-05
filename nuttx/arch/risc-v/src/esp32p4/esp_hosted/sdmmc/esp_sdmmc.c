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
 * from a companion module reached over SDIO.  See docs/02_驱动实现/WiFi_SDIO实现.md.
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

/* Driver tracing.
 *
 * NuttX's syslog does not filter by level in this configuration -- a LOG_DEBUG
 * line reaches the console exactly like a LOG_INFO one -- so the driver's
 * per-command trace runs interleaved, character by character, with the official
 * stack's own log.  While the caps handshake is being brought up that stack log
 * is the one that matters and it cannot be read through the interleaving, so the
 * driver's trace is compiled out unless CONFIG_ESPRESSIF_SDMMC_TRACE is set.
 * Error paths keep using syslog(LOG_ERR, ...) and are always visible.
 */

#ifdef CONFIG_ESPRESSIF_SDMMC_TRACE
#  define sdmmc_trace(...)  sdmmc_trace( __VA_ARGS__)
#else
#  define sdmmc_trace(...)  do { } while (0)
#endif
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

/* Card interrupt.  RINTSTS carries the SDIO interrupt the *card* raises by
 * pulling DAT1 low (bits 16-17 of the register).  esp-hosted's host waits for
 * exactly this before it reads anything, and it is the one signal a pure
 * register poll cannot replace: the card asserts it as soon as it has something
 * queued, whereas polling only ever samples INT_RAW at arbitrary moments.
 */

#define SDMMC_SDIO_INTR       (3u << 16)  /* SDIO card interrupt    */

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

/* The SDIO card interrupt (DAT1) is deliberately *not* enabled here.
 *
 * It is level-triggered and cannot be cleared by writing RINTSTS, so while the
 * companion holds DAT1 low the controller re-interrupts immediately: with the
 * official stack running, the ISR fired hundreds of thousands of times and its
 * trace buried every other log (measured, 800k+ interrupts).  ESP-IDF waits on
 * the pin instead (sdmmc_host_io_int_wait), and so does this driver's
 * esp_sdmmc_wait_card_intr(); masking the controller source costs nothing the
 * pin check does not already provide.
 */

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
  volatile bool cardintr;        /* The card raised DAT1              */
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

/* Whether the data bus is currently four bits wide.
 *
 * Only one pad's role depends on it -- slot 1's D3, which selects the
 * companion's mode while the bus is one bit and is DAT3 once it is four -- and
 * the pin routing needs to know which of the two it is being asked for.  See
 * esp_sdmmc_configpins() and esp_sdmmc_widebus().
 */

static bool g_widebus;

/* RINTSTS error bits seen by the last data transfer's wait loop. */

static uint32_t g_last_error_bits;

/* Set while the interrupt handler has HLE masked.
 *
 * HLE (hardware locked error) is level-driven: the CIU stays locked until the
 * controller is reset, so writing RINTSTS does not clear the condition and the
 * controller re-interrupts immediately -- measured 2026-11-04: the core never
 * returned to task context (JTAG PC sampling 8/8 inside _interrupt_handler /
 * riscv_dispatch_irq), the console stopped and NSH never started.  The source is
 * therefore masked in the handler, exactly like the level-triggered DAT1 source,
 * and re-armed when the next transfer is armed.
 */

static bool g_hle_masked;

/* Programmed-I/O data path.
 *
 * The controller's internal DMA engine never fetches a descriptor on this
 * board. Measured, with the FIFO holding received data and a valid descriptor
 * chain armed:
 *
 *   status=0003a900 (FIFO_COUNT = 1, data state machine idle)
 *   idsts=00002000  (FSM = DMA_SUSPEND, no DU, no fatal bus error)
 *   des0=8000001c   (OWN still set)
 *   dbaddr=4ff69ec0 bmod=00000082 ctrl=02000030 (enabled, internal DMA selected)
 *
 * and it stays there through poll demand, through a BMOD.SWR reset of the engine
 * and through re-enabling it. The bytes the companion sent are in the FIFO and
 * read back correctly (0x3c -- the register value asked for), so the link and
 * the data phase are fine; only the engine that is supposed to move them to
 * memory is not.
 *
 * Transfers of up to one FIFO (512 bytes) therefore go through the CPU instead:
 * the CIU is taken out of internal-DMA mode for the transfer, the receive FIFO
 * is drained into the caller's buffer while the data phase runs, and a write
 * fills the transmit FIFO from it. That is ordinary programmed I/O for an SD
 * host and it needs no engine cooperation. The DMA path is kept for anything
 * larger, where the FIFO alone cannot hold the transfer.
 */

#define ESP_SDMMC_PIO_MAX   512        /* bytes the FIFO can hold        */
#define ESP_SDMMC_FIFO_PORT 0x200      /* FIFO data port, not in the struct */

static uint8_t       *g_pio_rxbuf;     /* Receive buffer, NULL when unused */
static size_t         g_pio_rxlen;
static size_t         g_pio_rxdone;
static const uint8_t *g_pio_txbuf;     /* Transmit buffer, NULL when unused */
static size_t         g_pio_txlen;
static size_t         g_pio_txdone;

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

  sdmmc_trace(
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

  /* Kick the DMA engine NOW, not before the command.
   *
   * A poll demand written while the CIU is not transferring is ignored: the
   * engine stays suspended, never fetches the descriptor list -- its OWN bit in
   * the descriptor stays set -- and no data moves. Measured exactly that:
   *
   *     des0=8000002e   (bit 31 = OWN, still set)
   *     idsts=00002000  (FSM field = 1, suspended)
   *
   * ESP-IDF kicks the engine only once a transfer is in flight: its IDMAC
   * interrupt refills descriptors and calls sdmmc_host_dma_resume(), which is
   * nothing but a PLDMND write. Issuing the kick immediately after the command
   * is the same idea without the interrupt plumbing.
   */

  if (hwcmd.data_expected != 0)
    {
      sdmmc_ll_poll_demand(hw);
    }

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

      sdmmc_trace(
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

      sdmmc_trace(
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

  sdmmc_trace( "esp_sdmmc: pin identification starting\n");

  /* A one-pin probe that pulsed GPIO18 alone (three 50 ms pulses) was used to
   * settle which analyser channel carries CLK. Result: the pulses appeared on
   * channel 1 while channel 0 stayed flat, i.e. CLK is on channel 1 and CMD on
   * channel 0 -- one off from the channel list supplied with the board. The
   * six-pin loop below is kept because it identifies all six at once.
   */

  for (i = 0; i < 6; i++)
    {
      sdmmc_trace( "esp_sdmmc: %s (GPIO%d) -> %d pulse(s)\n",
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

  sdmmc_trace( "esp_sdmmc: pin identification done\n");
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
      sdmmc_trace(
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

  sdmmc_trace( "esp_sdmmc: CMD5-OCR probe entered (hw=%p slot=%d)\n",
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

  sdmmc_trace(
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

  sdmmc_trace(
         "esp_sdmmc: dma state: des0=%08" PRIx32 " des1=%08" PRIx32
         " buf=0%08" PRIx32 " ctrl=%08" PRIx32 " status=%08" PRIx32
         " idsts=%08" PRIx32 " dbaddr=%08" PRIx32 " bmod=%08" PRIx32 "\n",
         g_sdmmc.desc[0].des0, g_sdmmc.desc[0].des1,
         g_sdmmc.desc[0].buffer,
         *(volatile uint32_t *)&hw->ctrl,
         *(volatile uint32_t *)&hw->status,
         sdmmc_ll_get_idsts_interrupt_raw(hw),
         *(volatile uint32_t *)&hw->dbaddr,
         *(volatile uint32_t *)&hw->bmod);
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

      sdmmc_trace(
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

      /* DAT3 changes role with the bus width.
       *
       * Before the bus is four bits wide this pad is what selects the
       * companion's mode: it samples D3 at power-up to choose between SD and
       * SPI, so it has to be held high. ESP-IDF's sdmmc_host.c does exactly
       * this for a non-UHS1 slot:
       *
       *   // Force D3 high to make slave enter SD mode.
       *   gpio_config(&gpio_conf);            // GPIO_MODE_OUTPUT
       *   gpio_set_level(slot_gpio->d3, 1);
       *
       * Once the bus *is* four bits wide the same pad is DAT3 and belongs to
       * the controller -- IDF re-configures it right there, in
       * sdmmc_host_set_bus_width(), with the comment "D3 was set to GPIO high
       * to force slave into SD mode, until 4-bit mode is set".
       *
       * Leaving it as a GPIO in four-bit mode fails silently, and it is the one
       * failure this driver had not separated from "the companion sent
       * nothing": the CIU recognises the start of a data block by all four data
       * lines being low for one clock, so with DAT3 stuck at the level a GPIO
       * drives it never sees one -- no RXDR, no DATA_OVER, no error bit, an
       * empty FIFO and a host-side timeout, while the analyser shows the
       * companion driving all four lines with correct data.
       */

      if (g_widebus)
        {
          esp_rom_gpio_connect_out_signal(d3_pin,
                                          SD_CARD_CDATA3_2_PAD_OUT_IDX,
                                          false, false);
          esp_rom_gpio_connect_in_signal(d3_pin,
                                         SD_CARD_CDATA3_2_PAD_IN_IDX,
                                         false);
          esp_configgpio(d3_pin, INPUT | OUTPUT | PULLUP | DRIVE_3);
        }
      else
        {
          esp_configgpio(d3_pin, OUTPUT | PULLUP);
          esp_gpiowrite(d3_pin, true);
        }

      esp_configgpio(d1_pin, INPUT | OUTPUT | PULLUP | DRIVE_3);
      esp_configgpio(d2_pin, INPUT | OUTPUT | PULLUP | DRIVE_3);

      sdmmc_trace(
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

  sdmmc_trace(
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

  /* Name every clock change and the state it arrives in.
   *
   * The clock sequence starts by disabling the card clock and only re-enables it
   * at the end, so a clock change that is issued while the CIU is busy leaves the
   * card with no clock at all -- measured: clkena fell to 00000000 in the middle
   * of the transport's transfers, after which every CMD53 stalled with
   * "fifo=0 data_state_mc_busy=1" (status 0001ad06).  Which rate was asked for,
   * and what the controller was doing at that moment, is the fact needed to fix
   * the caller rather than the symptom.
   */

  {
    static int clock_calls;

    if (clock_calls < 12)
      {
        sdmmc_trace(
               "esp_sdmmc: clock(%d) requested: status=%08" PRIx32
               " clkena=%08" PRIx32 " cmd=%08" PRIx32 " idsts=%08" PRIx32 "\n",
               (int)rate, *(volatile uint32_t *)&hw->status, hw->clkena.val,
               *(volatile uint32_t *)&hw->cmd, priv->hw->idsts.val);
        clock_calls++;
      }
  }

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

        /* Full rate, matching the working official firmware.
         *
         * An earlier round set div = 250 (400 kHz) to mimic a minimal IDF probe,
         * but that probe is not the reference: the official esp-hosted project
         * (~/p4official) runs SDIO at 40 MHz over 4 bits, and its own register
         * dump shows clkdiv=00000000 -- i.e. the card divider bypassed and the
         * rate coming from the system clock block.  Bringing this back to zero
         * is what makes the two configurations comparable.
         */

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

      sdmmc_trace(
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

  /* Never return with the card clock off unless that was what was asked for.
   *
   * The sequence above starts by disabling the card clock, and the commit that
   * re-enables it can fail while the CIU is busy -- a clock change that arrives
   * in the middle of a transfer is a command like any other and does not
   * complete.  Measured: clkena fell to 00000000 during the transport's
   * transfers (the trace pointed at the stack's card_init retry, which asks for
   * CLOCK_IDMODE again), and from then on every CMD53 stalled with
   * "fifo=0 data_state_mc_busy=1" -- the bus was dead, not slow (appendix
   * BY.8).
   *
   * Putting the clock back costs one register write and keeps a failed clock
   * change from killing the link.  For CLOCK_SDIO_DISABLED the caller wants the
   * clock off, so nothing is done there.
   */

  if (rate != CLOCK_SDIO_DISABLED && hw->clkena.val == 0)
    {
      syslog(LOG_WARNING,
             "%s: card clock was left off by a clock change (ctrl=%08" PRIx32
             " cmd=%08" PRIx32 "), re-enabling it\n", "esp_sdmmc",
             hw->ctrl.val, *(volatile uint32_t *)&hw->cmd);

      sdmmc_ll_enable_card_clock(hw, priv->slot, true);
      esp_sdmmc_clk_update(hw, priv->slot, "clock restored");
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

  /* Bring the FIFO thresholds back to something a transfer can reach.
   *
   * The controller reset above restores FIFOTH to its hardware default,
   * rx_wmark = 511 bytes, and with it the IDMAC is only asked to fetch the
   * descriptor once the FIFO has passed that mark -- which no register read on
   * this link ever does.  The descriptor then sits armed and the DMA state
   * machine in DMA_SUSPEND forever: measured in the transport,
   * "dwait status=0001ad06 fifo=0 datbusy=0 mc=1 idsts=00002000 des0=8000001c",
   * i.e. data_state_mc_busy with an empty FIFO, followed by -ETIMEDOUT on a
   * four-byte register read and "eh_sdio: failed to read registers"
   * (appendix BY.8).
   *
   * esp_sdmmc_reset_data_path() programs this per transfer, but every controller
   * reset -- and the clock path performs one -- throws it away again, so it is
   * programmed here too: a reset must not leave the data path unable to move
   * the first byte.
   */

  hw->fifoth.rx_wmark = 0;
  hw->fifoth.tx_wmark = 0;

  /* Put the interrupt mask back the way attach() left it.
   *
   * The reset starts by disabling every source, and without this line that is
   * how the controller stays: the completion bits are still set in the raw
   * RINTSTS, but every reader of the *masked* register (sdmmc_ll_get_intr_status
   * returns MINTSTS) sees zero.  Measured at the hand-over, where a clock change
   * runs this reset: the card answered CMD5 (raw RINTSTS = 00000004, RESP0 =
   * a0ffff00) and this driver still reported "no command completion" followed by
   * -ETIMEDOUT, after which every probe retry failed (appendix BY.6).
   */

  sdmmc_ll_enable_interrupt(hw, SDMMC_INT_ENABLE, true);

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
  g_widebus     = enable;

  sdmmc_ll_set_card_width(priv->hw, priv->slot,
                          enable ? SD_BUS_WIDTH_4_BIT : SD_BUS_WIDTH_1_BIT);

  /* Re-route the pads for the width that was just programmed.
   *
   * This is not optional and not only about DAT1..3 being handed to the
   * controller: D3 changes role with the width (mode select -> DAT3), so the
   * routing has to be redone here, not just at bring-up.  See the D3 note in
   * esp_sdmmc_configpins().
   *
   * DAT1..3 must be handed to the controller, not turned into plain outputs.
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

  esp_sdmmc_route_pins();

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

  /* Enable the *IRQ*, not the peripheral source number.
   *
   * irq.h maps the two with ESP_SOURCE2IRQ(id) == id + ESP_IRQ_FIRSTPERIPH.
   * This used to call up_enable_irq(SDIO_HOST_INTR_SOURCE) -- the raw source
   * number -- so the line was enabled on an unrelated IRQ and this handler was
   * never entered. That is why every wait in this driver was later rewritten to
   * poll, and why the companion's SDIO interrupt (DAT1) was never seen.
   */

  irq = ESP_SOURCE2IRQ(SDIO_HOST_INTR_SOURCE);
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

static void esp_sdmmc_reset_data_path(struct esp_sdmmc_dev_s *priv);

static int esp_sdmmc_sendcmd(struct sdio_dev_s *dev, uint32_t cmd,
                             uint32_t arg)
{
  struct esp_sdmmc_dev_s *priv = (struct esp_sdmmc_dev_s *)dev;
  sdmmc_hw_cmd_t hwcmd;
  uint32_t cmdidx;
  static int traces;

  /* Put the data path back before every command.
   *
   * Measured: the transport's CMD53 is written to the controller and never
   * completes, while the identical command completes during bring-up -- and the
   * controller's IDMAC reports FSM = SUSPEND (idsts = 00002000) at that point.
   * The data path reset exists for exactly this state but was only reached from
   * the DMA transfer setup, which byte-mode register reads do not go through.
   * Doing it here covers every command.
   */

  esp_sdmmc_reset_data_path(priv);

  if (traces < 200)
    {
      sdmmc_trace(
             "esp_sdmmc: sendcmd cmd=%02u arg=%08x ctrl=%08x status=%08x"
             " bmod=%08x intmask=%08x tmout=%08x\n",
             (unsigned)((cmd & MMCSD_CMDIDX_MASK) >> MMCSD_CMDIDX_SHIFT),
             (unsigned)arg, priv->hw->ctrl.val, priv->hw->status.val,
             priv->hw->bmod.val, priv->hw->intmask.val, priv->hw->tmout.val);
      traces++;
    }

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

      /* Direction comes from the dedicated write bit, NOT from MMCSD_WRDATAXFR.
       *
       * sdio.h defines the two transfer flags so that they overlap:
       *
       *     MMCSD_RDDATAXFR = MMCSD_DATAXFR               (bit 10)
       *     MMCSD_WRDATAXFR = MMCSD_DATAXFR|MMCSD_WRXFR   (bits 10 and 12)
       *
       * Testing `cmd & MMCSD_WRDATAXFR` is therefore non-zero for a read as
       * well, because a read sets bit 10 too. Every read was consequently
       * handed to the controller with rw = 1 (write to card): the measured
       * command word 0xa0010775 has bit 10 set and bit 12 clear, the CIU
       * answered with TXDR (RINTSTS bit 4, "TX FIFO data request") and the data
       * phase ran into the data timeout with nothing ever read.
       *
       * MMCSD_WRXFR is the bit that actually distinguishes the two.
       */

      hwcmd.rw            = (cmd & MMCSD_WRXFR) ? 1 : 0;

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
  /* wait_complete is deliberately NOT set here.
   *
   * ESP-IDF does set it (make_hw_cmd sets it on every command except GO_IDLE,
   * STOP_TRANSMISSION and the voltage switch), so this is a real difference --
   * but it cannot be adopted while CMD53 does not complete: with the bit set,
   * the next command waits for the previous transfer and the driver reports
   *   "previous command still pending after 100 ticks (cmd=a0012174 ...)"
   * on the first CMD52 after a data command. That measurement is itself useful
   * evidence -- the data command is issued and never finishes -- but turning the
   * hang into a -EBUSY storm is worse than leaving the bit clear. Revisit once
   * the data phase works.
   */

  priv->xfrmask = 0;
  priv->nblocks = 0;

  /* Drop the previous command's completion flags before this one goes out.
   *
   * The ISR records CMDDONE/RESPONSEDONE in xfrmask, and a command can finish
   * before waitresponse() is even entered. Clearing here (rather than at the
   * start of waitresponse) keeps such an early completion instead of throwing
   * it away -- which is exactly what made every command after CMD5 time out.
   */

  priv->xfrmask &= ~(SDIOWAIT_CMDDONE | SDIOWAIT_RESPONSEDONE);

  /* Re-arm both timeouts before every command.
   *
   * TMOUT carries the response timeout and the data timeout in one register,
   * and both were programmed only once, in esp_sdmmc_initialize(). blocksetup()
   * already re-arms the data half for exactly this reason; the response half
   * was left alone. A response timeout that has been cleared makes the CIU
   * report RTO the moment a command is issued -- and the observed pattern is
   * precisely that of a short timeout: fast CMD52 exchanges complete, while
   * CMD53 (which the companion needs longer to answer) comes back as
   * "ISR raw status=00000100" (bit 8, RTO).
   */

  sdmmc_ll_set_response_timeout(priv->hw, 0xffffff);
  sdmmc_ll_set_data_timeout(priv->hw, 0xffffff);

  /* NOTE: the data path is no longer reset here.
   *
   * STATUS reports DATA_STATE_MC_BUSY together with FIFO_COUNT = 0 once a data
   * transfer has failed, and the machine does not return to idle by itself, so
   * a recovery reset is needed -- but this is the wrong place for it.
   *
   * NuttX orders a read as
   *
   *     SDIO_DMARECVSETUP   (setup_desc -> DBADDR -> enable_dma -> PLDMND)
   *     SDIO_SENDCMD        (this function)
   *     SDIO_EVENTWAIT
   *
   * so a reset here lands *between* arming the engine and issuing the command
   * and throws the armed state away: the descriptor's OWN bit stays set, IDSTS
   * reports the FSM suspended and the CIU waits for data the engine never
   * fetches. That is exactly the measured failure signature (idsts=00002000,
   * zero bytes, data timeout) and it also explains why the failure looked
   * independent of geometry, size, mode and clock.
   *
   * ESP-IDF never resets the DMA/FIFO around a transfer either: it arms
   * (sdmmc_host_dma_prepare() ends with enable_dma + poll_demand) and then calls
   * sdmmc_host_start_command(); CTRL.FIFO_RST/DMA_RST are touched only in
   * sdmmc_host_reset(), i.e. at initialisation.
   *
   * The reset has therefore moved to the top of esp_sdmmc_dmarecvsetup() /
   * esp_sdmmc_dmasendsetup(): same recovery, but before the chain is armed
   * rather than after.
   */

  if ((cmd & MMCSD_DATAXFR) != 0)
    {
      /* Measure rather than assume.
       *
       * BLKSIZ and BYTCNT decide how many bytes the CIU moves and CTRL says
       * whether the internal DMA is armed. CMDARG is what actually reaches the
       * card, and CDETECT is the card-detect state -- ESP-IDF refuses to issue
       * a command at all when the card is not detected, so it is worth knowing
       * what this controller reports.
       */

      sdmmc_trace(
             "esp_sdmmc: data cmd regs: blksiz=%08" PRIx32 " bytcnt=%08" PRIx32
             " cmdarg=%08" PRIx32 " cdetect=%08" PRIx32 " ctrl=%08" PRIx32
             " tmout=%08" PRIx32 "\n",
             priv->hw->blksiz.val, priv->hw->bytcnt.val,
             (uint32_t)priv->hw->cmdarg,
             (uint32_t)priv->hw->cdetect.card_detect_n,
             priv->hw->ctrl.val, priv->hw->tmout.val);

      /* Same nine fields the working official firmware prints every 20 ms.
       *
       * Measured on the official stack while it is receiving RPC events from
       * this very companion:
       *
       *   ctrl=02000030 clkena=00020002 tmout=3d0900ff cdetect=00000000
       *   rintsts=00000010 intmask=0002b7cf bmod=00000082 idinten=00000103
       *   idsts=00000000 clkdiv=00000000
       *
       * Two of those differ from what this driver shows at a failed transfer
       * (rintsts=0 and idsts=00002000, IDMAC FSM suspended), so the dump is
       * widened to the same set to make the comparison line-by-line.
       */

      sdmmc_trace(
             "esp_sdmmc: regs2: ctrl=%08" PRIx32 " clkena=%08" PRIx32
             " tmout=%08" PRIx32 " cdetect=%08" PRIx32 " rintsts=%08" PRIx32
             " intmask=%08" PRIx32 " bmod=%08" PRIx32 " idinten=%08" PRIx32
             " idsts=%08" PRIx32 " clkdiv=%08" PRIx32 "\n",
             priv->hw->ctrl.val, priv->hw->clkena.val, priv->hw->tmout.val,
             (uint32_t)priv->hw->cdetect.card_detect_n,
             priv->hw->rintsts.val, priv->hw->intmask.val, priv->hw->bmod.val,
             priv->hw->idinten.val, priv->hw->idsts.val, priv->hw->clkdiv.val);
    }

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

  /* Byte mode asks for byte count, not blocks.
   *
   * When NuttX issues a CMD53 in byte mode it calls this with nblocks = 0, and
   * "blocklen * 0" programmed BYTCNT as zero -- so the controller expected a
   * zero-byte transfer and never completed one.  Measured: the transport's
   * first register read (INT_RAW..PACKET_LEN, 20 bytes, byte mode) entered
   * eh_host_port_sdio_read_reg() and never returned, while the block-mode reads
   * of the bring-up sweep worked.
   */

  sdmmc_ll_set_data_transfer_len(priv->hw,
                                 blocklen * (nblocks != 0 ? nblocks : 1));

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

/****************************************************************************
 * Name: esp_sdmmc_reset_data_path
 *
 * Description:
 *   Put the CIU's data path back to a known state before a transfer is armed.
 *
 *   A failed transfer leaves STATUS.DATA_STATE_MC_BUSY set with FIFO_COUNT = 0
 *   and the machine does not return to idle on its own, so without this every
 *   later transfer inherits the failure. CTRL.FIFO_RST and CTRL.DMA_RST are
 *   self-clearing, hence the wait.
 *
 *   The placement matters as much as the reset: it must happen *before* the
 *   descriptor chain is armed. Doing it in sendcmd() instead -- which is where
 *   it used to be -- lands between DMARECVSETUP arming the engine and the
 *   command going out, and discards the armed state (see the note there).
 *
 ****************************************************************************/

static void esp_sdmmc_reset_data_path(struct esp_sdmmc_dev_s *priv)
{
  clock_t start = clock_systime_ticks();

  /* Bring the FIFO's receive watermark down to something a transfer can reach.
   *
   * The internal DMA is asked to move received data once the FIFO count passes
   * the receive watermark in FIFOTH -- and this board comes up with
   * FIFOTH = 0x01ff0000, i.e. rx_wmark = 511 bytes, nearly the whole 512-byte
   * FIFO.  Nothing on this link transfers that much in one go, so the IDMAC is
   * never asked to fetch the descriptor.  Measured on a one-byte CMD53 read:
   *
   *   status=0003a900 (FIFO_COUNT = 1, data state machine idle)
   *   fifoth=01ff0000  idsts=00002000 (FSM suspended)  des0=8000001c (OWN set)
   *
   * and the byte sitting in the FIFO reads back as 0x3c -- the register value
   * that was asked for.  No DATA_OVER follows, because BYTCNT only reaches zero
   * once the data has actually been moved to memory.
   *
   * That is the last piece of "the companion sent nothing" this driver had to
   * disprove, and the fix is one field.  It is written here, at the top of every
   * transfer, rather than once at initialisation: the clock path resets the
   * controller afterwards and the reset puts FIFOTH back to its default, which
   * is how a write placed there was measured to have no effect.
   *
   * tx_wmark is left at zero, which is both the reset default and the value the
   * working reference runs with.
   */

  priv->hw->fifoth.rx_wmark = 0;
  priv->hw->fifoth.tx_wmark = 0;

  /* Reset the IDMAC engine, then re-enable it.
   *
   * The interface resets below (CTRL.fifo_reset / CTRL.dma_reset) put the FIFO
   * and the DMA interface back, but they do not move the IDMAC's own state
   * machine: measured with data waiting in the FIFO, IDSTS keeps reporting
   * FSM = 1 (DMA_SUSPEND) with no DU and no fatal bus error, and writing poll
   * demand does not bring it out. BMOD.SWR is the register the databook gives
   * for exactly this -- "reset the DMA controller's internal registers and
   * state machine" -- and it is what sdmmc_ll_init_dma() uses at
   * initialisation; doing the same per transfer, while nothing is armed, cannot
   * discard a transfer that has not started yet.
   */

  {
    clock_t sstart = clock_systime_ticks();

    priv->hw->bmod.val = 0;
    priv->hw->bmod.sw_reset = 1;

    while (priv->hw->bmod.sw_reset != 0 &&
           (clock_systime_ticks() - sstart) <= SDMMC_CMDTIMEOUT)
      {
      }

    sdmmc_ll_enable_dma(priv->hw, true);
  }

  sdmmc_ll_reset_fifo(priv->hw);
  sdmmc_ll_reset_dma(priv->hw);

  while ((!sdmmc_ll_is_fifo_reset_done(priv->hw) ||
          !sdmmc_ll_is_dma_reset_done(priv->hw)) &&
         (clock_systime_ticks() - start) <= SDMMC_CMDTIMEOUT)
    {
    }
}

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

      /* Match ESP-IDF's descriptor bits exactly.
       *
       * Reference (sdmmc_host.c, fill_dma_descriptors):
       *
       *     desc->last_descriptor        = last;
       *     desc->second_address_chained = 1;        // always
       *     desc->owned_by_idmac         = 1;
       *
       * Three differences mattered. The chained bit was set only for chains
       * longer than one descriptor; disable_int_on_completion was set, which
       * the reference never does; and end_of_ring was set on the last
       * descriptor -- a bit describing a ring this driver does not build.
       *
       * Measured with those bits: des0=8000002e (OWN still set, so the engine
       * never took the descriptor) with IDSTS reporting the engine suspended.
       * Aligning the bits is the smallest change towards a chain the engine is
       * known to accept.
       */

      priv->desc[index].des0   = SDMMC_DESC_OWN | SDMMC_DESC_CH;

      /* The IDMAC moves whole 32-bit words, and the reference rounds this field
       * up accordingly:
       *
       *     sdmmc_host.c: desc->buffer1_size = (size_to_fill + 3) & (~3);
       *
       * A descriptor whose size is not a multiple of four was passing straight
       * through here, and the one-byte CMD53 read -- the hardest failure of the
       * size sweep, SDIOWAIT_ERROR rather than a timeout -- is exactly the case
       * that produces it.  The caller's buffer is at least this large because
       * every transfer here is word-aligned, but the extra bytes are read from
       * the card and discarded, so the round-up is safe.
       */

      priv->desc[index].des1   = (chunk + 3u) & ~3u;
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

  /* CH is already set on every descriptor; only the link pointer is needed. */

  if (index > 1)
    {
      priv->desc[0].next = (uint32_t)(uintptr_t)&priv->desc[1];
    }

  /* Only the last descriptor is marked; end_of_ring is deliberately NOT set,
   * because it describes a ring this driver does not build and the reference
   * never sets it either.
   */

  priv->desc[index - 1].des0 |= SDMMC_DESC_LD;

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
  static int n;

  /* Which setup does NuttX call?
   *
   * SDIO_CAPS_DMASUPPORTED is advertised, so the data path should be armed by
   * esp_sdmmc_dmarecvsetup().  This stub is the non-DMA entry point; if it is
   * the one being called, no transfer is ever armed and a read waits forever --
   * which is exactly the symptom being chased.  Logging here (and in
   * dmarecvsetup) settles it.
   */

  if (n < 4)
    {
      sdmmc_trace( "esp_sdmmc: recvsetup(stub) called nbytes=%u\n",
             (unsigned)nbytes);
      n++;
    }

  UNUSED(dev);
  UNUSED(buffer);
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

static void esp_sdmmc_dump_state(FAR struct esp_sdmmc_dev_s *priv,
                                 FAR const char *where);

static int esp_sdmmc_dmarecvsetup(struct sdio_dev_s *dev, uint8_t *buffer,
                                  size_t buflen)
{
  struct esp_sdmmc_dev_s *priv = (struct esp_sdmmc_dev_s *)dev;
  int ret;
  static int dumps;

  /* Every receive goes through here, so this is where the "working" and
   * "hanging" moments can be compared from (appendix BW.3).
   */

  if (dumps < 6)
    {
      esp_sdmmc_dump_state(priv, dumps == 0 ? "first-recv" : "recv");
      dumps++;
    }

  /* Reset before arming, never after: a previous transfer may have left the
   * data path busy (see esp_sdmmc_reset_data_path()).
   */

  esp_sdmmc_reset_data_path(priv);

  g_pio_rxbuf  = NULL;
  g_pio_rxlen  = 0;
  g_pio_rxdone = 0;

  if (buflen <= ESP_SDMMC_PIO_MAX)
    {
      /* Keep the engine off the FIFO for a CPU-moved transfer.
       *
       * FIFOTH.rx_wmark is programmed to 0 on every reset (and by
       * esp_sdmmc_reset_data_path()) so that the IDMAC fetches as soon as data
       * arrives -- which is what a DMA transfer wants and exactly what a
       * CPU-moved transfer must avoid: with the engine also reading the FIFO,
       * the CPU's reads hand back the same word over and over.  Measured on a
       * 512-byte payload read, the caller received "03 00 1d 01" four times
       * while the companion had sent a whole frame (appendix BY.11).  A
       * watermark larger than any PIO transfer keeps the engine suspended, which
       * is what this path relies on.
       */

      priv->hw->fifoth.rx_wmark = 512;

      /* Move the data with the CPU, with everything else armed exactly as the
       * DMA path arms it.
       *
       * Both halves of that were measured. Taking the CIU out of internal-DMA
       * mode (CTRL.use_internal_dma = 0) stops the FIFO filling at all -- the
       * earlier note in this file already recorded "with DMA disabled neither
       * RXDR nor TXDR ever appeared" -- while leaving the descriptors unarmed
       * on top of it makes the data phase never complete (every read came back
       * -110 with STATUS.DATA_STATE_MC_BUSY still set). With the chain armed as
       * usual and the engine stuck where it always is, the data phase *does*
       * complete (DATA_STATE_MC_BUSY = 0) and the received bytes sit in the
       * FIFO holding the right values.
       *
       * So: arm the descriptor chain as before, remember where the bytes go,
       * and let esp_sdmmc_eventwait() empty the FIFO as soon as the data state
       * machine says the phase is over. The engine never takes anything.
       */

      g_pio_rxbuf = buffer;
      g_pio_rxlen = buflen;
    }
  else
    {
      g_pio_rxbuf = NULL;
      g_pio_rxlen = 0;
    }

  sdmmc_ll_enable_dma(priv->hw, true);

  ret = esp_sdmmc_setup_desc(priv, (uintptr_t)buffer, buflen, false);
  if (ret < 0)
    {
      return ret;
    }

  /* Arm the engine, then WAIT for its software reset to finish.
   *
   * sdmmc_ll_init_dma() sets BMOD.SWR, which is self-clearing and completes
   * asynchronously. Programming the descriptor address while that reset is
   * still in progress loses the write, and the engine then transfers nothing:
   * every read came back -110 with DATA_STATE_MC_BUSY still set in STATUS and
   * the buffer untouched. The driver already waits for this bit in the reset
   * paths, so waiting here is consistent with how the block is used elsewhere.
   */

  /* No DMA software reset here.
   *
   * ESP-IDF calls sdmmc_ll_init_dma() exactly once, when the host is
   * initialised, and never again per transfer; this driver was resetting the
   * engine inside every setup. A reset leaves the engine suspended, and the
   * measured state is precisely that -- IDSTS FSM = 1 with the descriptor's OWN
   * bit still set -- however the chain is built and however often the engine is
   * kicked. The initialisation path already calls sdmmc_ll_init_dma(), so
   * transfers now start from an engine that was armed once, like the reference.
   */

  sdmmc_ll_set_desc_addr(priv->hw, (uint32_t)(uintptr_t)priv->desc);
  sdmmc_ll_enable_dma(priv->hw, true);

  /* Kick the engine so it fetches the descriptor list.
   *
   * PLDMND is the "poll demand" the block offers for exactly this: telling the
   * internal DMA to go and look at DBADDR. Without a kick the engine can sit
   * idle while the CIU waits for data that never comes -- STATUS keeps
   * DATA_STATE_MC_BUSY set, FIFO_COUNT stays zero and the transfer ends in the
   * data timeout with the buffer untouched.
   */

  sdmmc_ll_poll_demand(priv->hw);

  /* Keep the internal DMA armed.
   *
   * Switching it off to fall back on the FIFO was measured once: with DMA
   * disabled neither RXDR nor TXDR ever appeared, i.e. the FIFO never received
   * a single byte. That rules out "the IDMAC is the only fault" -- bypassing it
   * moves no data either -- so there is no reason to leave transfers on a path
   * that is measurably worse. The diagnostic that proved it stays in eventwait.
   */

  return OK;
}

static int esp_sdmmc_dmasendsetup(struct sdio_dev_s *dev,
                                  const uint8_t *buffer, size_t buflen)
{
  struct esp_sdmmc_dev_s *priv = (struct esp_sdmmc_dev_s *)dev;
  int ret;

  /* Same rule as the receive path: reset first, then arm. */

  esp_sdmmc_reset_data_path(priv);

  g_pio_txbuf  = NULL;
  g_pio_txlen  = 0;
  g_pio_txdone = 0;

  if (buflen <= ESP_SDMMC_PIO_MAX)
    {
      /* Programmed I/O transmit: the same arrangement as the receive path --
       * the CIU stays in internal-DMA mode, the engine is left unarmed, and the
       * CPU fills the FIFO in esp_sdmmc_eventwait().
       */

      priv->desc[0].des0 = 0;
      priv->desc[1].des0 = 0;
      up_clean_dcache((uintptr_t)priv->desc, (uintptr_t)(priv->desc + 2));

      sdmmc_ll_enable_dma(priv->hw, true);

      g_pio_txbuf = buffer;
      g_pio_txlen = buflen;

      return OK;
    }

  sdmmc_ll_enable_dma(priv->hw, true);

  ret = esp_sdmmc_setup_desc(priv, (uintptr_t)buffer, buflen, true);
  if (ret < 0)
    {
      return ret;
    }

  /* Arm the engine, then WAIT for its software reset to finish.
   *
   * sdmmc_ll_init_dma() sets BMOD.SWR, which is self-clearing and completes
   * asynchronously. Programming the descriptor address while that reset is
   * still in progress loses the write, and the engine then transfers nothing:
   * every read came back -110 with DATA_STATE_MC_BUSY still set in STATUS and
   * the buffer untouched. The driver already waits for this bit in the reset
   * paths, so waiting here is consistent with how the block is used elsewhere.
   */

  /* No DMA software reset here.
   *
   * ESP-IDF calls sdmmc_ll_init_dma() exactly once, when the host is
   * initialised, and never again per transfer; this driver was resetting the
   * engine inside every setup. A reset leaves the engine suspended, and the
   * measured state is precisely that -- IDSTS FSM = 1 with the descriptor's OWN
   * bit still set -- however the chain is built and however often the engine is
   * kicked. The initialisation path already calls sdmmc_ll_init_dma(), so
   * transfers now start from an engine that was armed once, like the reference.
   */

  sdmmc_ll_set_desc_addr(priv->hw, (uint32_t)(uintptr_t)priv->desc);
  sdmmc_ll_enable_dma(priv->hw, true);

  /* Kick the engine so it fetches the descriptor list.
   *
   * PLDMND is the "poll demand" the block offers for exactly this: telling the
   * internal DMA to go and look at DBADDR. Without a kick the engine can sit
   * idle while the CIU waits for data that never comes -- STATUS keeps
   * DATA_STATE_MC_BUSY set, FIFO_COUNT stays zero and the transfer ends in the
   * data timeout with the buffer untouched.
   */

  sdmmc_ll_poll_demand(priv->hw);

  return OK;
}
#endif

/****************************************************************************
 * Name: esp_sdmmc_waitresponse / esp_sdmmc_recv_*
 ****************************************************************************/

static int esp_sdmmc_waitresponse(struct sdio_dev_s *dev, uint32_t cmd)
{
  sdio_eventset_t wkupevent;
  static int waits;
  struct esp_sdmmc_dev_s *priv = (struct esp_sdmmc_dev_s *)dev;

  /* Trace the command whose response is being waited for.
   *
   * The hang happens before any data setup runs (appendix BW.4), so the
   * question is which command is being waited on and what the controller shows
   * while it waits.  Capped, so the console stays readable.
   */

  if (waits < 24)
    {
      sdmmc_trace(
             "esp_sdmmc: waitresponse enter cmd=%02u rintsts=%08" PRIx32
             " ctrl=%08" PRIx32 " status=%08" PRIx32 "\n",
             (unsigned)((cmd & MMCSD_CMDIDX_MASK) >> MMCSD_CMDIDX_SHIFT),
             priv->hw->rintsts.val, priv->hw->ctrl.val, priv->hw->status.val);
      waits++;
    }

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
      sdmmc_trace( "esp_sdmmc: waitresponse -> OK (no response phase)\n");
      if (waits < 24)
        {
          sdmmc_trace(
                 "esp_sdmmc: waitresponse out ret=%d xfrmask=%02x"
                 " rintsts=%08x ctrl=%08x\n",
                 (int)(OK), (unsigned)priv->xfrmask,
                 (unsigned)priv->hw->rintsts.val,
                 (unsigned)priv->hw->ctrl.val);
        }

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
        /* The ISR now runs, and it clears RINTSTS before this loop can see it.
         *
         * So check the event mask the ISR fills first: it records that the
         * command completed even though the hardware bit is already gone.
         * The matching clear happens in sendcmd(), before the command is
         * issued -- clearing here would discard an early completion.
         */

        if ((p->xfrmask & (SDIOWAIT_CMDDONE | SDIOWAIT_RESPONSEDONE)) != 0)
          {
            /* The ISR already recorded the completion.
             *
             * This must set the same flag the polling path does: every
             * completion now arrives here (the handler clears RINTSTS before
             * this loop can see the hardware bit), and leaving 'done' false
             * made the caller report -ETIMEDOUT for commands that had in fact
             * completed -- the measured "xfrmask=03 but no command completion".
             */

            done = true;
            break;
          }

        /* Read the RAW status, not the masked one.
         *
         * sdmmc_ll_get_intr_status() returns MINTSTS -- raw AND the enable mask
         * -- so it reads zero whenever the source happens to be masked, and that
         * is how a command the card had already answered was reported as "no
         * command completion": the error line prints the raw register and showed
         * 00000004 while this check saw nothing (appendix BY.6).  Completion is
         * a fact about the wire, not about our mask.
         */

        if ((*(volatile uint32_t *)&hw->rintsts & SDMMC_CMD_DONE) != 0)
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

    sdmmc_trace(
           "esp_sdmmc: waitresponse exit xfrmask=%02x waitmask=%02x\n",
           (unsigned)p->xfrmask, (unsigned)p->waitmask);

    sdmmc_trace(
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
        sdmmc_trace( "esp_sdmmc: waitresponse -> -ETIMEDOUT\n");
      if (waits < 24)
        {
          sdmmc_trace(
                 "esp_sdmmc: waitresponse out ret=%d xfrmask=%02x"
                 " rintsts=%08x ctrl=%08x\n",
                 (int)(-ETIMEDOUT), (unsigned)priv->xfrmask,
                 (unsigned)priv->hw->rintsts.val,
                 (unsigned)priv->hw->ctrl.val);
        }

        return -ETIMEDOUT;
      }

    if ((*(volatile uint32_t *)&hw->rintsts & SDMMC_ERR_RE) != 0)
      {
        syslog(LOG_ERR, "esp_sdmmc: response error (rintsts=%08" PRIx32 ")\n",
               *(volatile uint32_t *)&hw->rintsts);
        sdmmc_trace( "esp_sdmmc: waitresponse -> -EIO (RE set)\n");
      if (waits < 24)
        {
          sdmmc_trace(
                 "esp_sdmmc: waitresponse out ret=%d xfrmask=%02x"
                 " rintsts=%08x ctrl=%08x\n",
                 (int)(-EIO), (unsigned)priv->xfrmask,
                 (unsigned)priv->hw->rintsts.val,
                 (unsigned)priv->hw->ctrl.val);
        }

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

  sdmmc_trace( "esp_sdmmc: waitresponse -> OK\n");
      if (waits < 24)
        {
          sdmmc_trace(
                 "esp_sdmmc: waitresponse out ret=%d xfrmask=%02x"
                 " rintsts=%08x ctrl=%08x\n",
                 (int)(OK), (unsigned)priv->xfrmask,
                 (unsigned)priv->hw->rintsts.val,
                 (unsigned)priv->hw->ctrl.val);
        }

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

  sdmmc_trace( "esp_sdmmc: R%u raw=%08" PRIx32 " (cmd=%08" PRIx32 ")\n",
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

  sdmmc_trace( "esp_sdmmc: short response raw=%08" PRIx32 " (cmd=%08" PRIx32 ")\n",
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

  /* Start every wait from a clean slate.
   *
   * This is the point where a transfer is armed: NuttX calls it for a data
   * command after blocksetup() and before the command goes out. Anything the
   * previous command left pending has to go now, or the next wait is decided by
   * the last one's leftovers.
   *
   * Measured, and it is what made "only the first transfer after bring-up
   * works" look like a link problem: sdio.c issues a CMD52 "ABRT" after each
   * CMD53 read (SD_ACMD52ABRT -- response not looked at), the controller
   * reports a response timeout for it (RINTSTS bit 8, RTO, seen as
   * "ISR error raw=00000100"), and the interrupt handler turns that into
   * SDIOWAIT_ERROR in priv->xfrmask. That bit is sticky, so the *next*
   * transfer's eventwait saw an error before it had even started and returned
   * -EIO immediately -- for every transfer from then on, while the first
   * ladder (run before any ABRT) worked perfectly.
   */

  sdmmc_ll_clear_interrupt(priv->hw, 0xffffffff);
  priv->xfrmask = 0;
  g_last_error_bits = 0;

  /* Re-arm HLE if the handler masked it (see g_hle_masked).
   *
   * Arming a transfer is the natural place: the previous transfer is over, so a
   * lock-up that belonged to it may be gone, and if it is not, the cost is one
   * interrupt for this transfer instead of a storm that starves the core.
   */

  if (g_hle_masked)
    {
      priv->hw->intmask.val |= SDMMC_ERR_HLE;
      g_hle_masked = false;
    }

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
    int          dwait_report = 0;
    int          pio_settle = 0;

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
        uint32_t st = *(volatile uint32_t *)&hw->rintsts;

        /* Move the data by hand while the data phase runs.
         *
         * This is the programmed-I/O path described above the PIO state: the
         * CIU is not in internal-DMA mode for this transfer, so the receive
         * FIFO holds the bytes until the CPU takes them and the transmit FIFO
         * needs filling or the card gets nothing. Both directions are serviced
         * here because this loop is already the one place that runs while a
         * transfer is in flight.
         */

        if (g_pio_rxbuf != NULL)
          {
            uint32_t sts = *(volatile uint32_t *)&hw->status;
            unsigned bytes = (unsigned)((sts >> 17) & 0x1fff);
            bool     idle  = ((sts >> 10) & 1u) == 0;

            /* Only believe the FIFO once the whole transfer is in it (or the
             * CIU says the block is over).
             *
             * Waiting a fixed couple of milliseconds was not enough: after the
             * command there is a stretch where the data state machine is idle
             * because the data phase has not begun yet -- at 50 kHz the command
             * plus response alone take about two milliseconds -- so the drain
             * fired early and read a word whose invalid bytes repeat the first
             * one (measured: a four-byte read of FN1 0x60 returned
             * f4 f4 f4 f4, and an eight-byte one f4 0a f4 0a, where the bytes
             * in the FIFO were f4 0a 00 00 ...).
             *
             * A transfer of up to one FIFO is completely inside it when the
             * block is over, so the condition is simply "the count reached the
             * length that was asked for".
             */

            /* Require the state machine to have been idle for a while before
             * believing the FIFO.
             *
             * Reading it the moment DATA_STATE_MC_BUSY first reads 0 handed
             * back a word that was the same byte repeated (0xf4f4f4f4 for a
             * four-byte read, 0x0af40af4 for eight), while the same port read
             * after the transfer had settled holds the bytes that were actually
             * in the register (0x0000003c, and PACKET_LEN's four bytes). The
             * FIFO is being published by the CIU at the end of the block, so
             * give it a couple of milliseconds of quiet first -- the loop polls
             * every 20 us.
             */

            /* Require the FSM to have been quiet for a while, and USE it.
             *
             * pio_settle was incremented here all along and then never read, so
             * the drain below fired on the first poll where the FSM happened to
             * look idle and the FIFO had bytes -- which for a block transfer is
             * *during* the phase.  The FIFO port then hands back the same word
             * over and over: measured on a 512-byte payload read, the caller
             * received "03 00 1d 01" four times where the companion had sent a
             * whole frame (the analyzer shows the frame on the wire), and every
             * packet was dropped by the stack (appendix BY.11).
             *
             * 50 polls at the 20 us cadence below is about a millisecond of
             * quiet: long enough for the CIU to publish a completed block, short
             * enough to be invisible next to a transfer.
             */

            pio_settle = idle ? pio_settle + 1 : 0;

            /* Drain once the CIU says the block is over and has actually
             * published bytes into the FIFO. Both halves matter: draining
             * on "the state machine is idle" alone is too early (the data
             * phase has not begun yet) and the FIFO hands back a word whose
             * unused bytes repeat the first one -- measured as
             * f4 f4 f4 f4 for a four-byte read of FN1 0x60 where the
             * register holds f4 0a 60 2f.
             */

            /* NOTE: accepting the transfer on "the FIFO holds enough bytes"
             * instead of waiting for DATA_OVER was tried and reverted.
             *
             * It made every step of the sweep report ret=0, but all of them
             * came back with the same 5a 5a 5a 5a -- the pattern the buffer
             * holds when nothing was drained -- so it manufactured successes
             * rather than transferring data.  The real observation to chase is
             * the one below: bytes do reach the FIFO (0x0000003c for a one-byte
             * read of FN1 0x60) while DATA_OVER never arrives.
             */

            /* For a CPU-moved transfer the state machine going idle *is* the
             * end of the data phase: the engine never takes anything, so
             * DATA_OVER -- which belongs to the internal DMA path -- is not what
             * says the bytes are in.  This is the rule the note further up
             * describes as measured-working ("the data phase does complete ...
             * and the received bytes sit in the FIFO holding the right values");
             * requiring DATA_OVER on top of it is what turned reads into
             * timeouts with the card's data already sitting in the FIFO
             * (appendix BT: "timeout with 1 byte(s) still in the FIFO:
             * first=0x0000003c").
             *
             * The quiet-period counter above is what keeps this from firing
             * before the phase starts.
             */

            idle = ((st & SDMMC_DATA_OVER) != 0 || g_pio_rxbuf != NULL) &&
                   bytes > 0 && idle && pio_settle >= 50;

            /* Empty the FIFO once the CIU says the data phase is over.
             *
             * Reading it *while* the phase runs was measured to hand back a
             * word that is the same byte repeated (0xf4f4f4f4 for a four-byte
             * read of FN1 0x60), where the same FIFO read after the transfer
             * holds the real bytes (0x0000003c for a one-byte read, and the
             * four bytes of PACKET_LEN for a four-byte one). The CIU publishes
             * the received block to the FIFO when the block is complete, so the
             * drain belongs here rather than in the polling path.
             *
             * FIFO_COUNT is in bytes, and a transfer of up to one FIFO is
             * drained in one go -- which covers every transfer this driver
             * makes; anything larger would need the loop below to run during
             * the phase as well, which is why the DMA path is kept for it.
             */

            /* Drain on the LIVE FIFO count, one word per published word.
             *
             * Taking FIFO_COUNT once and then reading that many words assumes
             * the FIFO has already published all of them.  When it has not, the
             * reads come back with the last word repeated: measured on a
             * 512-byte payload read, the caller received "03 00 1d 01" four
             * times where the companion had sent a frame, and the transport
             * dropped every packet with "offset[3] != exp[12]" (appendix
             * BY.10).  Re-reading the count per word stops as soon as the FIFO
             * has nothing new, and the next poll picks up where this one left
             * off.
             */

            while (idle && g_pio_rxdone < g_pio_rxlen)
              {
                uint32_t qsts   = *(volatile uint32_t *)&hw->status;
                unsigned qbytes = (unsigned)((qsts >> 17) & 0x1fff);
                uint32_t word;
                size_t   left;

                if (qbytes == 0)
                  {
                    break;
                  }

                word = *(volatile uint32_t *)((uintptr_t)hw + ESP_SDMMC_FIFO_PORT);
                left = g_pio_rxlen - g_pio_rxdone;

                if (left > 4)
                  {
                    left = 4;
                  }

                if (left > qbytes)
                  {
                    left = qbytes;
                  }

                memcpy(g_pio_rxbuf + g_pio_rxdone, &word, left);
                g_pio_rxdone += left;
              }

            if (g_pio_rxdone >= g_pio_rxlen)
              {
                priv->xfrmask |= SDIOWAIT_TRANSFERDONE;
              }
          }

        if (g_pio_txbuf != NULL && g_pio_txdone < g_pio_txlen)
          {
            uint32_t sts = *(volatile uint32_t *)&hw->status;

            /* FIFO_FULL is bit 3; stop when it says so rather than writing into
             * a full FIFO and losing bytes.
             */

            while ((sts & (1u << 3)) == 0 && g_pio_txdone < g_pio_txlen)
              {
                uint32_t word = 0;
                size_t   left = g_pio_txlen - g_pio_txdone;

                if (left > 4)
                  {
                    left = 4;
                  }

                memcpy(&word, g_pio_txbuf + g_pio_txdone, left);
                *(volatile uint32_t *)((uintptr_t)hw + ESP_SDMMC_FIFO_PORT) =
                  word;
                g_pio_txdone += left;
                sts = *(volatile uint32_t *)&hw->status;
              }
          }

        /* Does the FIFO ever see data?
         *
         * With the internal DMA switched off, RXDR is the CIU saying "there are
         * bytes in the receive FIFO", and TXDR the same for transmit. Either
         * one proves the card is actually driving the bus; neither appearing
         * means it never sent anything. Report it once per transfer.
         */

        if ((st & (SDMMC_RXDR | SDMMC_TXDR)) != 0)
          {
            static int fifo_report;

            if (fifo_report < 6)
              {
                fifo_report++;
                syslog(LOG_ERR,
                       "esp_sdmmc: FIFO has data: raw=%08" PRIx32 " rxdr=%u"
                       " txdr=%u status=%08" PRIx32 "\n",
                       st, (unsigned)((st >> 5) & 1), (unsigned)((st >> 4) & 1),
                       *(volatile uint32_t *)&hw->status);
              }
          }

        /* Trace a data transfer while it is in flight.
         *
         * "The card sent nothing" and "the card sent data the CIU never
         * accepted" look identical from the outside -- both end as a timeout
         * with an empty buffer -- but the controller separates them: the first
         * leaves the data state machine idle, the second leaves it busy with
         * FIFO_COUNT moving. Sampling STATUS/IDSTS/DES0 while the transfer is
         * in flight is what tells the two apart, and it is the one observation
         * the earlier rounds made only *after* the transfer had failed.
         *
         * One line every ~50 ms, at most 8 per transfer, so a 5 s timeout
         * produces a compact timeline instead of a flood.
         */

        /* Resume the engine when data is waiting for it.
         *
         * The IDMAC fetches descriptors when the transfer asks it to, and
         * ESP-IDF resumes it from its interrupt handler whenever it has stopped
         * with work outstanding (sdmmc_host_dma_resume() -> PLDMND, called on
         * NI).  This driver has no IDMAC handler at all, so the engine is never
         * resumed and a byte that reached the FIFO stays there: measured as
         * FIFO_COUNT = 1, DATA_STATE_MC_BUSY = 0, IDSTS FSM suspended, the
         * descriptor's OWN bit still set, and no DATA_OVER -- because BYTCNT
         * only reaches zero once the data has actually been moved to memory.
         *
         * The condition is exactly "there is data and the engine still owns the
         * descriptor", so this is a resume and not a blind kick; repeating it
         * is harmless because PLDMND is a one-shot request the engine consumes.
         */

        if ((priv->waitmask & SDIOWAIT_TRANSFERDONE) != 0)
          {
            uint32_t sts = *(volatile uint32_t *)&hw->status;

            if (((sts >> 17) & 0x1fff) != 0 &&
                (priv->desc[0].des0 & SDMMC_DESC_OWN) != 0)
              {
                sdmmc_ll_poll_demand(hw);
              }
          }

        if ((priv->waitmask & SDIOWAIT_TRANSFERDONE) != 0)
          {
            if ((clock_systime_ticks() - start) >=
                (dwait_report + 1) * (50 / MSEC_PER_TICK) && dwait_report < 8)
              {
                uint32_t sts = *(volatile uint32_t *)&hw->status;

                dwait_report++;
                syslog(LOG_ERR,
                       "esp_sdmmc: dwait[%d] t=%ums status=%08" PRIx32
                       " fifo=%u datbusy=%u mc=%u idsts=%08" PRIx32
                       " des0=%08" PRIx32 " ctrl=%08" PRIx32 "\n",
                       dwait_report,
                       (unsigned)((clock_systime_ticks() - start) *
                                  MSEC_PER_TICK),
                       sts, (unsigned)((sts >> 17) & 0x1fff),
                       (unsigned)((sts >> 9) & 1), (unsigned)((sts >> 10) & 1),
                       priv->hw->idsts.val, priv->desc[0].des0,
                       priv->hw->ctrl.val);
              }
          }

        if (st & SDMMC_DATA_OVER)
          {
            sdmmc_ll_clear_interrupt(hw, SDMMC_DATA_OVER);

            /* DATA_OVER says the wire phase is over.  It does not say the bytes
             * reached the caller's buffer.
             *
             * The drain above runs on one FIFO_COUNT sample per pass, so a
             * transfer longer than whatever was in the FIFO at that instant was
             * completed with only its first word copied: measured, a twenty-byte
             * register read delivered four bytes and left the rest untouched,
             * and a 512-byte payload read came back as "ret 0, first word
             * 00000000" (appendix BY.5).  Worse, the CIU was then left with its
             * data state machine still busy -- after that, no command completed
             * at all, not even the controller-internal clock update, so the
             * transport could never start.
             *
             * Finish the drain here, bounded, before reporting success: take
             * what the FIFO still holds, and give the CIU a bounded number of
             * empty polls for the remaining words before giving up.
             */

            if (g_pio_rxbuf != NULL)
              {
                unsigned idle_polls = 0;
                unsigned limit      = 20000;

                while (g_pio_rxdone < g_pio_rxlen && limit-- > 0)
                  {
                    uint32_t sts  = *(volatile uint32_t *)&hw->status;
                    unsigned have = (unsigned)((sts >> 17) & 0x1fff);

                    if (have == 0)
                      {
                        if (++idle_polls > 200)
                          {
                            /* The FIFO stopped filling: whatever arrived is in
                             * the buffer now.  Saying so is better than
                             * spinning, and the caller's timeout still covers a
                             * transfer that never completes at all.
                             */

                            break;
                          }

                        continue;
                      }

                    idle_polls = 0;

                    {
                      uint32_t word =
                        *(volatile uint32_t *)((uintptr_t)hw +
                                               ESP_SDMMC_FIFO_PORT);
                      size_t left = g_pio_rxlen - g_pio_rxdone;

                      if (left > 4)
                        {
                          left = 4;
                        }

                      if (left > have)
                        {
                          left = have;
                        }

                      memcpy(g_pio_rxbuf + g_pio_rxdone, &word, left);
                      g_pio_rxdone += left;
                    }
                  }
              }

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
            g_last_error_bits = st & SDMMC_ALL_ERRORS;
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

        /* A CPU-moved transfer completes when the data state machine goes idle
         * with the bytes in the FIFO -- the same reasoning as the drain below,
         * and the reason a byte-mode CMD53 read used to hang: its completion
         * event comes from DATA_OVER, which a PIO transfer never raises
         * (measured: eh_host_port_sdio_read_reg() entered and never returned).
         */

        if (g_pio_rxbuf != NULL && (priv->waitmask & SDIOWAIT_TRANSFERDONE) != 0)
          {
            /* Only when every byte the caller asked for is in the buffer.
             *
             * Completing on "the FSM is idle and something was in the FIFO" is
             * how a partially drained transfer used to be reported as a
             * success, leaving the caller with stale bytes (appendix BY.11).
             */

            if (g_pio_rxdone >= g_pio_rxlen)
              {
                priv->xfrmask |= SDIOWAIT_TRANSFERDONE;
              }
          }

        if ((priv->xfrmask & priv->waitmask) != 0)
          {
            break;
          }

        if (clock_systime_ticks() - start > SDMMC_LONGTIMEOUT)
          {
            /* Data left in the FIFO at the timeout is the one case where the
             * controller has proof the card sent something. Read it out: the
             * value is the answer to "did the CIU receive the right bytes",
             * and the FIFO is about to be reset anyway. FIFOTH says whether a
             * watermark is what kept the engine from draining it.
             */

            uint32_t sts = *(volatile uint32_t *)&hw->status;
            unsigned cnt = (unsigned)((sts >> 17) & 0x1fff);

            if (cnt != 0)
              {
                /* The FIFO data port is at offset 0x200 and is not part of the
                 * register struct this HAL exposes, hence the raw access.
                 */

                syslog(LOG_ERR,
                       "esp_sdmmc: timeout with %u byte(s) still in the FIFO: "
                       "first=0x%08" PRIx32 " fifoth=0x%08" PRIx32
                       " idsts=%08" PRIx32 " des0=%08" PRIx32 "\n",
                       cnt, *(volatile uint32_t *)((uintptr_t)hw + 0x200),
                       *(volatile uint32_t *)&hw->fifoth,
                       priv->hw->idsts.val, priv->desc[0].des0);
              }

            priv->xfrmask |= SDIOWAIT_TIMEOUT;

            /* Put the data path back before returning.
             *
             * A timed-out transfer leaves the data state machine busy with an
             * empty FIFO and an armed descriptor, and it does not return to
             * idle on its own -- "every later transfer inherits the failure",
             * as the note on esp_sdmmc_reset_data_path() says.  That note is
             * about doing the reset *before* a transfer; measured, the failed
             * state has to be cleared *after* the failure as well, or the very
             * next command stalls identically (the transport's four-byte
             * register reads: dwait status=0001ad06 fifo=0 mc=1
             * idsts=DMA_SUSPEND des0=OWN, appendix BY.8).  With the path reset
             * here the transport's own retry gets a working controller back
             * instead of inheriting a dead one.
             */

#if defined(CONFIG_ESPRESSIF_SDMMC_DMA) && defined(CONFIG_SDIO_DMA)
            esp_sdmmc_reset_data_path(priv);
#endif
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

  /* Do not leave the data state machine busy for the next transfer.
   *
   * A transfer can be reported as done (DATA_OVER, or the PIO drain gate) while
   * the CIU's data state machine has not returned to idle yet.  The next command
   * then finds it busy and its data phase never starts: measured in the
   * transport, the first twenty-byte register read succeeded and every read
   * after it came back with -ETIMEDOUT while "dwait status=0001ad06 fifo=0
   * mc=1 idsts=DMA_SUSPEND des0=OWN" showed the machine stuck (appendix BY.9).
   *
   * So wait, bounded, for idle -- and if it never comes, put the data path back
   * (the FIFO, the IDMAC and the state machine) instead of handing a dead
   * controller to the next transfer.
   */

  if ((wkupevent & SDIOWAIT_TRANSFERDONE) != 0)
    {
      clock_t istart = clock_systime_ticks();

      while (((*(volatile uint32_t *)&priv->hw->status >> 10) & 1u) != 0)
        {
          if (clock_systime_ticks() - istart > MSEC2TICK(100))
            {
              syslog(LOG_ERR,
                     "esp_sdmmc: data state machine still busy 100 ms after "
                     "the transfer (status=%08" PRIx32 " idsts=%08" PRIx32
                     " des0=%08" PRIx32 ")\n",
                     *(volatile uint32_t *)&priv->hw->status,
                     priv->hw->idsts.val, priv->desc[0].des0);

#if defined(CONFIG_ESPRESSIF_SDMMC_DMA) && defined(CONFIG_SDIO_DMA)
              esp_sdmmc_reset_data_path(priv);
#endif
              break;
            }

          up_udelay(20);
        }
    }

  /* The transfer is over: drop the PIO references so the next one starts clean.
   */

  g_pio_rxbuf  = NULL;
  g_pio_rxlen  = 0;
  g_pio_rxdone = 0;
  g_pio_txbuf  = NULL;
  g_pio_txlen  = 0;
  g_pio_txdone = 0;
  priv->enablewait = false;

  /* Log what the data phase actually reported: whether the controller ever
   * raised DATA_OVER, and what the event set looks like on the way out.
   */

  sdmmc_trace(
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

  /* Count how often the handler actually runs.
   *
   * The driver was written around the belief that this never fires, so it
   * polls instead. That belief has never been measured -- and the SDIO card
   * interrupt (the companion pulling DAT1 low) only arrives here.
   */

  {
    static volatile uint32_t isr_count;

    isr_count++;
    if (isr_count <= 5)
      {
        sdmmc_trace( "esp_sdmmc: ISR #%" PRIu32 "\n", isr_count);
      }

    if (isr_count <= 12)
      {
        sdmmc_trace( "esp_sdmmc: ISR raw status=%08" PRIx32
               " intmask=%08" PRIx32 "\n",
               sdmmc_ll_get_intr_status(priv->hw),
               *(volatile uint32_t *)&priv->hw->intmask);
      }
  }

  status = sdmmc_ll_get_intr_status(priv->hw);

  /* Clear every bit that was read, after translating it below.
   *
   * A note in this file once argued the completion bits should be left for the
   * waiter; that is wrong for this driver.  The waiter does not read RINTSTS --
   * the handler translates it into priv->xfrmask events, so the handler must
   * clear what it read.  Leaving CMD_DONE set made the controller re-interrupt
   * forever: measured, a storm of "ISR raw status=00000004" (CMD_DONE) that
   * starved the system until it reset.
   */

  sdmmc_ll_clear_interrupt(priv->hw, status);

  if (status & SDMMC_CMD_DONE)
    {
      eventset |= SDIOWAIT_CMDDONE | SDIOWAIT_RESPONSEDONE;
    }

  if (status & SDMMC_DATA_OVER)
    {
      eventset |= SDIOWAIT_TRANSFERDONE;
    }

  /* The card pulled DAT1: it has something queued for the host.
   *
   * Recorded on its own rather than as a transfer event, because it belongs to
   * no single command -- it is the card asking the host to come and read, which
   * is the step esp-hosted's own host waits for before it touches a register.
   */

  if ((status & SDMMC_SDIO_INTR) != 0)
    {
      priv->cardintr = true;

      /* Mask the source now.
       *
       * DAT1 is level-triggered and cannot be cleared by writing RINTSTS, so
       * with the source enabled the controller re-interrupts for as long as the
       * companion holds the line (measured: ~800k interrupts, appendix BG).
       * Masking here means one interrupt per assertion; the wait re-enables it,
       * by which time the host has serviced the card.
       */

      priv->hw->intmask.val &= ~SDMMC_SDIO_INTR;
    }

  if (status & SDMMC_ALL_ERRORS)
    {
      /* Name the bit, and do NOT fail the transfer on it.
       *
       * This used to set SDIOWAIT_ERROR in priv->xfrmask, which is the error
       * flag the *next* eventwait inspects -- and the error bit arrives
       * asynchronously, from whatever command was in flight a moment ago. On
       * this link RINTSTS bit 8 (RTO, response timeout) is raised for command
       * after command, including CMD52s that then complete normally, so the
       * injection turned every data transfer into an immediate -EIO: the first
       * ladder after bring-up worked, and every transfer after it failed before
       * it had even started.
       *
       * An error that matters is one that belongs to the transfer being waited
       * for, and esp_sdmmc_eventwait() already polls RINTSTS for exactly that
       * (it checks SDMMC_ALL_ERRORS on every pass and reports the bits it
       * finds). So this is a diagnostic, not a verdict; it is logged and the
       * sticky bits are cleared so the next poll sees only what is new.
       */

      static int err_report;

      if (err_report < 12)
        {
          err_report++;
          syslog(LOG_ERR,
                 "esp_sdmmc: ISR error raw=%08" PRIx32
                 " [re=%u rcrc=%u dcrc=%u rto=%u drto=%u hto=%u frun=%u"
                 " hle=%u sbe=%u ebe=%u]\n",
                 status,
                 (unsigned)((status >> 1) & 1),  (unsigned)((status >> 6) & 1),
                 (unsigned)((status >> 7) & 1),  (unsigned)((status >> 8) & 1),
                 (unsigned)((status >> 9) & 1),  (unsigned)((status >> 10) & 1),
                 (unsigned)((status >> 11) & 1), (unsigned)((status >> 12) & 1),
                 (unsigned)((status >> 13) & 1), (unsigned)((status >> 15) & 1));
        }

      /* HLE is level-driven -- masking the source is the only way to stop the
       * controller re-interrupting (see g_hle_masked above).  It is re-armed by
       * esp_sdmmc_waitenable() when the next transfer is armed, so at most one
       * interrupt per arming is spent on a controller that is still locked.
       */

      if ((status & SDMMC_ERR_HLE) != 0 && !g_hle_masked)
        {
          priv->hw->intmask.val &= ~SDMMC_ERR_HLE;
          g_hle_masked = true;

          syslog(LOG_WARNING,
                 "esp_sdmmc: HLE masked (intmask=%08" PRIx32
                 ") -- controller locked; re-armed at the next transfer\n",
                 *(volatile uint32_t *)&priv->hw->intmask);
        }
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
 * Name: esp_sdmmc_wait_card_intr
 *
 * Description:
 *   Wait for the card to raise its SDIO interrupt (DAT1 pulled low).
 *
 *   esp-hosted's host blocks on this before reading anything, and it is the
 *   part of the protocol this driver never had: without it the host can only
 *   poll INT_RAW and hope to sample after the card has queued something. The
 *   flag is cleared on entry, so a caller always waits for a fresh interrupt.
 *
 * Input Parameters:
 *   dev        - The SDIO device to watch
 *   timeout_ms - How long to wait, in milliseconds
 *
 * Returned Value:
 *   true if the card signalled within the timeout, false otherwise.
 *
 ****************************************************************************/

bool esp_sdmmc_wait_card_intr(struct sdio_dev_s *dev, unsigned int timeout_ms)
{
  struct esp_sdmmc_dev_s *priv = (struct esp_sdmmc_dev_s *)dev;
  clock_t start;

  if (priv == NULL)
    {
      return false;
    }

  /* The D1 level IS the card interrupt, and it is active low.
   *
   * This is how ESP-IDF does it (sdmmc_host_io_int_wait): the CIU's SDIO
   * status bit is edge-latched and "is only set when first interrupt
   * triggered", so waiting on it alone misses every later assertion -- which is
   * exactly what the measured card-intr=0 showed. Reading the pin first, and
   * only falling back to the interrupt when it is high, is what the reference
   * relies on: if D1 is high at entry, the coming negedge is guaranteed to
   * raise the CIU interrupt.
   */

  /* The SDIO interrupt source is left masked.
   *
   * Arming it here (so the status bit would latch) was the previous design, and
   * it is the prime suspect for the one failure left: the transport's CMD53 is
   * written to the controller and never completes, while the identical command
   * completes before the stack starts -- the visible difference being that the
   * card interrupt is asserted on the bus by then and this source keeps being
   * armed around every transfer (appendix BW.22).  Waiting therefore relies on
   * the controller's status bits and the poll interval alone.
   */

  /* Ask the controller first.
   *
   * Once the slot-1 pads are routed through the GPIO matrix, the DAT1 pad
   * belongs to the SDMMC peripheral and esp_gpioread() no longer observes it --
   * measured on hardware: the wire held DAT1 asserted while this function read
   * the pad as high, so the host waited for an interrupt that had already
   * arrived.  RINTSTS keeps the raw SDIO-interrupt state regardless of the
   * interrupt mask, which is exactly the "card is asking" signal wanted here.
   */

  /* Arm the source, then look at the RAW status bit.
   *
   * The CIU only latches the SDIO status while the source is enabled (measured,
   * appendix BS.2), and the masked view (sdmmc_ll_get_intr_status) hides it even
   * then.  The pin fallback that used to stand in for this is the interrupt
   * storm of appendix BY.15: once the slot's pads are routed through the GPIO
   * matrix the DAT1 pad belongs to the SDMMC peripheral, esp_gpioread() reads it
   * as 0, and this function then answered "interrupted" on every call while
   * INT_RAW was empty -- the transport spun, the bus lock stayed busy and the
   * console was drowned.
   */

  sdmmc_ll_enable_interrupt(priv->hw, SDMMC_SDIO_INTR, true);

  if ((*(volatile uint32_t *)&priv->hw->rintsts & SDMMC_SDIO_INTR) != 0)
    {
      sdmmc_ll_clear_interrupt(priv->hw, SDMMC_SDIO_INTR);
      priv->cardintr = false;
      sdmmc_ll_enable_interrupt(priv->hw, SDMMC_SDIO_INTR, false);
      return true;
    }

  priv->cardintr = false;

  start = clock_systime_ticks();
  while (!priv->cardintr)
    {
      if ((*(volatile uint32_t *)&priv->hw->rintsts & SDMMC_SDIO_INTR) != 0)
        {
          sdmmc_ll_clear_interrupt(priv->hw, SDMMC_SDIO_INTR);
          priv->cardintr = false;
          sdmmc_ll_enable_interrupt(priv->hw, SDMMC_SDIO_INTR, false);
          return true;
        }

      if (clock_systime_ticks() - start > MSEC2TICK(timeout_ms))
        {
          /* 2026-11-05 修复：超时也必须把 SDIO 中断源屏蔽回去。
           *
           * 入口处武装这个源是必须的 —— CIU 只有在源使能时才锁存 SDIO 状态位
           * （BS.2 实测）。但原来两条返回路径都没有关闭它，于是**第一次超时之后
           * 这个源就永久武装着**；紧接着的 CMD53 恰好落在"卡中断已在总线上被拉低
           * + 源已武装"这个组合里，正是本文件记录的 "CMD53 永不完成" 的触发条件
           * （BW.22）。现场表现完全吻合：开机先是好的（传输几乎逐条成功），
           * 某刻起每个 RPC 都 3–5 s 超时、响应迟到被丢弃
           * （`request: no response ... (3000 ms)` / `rx: no waiter ... dropping`），
           * 同时 `no card interrupt within 2000 ms, polling the companion` 一直刷。
           *
           * 现在统一在返回前屏蔽。RAW 状态位不受中断屏蔽影响（RINTSTS 保持原始
           * 锁存），所以一次迟到的断言仍会被下一次调用看到，不会丢事件。
           */

          sdmmc_ll_enable_interrupt(priv->hw, SDMMC_SDIO_INTR, false);
          return false;
        }

      nxsig_usleep(1000);
    }

  priv->cardintr = false;
  sdmmc_ll_enable_interrupt(priv->hw, SDMMC_SDIO_INTR, false);
  return true;
}

/****************************************************************************
 * Name: esp_sdmmc_last_error_bits
 *
 * Description:
 *   See the header: the error bits of the most recent data transfer.
 *
 ****************************************************************************/

/****************************************************************************
 * Name: esp_sdmmc_dump_state
 *
 * Description:
 *   Print the controller registers that decide how a transfer behaves.
 *
 *   The same transfer completes from the bring-up thread and hangs once the
 *   stack has run (appendices BV.11/BW.2), so the difference has to be in these
 *   registers.  Called at both moments to compare them field by field.
 *
 ****************************************************************************/

static void esp_sdmmc_dump_state(FAR struct esp_sdmmc_dev_s *priv,
                                 FAR const char *where)
{
  sdmmc_trace(
         "esp_sdmmc: state @%s: ctrl=%08" PRIx32 " bmod=%08" PRIx32
         " idsts=%08" PRIx32 " rintsts=%08" PRIx32 " status=%08" PRIx32
         " fifoth=%08" PRIx32 " tmout=%08" PRIx32 "\n",
         where, priv->hw->ctrl.val, priv->hw->bmod.val, priv->hw->idsts.val,
         priv->hw->rintsts.val, priv->hw->status.val, priv->hw->fifoth.val,
         priv->hw->tmout.val);
}

uint32_t esp_sdmmc_last_error_bits(void)
{
  return g_last_error_bits;
}


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
   * CLK stayed at a constant level. See docs/02_驱动实现/WiFi_SDIO实现.md appendix Q.
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

  sdmmc_trace(
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

  /* The FIFO watermark is programmed per transfer instead of here.
   *
   * It has to be at the top of esp_sdmmc_reset_data_path(): the clock path runs
   * after this point and resets the controller, which restores FIFOTH's
   * default (rx_wmark = 511) and leaves the IDMAC never being asked for data.
   * See the note there.
   */

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

    sdmmc_trace(
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

    sdmmc_trace(
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
