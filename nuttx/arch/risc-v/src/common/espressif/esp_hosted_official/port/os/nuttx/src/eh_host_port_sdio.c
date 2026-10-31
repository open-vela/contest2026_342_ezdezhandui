/* SPDX-License-Identifier: Apache-2.0 */

/* SDIO port of the official esp-hosted host stack, on NuttX.
 *
 * This is the only place where the official stack touches the SDIO host: the
 * nine functions declared in port/os/include/eh_host_port_sdio.h, implemented
 * on top of this tree's SDIO device (esp_sdmmc.c) instead of ESP-IDF's
 * sdmmc_host/sdmmc_card pair.
 *
 * Why it is a port and not a copy of port/os/idf/src/eh_host_port_sdio.c: that
 * file is written against sdmmc_card_t/sdmmc_host_t, which do not exist here.
 * What it does have in common with this file is the sequence, and that is worth
 * keeping identical: the card is initialised as plain SDIO, function 1 is
 * enabled in CCCR, the block size is set to 512 on both functions, and the bus
 * is widened.  The bring-up below mirrors 附录 AK/AN, which is where the data
 * path was measured to work on this board.
 *
 * The data transfers themselves go through sdio_io_rw_direct()/extended(),
 * which is the path every working measurement in this project used -- including
 * the CPU-drained FIFO of appendix AK.3.
 */

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <nuttx/mmcsd.h>
#include <nuttx/mutex.h>
#include <nuttx/sdio.h>
#include <syslog.h>

#include "esp_sdmmc.h"

#include "eh_host_port_config.h"
#include "eh_host_port_sdio.h"
#include "eh_host_port_sync.h"
#include "eh_host_port_sdio_reg.h"

#define SDIO_TAG "eh_host_port_sdio"

/* Entry/exit tracing for the port's transfer entry points.
 *
 * When the transport stalls, the question is which call never returned: a task
 * stuck inside one of these holds the bus lock and everything else waits for it
 * (appendix BU.6).  Capped, so the console stays readable.
 */

static void port_trace(const char *fn, int id, int dir, unsigned a,
                       unsigned b, int r)
{
  static int lines;

  if (lines < 24)
    {
      syslog(LOG_INFO, "eh_host_port_sdio: %s %s id=%d addr=0x%05x size=%u ret=%d\n",
             fn, dir ? "exit" : "enter", id, a, b, r);
      lines++;
    }
}


#ifdef CONFIG_ESPRESSIF_HOSTED_OFFICIAL

#define TAG "eh_host_port_sdio"

/* CCCR/FBR registers used here; the values are the SDIO specification's. */

#define CCCR_IO_ENABLE        0x02
#define CCCR_BUS_IF           0x07
#define CCCR_FN1_BLKSIZE      0x110
#define CCCR_FN0_BLKSIZE      0x10
#define FN1_ENABLE_MASK       0x02          /* BIT(1) */

#define ESP_HOSTED_SDIO_BLOCK_SIZE 512

struct eh_host_port_sdio_ctx_s
{
  struct sdio_dev_s *dev;
  mutex_t            lock;
  bool               ready;
};

static struct eh_host_port_sdio_ctx_s g_sdio_ctx;
static bool g_sdio_ctx_init;

/****************************************************************************
 * Name: eh_host_port_sdio_init
 *
 * Description:
 *   Take the SDIO host device for the configured slot.  The controller itself
 *   is brought up by esp_sdmmc_initialize(); this only remembers the handle and
 *   creates the bus lock the port's lock_required parameter refers to.
 *
 ****************************************************************************/

void *eh_host_port_sdio_init(void)
{
  if (!g_sdio_ctx_init)
    {
      nxmutex_init(&g_sdio_ctx.lock);
      g_sdio_ctx_init = true;
    }

  g_sdio_ctx.dev = esp_sdmmc_initialize(CONFIG_ESPRESSIF_SDMMC_SLOT);

  if (g_sdio_ctx.dev == NULL)
    {
      syslog(LOG_ERR, "%s: no SDIO device on slot %d\n",
             TAG, CONFIG_ESPRESSIF_SDMMC_SLOT);
      return NULL;
    }

  return &g_sdio_ctx;
}

int eh_host_port_sdio_deinit(void *ctx)
{
  g_sdio_ctx.ready = false;
  return ESP_OK;
}

/****************************************************************************
 * Name: eh_host_port_sdio_card_init
 *
 * Description:
 *   Enumerate the companion and put function 1 into the state esp-hosted
 *   expects: I/O enabled in CCCR, 512-byte blocks on FN0 and FN1, and the bus
 *   widened to what eh_host_port_master_config.h asks for.
 *
 ****************************************************************************/

int eh_host_port_sdio_card_init(void *ctx, bool show_config)
{
  struct eh_host_port_sdio_ctx_s *c =
    (struct eh_host_port_sdio_ctx_s *)ctx;
  uint8_t  value;
  int      ret;

  if (c == NULL || c->dev == NULL)
    {
      return ESP_FAIL;
    }

  ret = sdio_probe(c->dev);
  if (ret < 0)
    {
      syslog(LOG_ERR, "%s: sdio_probe failed: %d\n", TAG, ret);
      return ESP_FAIL;
    }

  /* Function 1 I/O enable (CCCR 0x02, BIT(1)). */

  ret = sdio_io_rw_direct(c->dev, false, 0, CCCR_IO_ENABLE, 0, &value);
  if (ret < 0)
    {
      return ESP_FAIL;
    }

  value |= FN1_ENABLE_MASK;

  ret = sdio_io_rw_direct(c->dev, true, 0, CCCR_IO_ENABLE, value, NULL);
  if (ret < 0)
    {
      return ESP_FAIL;
    }

  /* 512-byte blocks on both functions.  FN0's copy lives at 0x10, function 1's
   * at 0x110 (FBR offset 0x10) -- reading 0x100/0x101 returns a different,
   * read-as-zero FBR byte, which cost a round of debugging in appendix AB.
   */

  ret  = sdio_io_rw_direct(c->dev, true, 0, CCCR_FN0_BLKSIZE,
                           ESP_HOSTED_SDIO_BLOCK_SIZE & 0xff, NULL);
  ret |= sdio_io_rw_direct(c->dev, true, 0, CCCR_FN0_BLKSIZE + 1,
                           (ESP_HOSTED_SDIO_BLOCK_SIZE >> 8) & 0xff, NULL);
  ret |= sdio_io_rw_direct(c->dev, true, 0, CCCR_FN1_BLKSIZE,
                           ESP_HOSTED_SDIO_BLOCK_SIZE & 0xff, NULL);
  ret |= sdio_io_rw_direct(c->dev, true, 0, CCCR_FN1_BLKSIZE + 1,
                           (ESP_HOSTED_SDIO_BLOCK_SIZE >> 8) & 0xff, NULL);
  if (ret < 0)
    {
      syslog(LOG_ERR, "%s: block size setup failed: %d\n", TAG, ret);
      return ESP_FAIL;
    }

  if (EH_HOST_PORT_SDIO_BUS_WIDTH == 4)
    {
      ret = sdio_set_wide_bus(c->dev);
      if (ret < 0)
        {
          return ESP_FAIL;
        }
    }

  c->ready = true;

  if (show_config)
    {
      syslog(LOG_INFO,
             "%s: card ready: slot %d, %d-bit, block %d, CLK io_%d CMD io_%d "
             "D0..D3 %d,%d,%d,%d\n",
             TAG, CONFIG_ESPRESSIF_SDMMC_SLOT, EH_HOST_PORT_SDIO_BUS_WIDTH,
             ESP_HOSTED_SDIO_BLOCK_SIZE, EH_HOST_PORT_SDIO_PIN_CLK,
             EH_HOST_PORT_SDIO_PIN_CMD, EH_HOST_PORT_SDIO_PIN_D0,
             EH_HOST_PORT_SDIO_PIN_D1, EH_HOST_PORT_SDIO_PIN_D2,
             EH_HOST_PORT_SDIO_PIN_D3);
    }

  return ESP_OK;
}

int eh_host_port_sdio_card_deinit(void *ctx)
{
  struct eh_host_port_sdio_ctx_s *c =
    (struct eh_host_port_sdio_ctx_s *)ctx;

  if (c != NULL)
    {
      c->ready = false;
    }

  return ESP_OK;
}

/****************************************************************************
 * Name: eh_host_port_sdio_read_reg / write_reg
 *
 * Description:
 *   Scratch-register access.  One byte is a CMD52; anything wider is a
 *   byte-mode CMD53, which is what esp-hosted's own host uses for the 4-byte
 *   registers (INT_RAW, PACKET_LEN).
 *
 ****************************************************************************/

int eh_host_port_sdio_read_reg(void *ctx, uint32_t reg, uint8_t *data,
                               uint16_t size, bool lock_required)
{
  static int calls;
  int trace_id = ++calls;

  port_trace("read_reg", trace_id, 0, reg, size, 0);
  struct eh_host_port_sdio_ctx_s *c =
    (struct eh_host_port_sdio_ctx_s *)ctx;
  int ret;

  if (c == NULL || c->dev == NULL || data == NULL)
    {
      return ESP_FAIL;
    }
  (void)lock_required;
  /* Mask the address to the slave's register window.
   *
   * The transport passes the *slave's* absolute address (e.g. 0x3ff55050 for
   * INT_RAW), and the SDIO function decodes only the low bits of it.  The
   * vendor's own port applies ESP_ADDRESS_MASK here and this one did not --
   * which is why register reads came back as one garbage byte repeated
   * (val=1c 1c 1c 1c) instead of the interrupt bits.
   */

  reg &= ESP_ADDRESS_MASK;


  /* The port's own mutex is deliberately not taken here.
   *
   * The transport serialises every register and block access with SDIO_DRV_LOCK
   * (eh_host_mutex_lock(sdio_bus_lock)), so this second lock guards nothing --
   * and it became a deadlock point: measured, eh_host_port_sdio_read_reg()
   * entered and never even acquired it, because a transfer that had taken it
   * never returned (appendices BU.6/BV).  Leaving it out keeps the calls
   * serialised by the transport's lock alone.
   */

  if (lock_required)
    {
      port_trace("read_reg", trace_id, 0, reg, size, 1);   /* locked */
    }

  if (size == 1)
    {
      ret = sdio_io_rw_direct(c->dev, false, 1, reg, 0, data);
    }
  else
    {
      /* One byte-mode transfer for the whole window.
       *
       * Four-byte chunks were tried and hang (the completion trace never
       * returns), even though the bring-up ladder reads the same register as
       * four bytes successfully -- the difference between the two contexts is
       * the next thing to chase, and until it is understood a transfer that
       * returns is worth more than one that does not.
       */

      ret = sdio_io_rw_extended(c->dev, false, 1, reg, true, data, size, 0);
    }

  /* Trace the first few register reads.
   *
   * This is the path the stack actually walks while it waits: it polls the
   * companion's interrupt/packet-length registers and only then decides to pull
   * a payload (appendices BP/BQ).  Logging the address, the result and the value
   * says what the stack is seeing, which is the last piece of the software-side
   * picture.  Capped so the console stays readable.
   */

  {
    static int traced;

    if (traced < 12)
      {
        syslog(LOG_INFO,
               "%s: read_reg addr=0x%05x size=%u ret=%d val=%02x %02x %02x %02x\n",
               SDIO_TAG, (unsigned)reg, (unsigned)size, ret,
               data[0], size > 1 ? data[1] : 0,
               size > 2 ? data[2] : 0, size > 3 ? data[3] : 0);
        traced++;
      }
  }

  return (ret < 0) ? ESP_FAIL : ESP_OK;
}

int eh_host_port_sdio_write_reg(void *ctx, uint32_t reg, uint8_t *data,
                                uint16_t size, bool lock_required)
{
  struct eh_host_port_sdio_ctx_s *c =
    (struct eh_host_port_sdio_ctx_s *)ctx;
  int ret;

  if (c == NULL || c->dev == NULL || data == NULL)
    {
      return ESP_FAIL;
    }
  (void)lock_required;
  /* Mask the address to the slave's register window.
   *
   * The transport passes the *slave's* absolute address (e.g. 0x3ff55050 for
   * INT_RAW), and the SDIO function decodes only the low bits of it.  The
   * vendor's own port applies ESP_ADDRESS_MASK here and this one did not --
   * which is why register reads came back as one garbage byte repeated
   * (val=1c 1c 1c 1c) instead of the interrupt bits.
   */

  reg &= ESP_ADDRESS_MASK;


  /* The port's own mutex is deliberately not taken here.
   *
   * The transport serialises every register and block access with SDIO_DRV_LOCK
   * (eh_host_mutex_lock(sdio_bus_lock)), so this second lock guards nothing --
   * and it became a deadlock point: measured, eh_host_port_sdio_read_reg()
   * entered and never even acquired it, because a transfer that had taken it
   * never returned (appendices BU.6/BV).  Leaving it out keeps the calls
   * serialised by the transport's lock alone.
   */

  if (size == 1)
    {
      ret = sdio_io_rw_direct(c->dev, true, 1, reg, data[0], NULL);
    }
  else
    {
      /* Multi-byte register writes go out as one CMD52 per byte, for the same
       * reason the reads do (see eh_host_port_sdio_read_reg): the byte-mode
       * CMD53 does not complete here, and the transport's very next step after
       * reading the interrupt register is to clear it with a four-byte write.
       */

      unsigned i;

      ret = 0;
      for (i = 0; i < size; i++)
        {
          int one = sdio_io_rw_direct(c->dev, true, 1,
                                      (uint32_t)(reg + i), data[i], NULL);
          if (one < 0)
            {
              ret = one;
              break;
            }
        }
    }

  return (ret < 0) ? ESP_FAIL : ESP_OK;
}

/****************************************************************************
 * Name: eh_host_port_sdio_read_block / write_block
 *
 * Description:
 *   Payload transfers out of / into the companion's window, in block mode with
 *   the 512-byte block size configured above.
 *
 ****************************************************************************/

int eh_host_port_sdio_read_block(void *ctx, uint32_t reg, uint8_t *data,
                                 uint16_t size, bool lock_required)
{
  static int calls;
  int trace_id = ++calls;

  port_trace("read_block", trace_id, 0, reg, size, 0);
  struct eh_host_port_sdio_ctx_s *c =
    (struct eh_host_port_sdio_ctx_s *)ctx;
  unsigned nblocks;
  int ret;

  if (c == NULL || c->dev == NULL || data == NULL || size == 0)
    {
      return ESP_FAIL;
    }

  nblocks = (size + ESP_HOSTED_SDIO_BLOCK_SIZE - 1) /
            ESP_HOSTED_SDIO_BLOCK_SIZE;
  (void)lock_required;
  /* Mask the address to the slave's register window.
   *
   * The transport passes the *slave's* absolute address (e.g. 0x3ff55050 for
   * INT_RAW), and the SDIO function decodes only the low bits of it.  The
   * vendor's own port applies ESP_ADDRESS_MASK here and this one did not --
   * which is why register reads came back as one garbage byte repeated
   * (val=1c 1c 1c 1c) instead of the interrupt bits.
   */

  reg &= ESP_ADDRESS_MASK;


  /* The port's own mutex is deliberately not taken here.
   *
   * The transport serialises every register and block access with SDIO_DRV_LOCK
   * (eh_host_mutex_lock(sdio_bus_lock)), so this second lock guards nothing --
   * and it became a deadlock point: measured, eh_host_port_sdio_read_reg()
   * entered and never even acquired it, because a transfer that had taken it
   * never returned (appendices BU.6/BV).  Leaving it out keeps the calls
   * serialised by the transport's lock alone.
   */

  ret = sdio_io_rw_extended(c->dev, false, 1, reg, true, data,
                            ESP_HOSTED_SDIO_BLOCK_SIZE, nblocks);

  /* Trace the first few payload reads.
   *
   * These are the transfers the official stack issues once the data path is
   * open; they are the ones whose data phase never happens on this board
   * (appendices BP/BQ).  Logging the return code and the first bytes here puts
   * the finding on the stack's own path instead of only on the bring-up sweep,
   * and the count is capped so the console stays readable.
   */

  {
    static int traced;

    if (traced < 10)
      {
        syslog(LOG_INFO,
               "%s: read_block f%d addr=0x%05x size=%u nb=%u ret=%d first=%02x %02x %02x %02x\n",
               SDIO_TAG, 1, (unsigned)reg, (unsigned)size, nblocks, ret,
               data[0], data[1], data[2], data[3]);
        traced++;
      }
  }

  return (ret < 0) ? ESP_FAIL : ESP_OK;
}

int eh_host_port_sdio_write_block(void *ctx, uint32_t reg, uint8_t *data,
                                  uint16_t size, bool lock_required)
{
  static int calls;
  int trace_id = ++calls;

  port_trace("write_block", trace_id, 0, reg, size, 0);
  struct eh_host_port_sdio_ctx_s *c =
    (struct eh_host_port_sdio_ctx_s *)ctx;
  unsigned nblocks;
  int ret;

  if (c == NULL || c->dev == NULL || data == NULL || size == 0)
    {
      return ESP_FAIL;
    }

  nblocks = (size + ESP_HOSTED_SDIO_BLOCK_SIZE - 1) /
            ESP_HOSTED_SDIO_BLOCK_SIZE;
  (void)lock_required;
  /* Mask the address to the slave's register window.
   *
   * The transport passes the *slave's* absolute address (e.g. 0x3ff55050 for
   * INT_RAW), and the SDIO function decodes only the low bits of it.  The
   * vendor's own port applies ESP_ADDRESS_MASK here and this one did not --
   * which is why register reads came back as one garbage byte repeated
   * (val=1c 1c 1c 1c) instead of the interrupt bits.
   */

  reg &= ESP_ADDRESS_MASK;


  /* The port's own mutex is deliberately not taken here.
   *
   * The transport serialises every register and block access with SDIO_DRV_LOCK
   * (eh_host_mutex_lock(sdio_bus_lock)), so this second lock guards nothing --
   * and it became a deadlock point: measured, eh_host_port_sdio_read_reg()
   * entered and never even acquired it, because a transfer that had taken it
   * never returned (appendices BU.6/BV).  Leaving it out keeps the calls
   * serialised by the transport's lock alone.
   */

  ret = sdio_io_rw_extended(c->dev, true, 1, reg, true, data,
                            ESP_HOSTED_SDIO_BLOCK_SIZE, nblocks);

  return (ret < 0) ? ESP_FAIL : ESP_OK;
}

/* Longest slice handed to the driver's card-interrupt wait, in milliseconds.
 * Keeping it well under 2^31 stops MSEC2TICK() from wrapping.
 */

#define EH_SDIO_INTR_SLICE_MS 1000

/* Poll interval used when the caller asks to wait forever (see the fallback in
 * eh_host_port_sdio_wait_slave_intr()).
 */

#define EH_SDIO_INTR_POLL_MS 20

/****************************************************************************
 * Name: eh_host_port_sdio_wait_slave_intr
 *
 * Description:
 *   Block until the companion raises its SDIO interrupt (DAT1) or the wait
 *   expires.  esp_sdmmc_wait_card_intr() takes milliseconds, and the stack's
 *   ticks_to_wait is passed straight through -- the caller only distinguishes
 *   "interrupted" from "timed out", so the unit does not change behaviour, and
 *   the value it passes is a few tens of ticks.
 *
 ****************************************************************************/

int eh_host_port_sdio_wait_slave_intr(void *ctx, uint32_t ticks_to_wait)
{
  struct eh_host_port_sdio_ctx_s *c =
    (struct eh_host_port_sdio_ctx_s *)ctx;
  bool     forever;
  uint32_t remaining;

  if (c == NULL || c->dev == NULL)
    {
      return ESP_FAIL;
    }

  /* Walk the wait in bounded slices.
   *
   * This function used to hand the stack's timeout straight to the driver, and
   * the transport asks for EH_HOST_PORT_WAIT_FOREVER (0xffffffff).  The driver
   * converts milliseconds to ticks, so that value wraps and the wait ends
   * immediately -- measured on hardware: the transport spun on
   * "wait_slave_intr error: -1" thousands of times a second instead of
   * blocking.  Slicing keeps every conversion in range; in practice the card's
   * interrupt is what ends the wait.
   */

  forever   = (ticks_to_wait == EH_HOST_PORT_WAIT_FOREVER);
  remaining = ticks_to_wait;

  /* "Forever" is answered by polling: see the fallback at the bottom of the
   * loop.  A short slice keeps the poll rate useful (the transport's register
   * read is cheap) without spinning.
   */

  if (forever)
    {
      static uint32_t calls;

      calls++;
      if (calls <= 20 || (calls % 50) == 0)
        {
          syslog(LOG_INFO, "%s: wait#%u (forever) -> polling\n",
                 SDIO_TAG, (unsigned)calls);
        }

      (void)esp_sdmmc_wait_card_intr(c->dev, EH_SDIO_INTR_POLL_MS);
      return ESP_OK;
    }

  /* What the wait actually observes.
   *
   * The transport's log only says it is waiting; whether the line is asserted
   * and what the driver answers decides where the stall is.  Capped, so the
   * console stays readable.
   */

  {
    static int traced;

    if (traced < 12)
      {
        int level = esp_gpioread(CONFIG_ESPRESSIF_SDMMC_D1);
        bool got  = esp_sdmmc_wait_card_intr(c->dev, 100);

        syslog(LOG_INFO,
               "%s: wait_slave_intr ticks=%u D1=%d probe=%d\n",
               SDIO_TAG, (unsigned)ticks_to_wait, level, (int)got);
        traced++;

        if (got)
          {
            return ESP_OK;
          }
      }
  }

  for (; ; )
    {
      uint32_t slice = (forever || remaining > EH_SDIO_INTR_SLICE_MS) ?
                       EH_SDIO_INTR_SLICE_MS : remaining;

      if (esp_sdmmc_wait_card_intr(c->dev, slice))
        {
          return ESP_OK;
        }

      /* Poll-mode fallback.
       *
       * The card-interrupt line is not observable from here on this board: the
       * DAT1 pad belongs to the SDMMC peripheral once the slot is routed through
       * the GPIO matrix (esp_gpioread() then reads a constant), and the CIU's own
       * SDIO-interrupt status bit does not latch either (appendices BR/BS).
       * Meanwhile the data path works -- the companion's registers read back
       * correctly (appendix BU) -- so the transport can find out what the
       * companion wants by *reading its interrupt register*, which is exactly
       * what it does after this returns.
       *
       * Returning "interrupted" once per slice therefore turns a wait that can
       * never be satisfied into a poll of the companion's INT_RAW at slice rate.
       */

      if (slice >= EH_SDIO_INTR_SLICE_MS)
        {
          return ESP_OK;
        }

      if (forever)
        {
          continue;
        }

      if (remaining <= slice)
        {
          return ESP_ERR_TIMEOUT;
        }

      remaining -= slice;
    }
}

#endif /* CONFIG_ESPRESSIF_HOSTED_OFFICIAL */
