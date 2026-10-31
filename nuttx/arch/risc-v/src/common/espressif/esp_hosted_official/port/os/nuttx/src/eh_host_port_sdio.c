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
#include <nuttx/signal.h>
#include <syslog.h>

#include "esp_sdmmc.h"

#include "eh_host_port_config.h"
#include "eh_host_port_sdio.h"
#include "eh_host_port_sync.h"
#include "eh_host_port_sdio_reg.h"

#define SDIO_TAG "eh_host_port_sdio"

#include <nuttx/wdog.h>

/* Transfer guard.
 *
 * A stuck transfer is invisible from the outside: the call simply never
 * returns, and the transport's own watchdog path cannot be reached.  This pair
 * of helpers makes it visible: the watchdog sets a flag after a second, and the
 * next port call reports that the previous one never came back.  It cannot
 * unblock the stuck call (the driver's wait does not consult this flag), but it
 * turns "silence" into a countable, datable event.
 */

static volatile bool g_xfer_stuck;
static struct wdog_s  g_xfer_wd;

static void xfer_guard_expire(wdparm_t arg)
{
  (void)arg;
  g_xfer_stuck = true;
}

static void xfer_guard_arm(void)
{
  if (g_xfer_stuck)
    {
      syslog(LOG_WARNING, "%s: previous transfer never returned\n", SDIO_TAG);
      g_xfer_stuck = false;
    }

  wd_start(&g_xfer_wd, MSEC2TICK(1000), xfer_guard_expire, 0);
}

static void xfer_guard_disarm(void)
{
  wd_cancel(&g_xfer_wd);
  g_xfer_stuck = false;
}


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

#define CCCR_REV              0x00          /* CCCR revision (0x32 on this CP) */
#define CCCR_IO_ENABLE        0x02
#define CCCR_IO_READY         0x03          /* function-ready status word     */
#define CCCR_INT_ENABLE       0x04
#define CCCR_BUS_IF           0x07
#define CCCR_FN1_BLKSIZE      0x110
#define CCCR_FN0_BLKSIZE      0x10
#define FN1_ENABLE_MASK       0x02          /* BIT(1)                         */
#define IENM_MASK             0x01          /* BIT(0) interrupt master enable */

#define ESP_HOSTED_SDIO_BLOCK_SIZE 512

/* Card bring-up retry policy, taken from the IDF port's card_fn_init():
 * ten tries, ten milliseconds apart.
 */

#define SDIO_FN_READY_TRIES   10
#define SDIO_FN_READY_WAIT_MS 10

struct eh_host_port_sdio_ctx_s
{
  struct sdio_dev_s *dev;
  mutex_t            lock;
  bool               ready;
};

static struct eh_host_port_sdio_ctx_s g_sdio_ctx;
static bool g_sdio_ctx_init;

/****************************************************************************
 * Name: sdio_port_transfer
 *
 * Description:
 *   One I/O transfer over the function-1 window, in the exact shape
 *   esp-hosted's own port uses (port/os/idf/src/eh_host_port_sdio.c,
 *   sdio_read_fromio()/sdio_write_toio()):
 *
 *     - 512 bytes or more: block mode, as many 512-byte blocks as fit;
 *     - the remainder:     byte mode, one transfer of that many bytes.
 *
 *   The address is deliberately NOT masked here.  Register accesses do live in
 *   the 10-bit window that ESP_ADDRESS_MASK selects (read_reg and write_reg
 *   apply it), but the payload window is addressed as
 *   ESP_SLAVE_CMD53_END_ADDR - len -- 0x1F7C4 for a 60-byte packet -- and
 *   masking that silently turns the transfer into a read of register 0x3C4.
 *   That was the defect of appendix BY: the companion's packet was never
 *   drained, so its length register reported the same packet forever, its
 *   interrupt stayed asserted and every later exchange stalled behind it.
 *
 *   Unlike the IDF port the tail is not padded up to a 4-byte boundary
 *   (EH_HOST_PORT_SDIO_RX_LEN_TO_TRANSFER).  The length comes from the caller,
 *   and rounding it up writes up to three bytes past the caller's buffer; every
 *   caller in the official transport passes either a 512-byte multiple (payload,
 *   block-only mode) or a 4-byte aligned register window, so nothing is lost.
 *
 * Input Parameters:
 *   c     - port context
 *   write - true for host -> slave
 *   reg   - function-1 address of the first byte (not masked, see above)
 *   data  - the buffer
 *   size  - number of bytes to transfer
 *
 * Returned Value:
 *   Zero on success, a negated errno on failure.
 *
 ****************************************************************************/

static int sdio_port_transfer(struct eh_host_port_sdio_ctx_s *c, bool write,
                              uint32_t reg, uint8_t *data, uint16_t size)
{
  unsigned done = 0;

  while ((unsigned)(size - done) >= ESP_HOSTED_SDIO_BLOCK_SIZE)
    {
      unsigned blocks = (unsigned)(size - done) / ESP_HOSTED_SDIO_BLOCK_SIZE;
      int      ret;

      ret = sdio_io_rw_extended(c->dev, write, 1, reg + done, true,
                                &data[done], ESP_HOSTED_SDIO_BLOCK_SIZE,
                                blocks);
      if (ret < 0)
        {
          return ret;
        }

      done += blocks * ESP_HOSTED_SDIO_BLOCK_SIZE;
    }

  if (done < size)
    {
      unsigned left = (unsigned)(size - done);
      int      ret;

      if ((left & 3u) != 0)
        {
          syslog(LOG_WARNING,
                 "%s: byte-mode transfer of %u bytes is not 4-byte aligned\n",
                 SDIO_TAG, left);
        }

      ret = sdio_io_rw_extended(c->dev, write, 1, reg + done, true,
                                &data[done], (int)left, 0);
      if (ret < 0)
        {
          return ret;
        }
    }

  return 0;
}

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

  /* The controller is left exactly as the bring-up left it.
   *
   * SDIO_RESET() was tried here, mirroring the official port's sdmmc_host_init()
   * + sdmmc_host_init_slot(), and it made things worse: the reset clears the CIU
   * but the driver's clock path is what re-programs the card clock afterwards,
   * and that path can end with the clock disabled -- after which the companion
   * answers nothing at all (every command completes with RINTSTS = 0).  Leaving
   * the controller alone keeps a clock that is known to work.
   */

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
  uint8_t  ready;
  int      ret;
  int      i;

  if (c == NULL || c->dev == NULL)
    {
      return ESP_FAIL;
    }

  /* Enumerate -- unless the card is already up.
   *
   * esp_hosted_sdio.c's bring-up enumerates the companion (and raises the bus
   * to its transfer clock) before the official stack is started, so card_init
   * is normally called on a card that is already selected.  A second
   * enumeration cannot work from that state -- measured: CMD55 gets no answer
   * while the card is selected and sdio_probe() returns -22 -- so ask CCCR
   * instead, which is what the function setup below needs anyway.
   *
   * Only a card that does not answer CCCR is probed the way the official port
   * does it (sdmmc_card_init), which is the standalone / first-boot case.
   */

  {
    uint8_t rev = 0;
    int     r2  = sdio_io_rw_direct(c->dev, false, 0, CCCR_REV, 0, &rev);

    if (r2 == 0 && rev != 0 && rev != 0xff)
      {
        syslog(LOG_INFO, "%s: card already enumerated (CCCR 0x00=%02x), "
               "skipping the re-probe\n", TAG, rev);
      }
    else
      {
        int tries;

        SDIO_CLOCK(c->dev, CLOCK_IDMODE);

        /* Give the companion time to come up.
         *
         * A card that has just been released from reset does not answer CMD5
         * immediately: measured, the stack's own card_init retries land about
         * 200 ms apart and all six of them fall inside that window, while the
         * bring-up's boot-time probe (four tries, 500 ms apart) always finds
         * it.  Retry here for the same reason, so the port does not depend on
         * the caller's retry cadence -- eight tries, half a second apart.
         */

        ret = -1;

        for (tries = 0; tries < 8; tries++)
          {
            ret = sdio_probe(c->dev);
            if (ret == OK)
              {
                if (tries > 0)
                  {
                    syslog(LOG_INFO,
                           "%s: SDIO probe succeeded on try %d\n",
                           TAG, tries + 1);
                  }

                break;
              }

            nxsig_usleep(500 * 1000);
          }

        if (ret != OK)
          {
            syslog(LOG_ERR, "%s: sdio_probe failed: %d\n", TAG, ret);
            return ESP_FAIL;
          }
      }
  }

  /* What follows is esp-hosted's own card_fn_init()
   * (port/os/idf/src/eh_host_port_sdio.c): enable function 1, WAIT for it to
   * report ready, enable interrupts for function 1 plus the master enable, and
   * set the block size on both functions.
   *
   * The order matters and the wait is load-bearing: function 1 does not answer
   * until the companion has brought it up, and an access made before that times
   * out -- which is precisely what appendix BX measured on a host that skipped
   * these steps (retracted there, explained in appendix BY.1).
   */

  /* 1. Function 1 I/O enable (CCCR 0x02, BIT(1)). */

  ret = sdio_io_rw_direct(c->dev, false, 0, CCCR_IO_ENABLE, 0, &value);
  if (ret < 0)
    {
      return ESP_FAIL;
    }

  syslog(LOG_INFO, "%s: CCCR 0x02 IOEx before: %02x\n", TAG, value);

  value |= FN1_ENABLE_MASK;

  ret = sdio_io_rw_direct(c->dev, true, 0, CCCR_IO_ENABLE, value, NULL);
  if (ret < 0)
    {
      return ESP_FAIL;
    }

  ret = sdio_io_rw_direct(c->dev, false, 0, CCCR_IO_ENABLE, 0, &value);
  if (ret < 0 || (value & FN1_ENABLE_MASK) == 0)
    {
      syslog(LOG_ERR, "%s: function 1 enable did not stick (CCCR 0x02=%02x)\n",
             TAG, value);
      return ESP_FAIL;
    }

  syslog(LOG_INFO, "%s: CCCR 0x02 IOEx after: %02x\n", TAG, value);

  /* 2. Wait for function 1 to report ready (CCCR 0x03, BIT(1)). */

  ready = 0;

  for (i = 0; i < SDIO_FN_READY_TRIES; i++)
    {
      ret = sdio_io_rw_direct(c->dev, false, 0, CCCR_IO_READY, 0, &ready);
      if (ret < 0)
        {
          return ESP_FAIL;
        }

      if ((ready & FN1_ENABLE_MASK) != 0)
        {
          break;
        }

      nxsig_usleep(SDIO_FN_READY_WAIT_MS * 1000);
    }

  syslog(LOG_INFO, "%s: CCCR 0x03 IO_RDY=%02x after %d try/tries\n",
         TAG, ready, i + 1);

  if ((ready & FN1_ENABLE_MASK) == 0)
    {
      syslog(LOG_ERR, "%s: function 1 never reported ready -- the companion is "
             "not serving FN1, so nothing else can be tested\n", TAG);
      return ESP_FAIL;
    }

  /* 3. Interrupt enable: the function bit AND the master enable.
   *
   * NuttX's sdio_enable_interrupt() sets only the function bit.  Without IENM
   * (BIT(0) of the same register) the card is not allowed to drive DAT1 at all,
   * which is why the transport could wait forever for an interrupt that could
   * never arrive (appendices BR/BS).
   */

  ret = sdio_io_rw_direct(c->dev, false, 0, CCCR_INT_ENABLE, 0, &value);
  if (ret < 0)
    {
      return ESP_FAIL;
    }

  syslog(LOG_INFO, "%s: CCCR 0x04 INT_ENABLE before: %02x\n", TAG, value);

  value |= IENM_MASK | FN1_ENABLE_MASK;

  ret = sdio_io_rw_direct(c->dev, true, 0, CCCR_INT_ENABLE, value, NULL);
  if (ret < 0)
    {
      return ESP_FAIL;
    }

  ret = sdio_io_rw_direct(c->dev, false, 0, CCCR_INT_ENABLE, 0, &ready);
  if (ret < 0)
    {
      return ESP_FAIL;
    }

  syslog(LOG_INFO, "%s: CCCR 0x04 INT_ENABLE after: %02x (want %02x)\n",
         TAG, ready, value);

  if ((ready & (IENM_MASK | FN1_ENABLE_MASK)) !=
      (IENM_MASK | FN1_ENABLE_MASK))
    {
      /* Not fatal: the wait path falls back to polling the companion's
       * interrupt register, which is what the transport reads next anyway.
       */

      syslog(LOG_ERR, "%s: interrupt enable did not stick; the slave-interrupt "
             "wait will fall back to polling\n", TAG);
    }

  /* 4. 512-byte blocks on both functions.  FN0's copy lives at 0x10, function
   * 1's at 0x110 (FBR offset 0x10) -- reading 0x100/0x101 returns a different,
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

  /* Read both back: the official port verifies its block-size writes too, and a
   * silently dropped 512 leaves block-mode transfers with a zero block size.
   */

  {
    uint8_t lo;
    uint8_t hi;
    int     rb;

    lo = 0;
    hi = 0;
    rb  = sdio_io_rw_direct(c->dev, false, 0, CCCR_FN0_BLKSIZE, 0, &lo);
    rb |= sdio_io_rw_direct(c->dev, false, 0, CCCR_FN0_BLKSIZE + 1, 0, &hi);
    syslog(LOG_INFO, "%s: FN0 block size read back %u (ret %d)\n", TAG,
           (unsigned)(lo | ((unsigned)hi << 8)), rb);

    lo = 0;
    hi = 0;
    rb  = sdio_io_rw_direct(c->dev, false, 0, CCCR_FN1_BLKSIZE, 0, &lo);
    rb |= sdio_io_rw_direct(c->dev, false, 0, CCCR_FN1_BLKSIZE + 1, 0, &hi);
    syslog(LOG_INFO, "%s: FN1 block size read back %u (ret %d)\n", TAG,
           (unsigned)(lo | ((unsigned)hi << 8)), rb);

    if (rb < 0)
      {
        return ESP_FAIL;
      }
  }

  if (EH_HOST_PORT_SDIO_BUS_WIDTH == 4)
    {
      ret = sdio_set_wide_bus(c->dev);
      if (ret < 0)
        {
          return ESP_FAIL;
        }
    }

  /* The card clock is left exactly as the bring-up set it.
   *
   * Raising it here was tried and is what broke the hand-over: the driver's
   * transfer-clock path ends with the clock disabled (see the note in
   * esp_hosted_sdio.c), after which the companion answers nothing at all.
   * Running the link at the identification rate costs throughput, not
   * correctness -- the caps handshake and an RPC round trip fit in a few
   * hundred bytes.
   */

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
  /* Serialise port access with a bounded wait.
   *
   * The transport does have its own SDIO_DRV_LOCK, but the receive and transmit
   * tasks both reach these functions and an interleaved transfer is what hangs
   * (measured: the bring-up ladder's four-byte read of INT_RAW works while the
   * same call from the transport context never completes).  A bounded wait keeps
   * that from turning into the deadlock an unbounded lock produced earlier
   * (appendix BV.3): on timeout the call proceeds and says so.
   */

  /* Block until the bus is ours, and fail the transfer if it never is.
   *
   * This used to time out after a second and then carry on unlocked, which was
   * a workaround for the wedge of the day (appendices BV.3/BU.6).  With that
   * wedge gone it is a correctness bug: the official transport reads registers,
   * writes registers and moves payloads from more than one task, and an
   * unlocked transfer races the one in flight.  Measured with the timed-out
   * version: the first register read after the hand-over succeeded and every
   * one after it failed with -ETIMEDOUT while the data state machine sat busy
   * (appendix BY.9).  Failing the call is the honest outcome when the lock
   * cannot be taken -- corrupting the transfer in flight is not.
   */

  if (lock_required && nxmutex_lock(&c->lock) < 0)
    {
      syslog(LOG_ERR, "%s: %s: cannot take the bus lock\n",
             SDIO_TAG, __func__);
      return ESP_FAIL;
    }

  /* Mask the address to the slave's register window.
   *
   * The transport passes the *slave's* absolute address (e.g. 0x3ff55050 for
   * INT_RAW) for register accesses, and the SDIO function decodes the low bits
   * of it, so the mask belongs here.  It must NOT be applied to the payload
   * transfers: those address the packet window as
   * ESP_SLAVE_CMD53_END_ADDR - len (see sdio_port_transfer(), appendix BY.2).
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

  xfer_guard_arm();

  /* One register per transfer: four bytes at a time.
   *
   * A single byte is a CMD52; anything wider is split into four-byte byte-mode
   * CMD53 transfers.  That is *not* the shape the IDF port uses (it hands the
   * whole size to sdmmc_io_read_bytes(), and the transport asks for the twenty
   * byte INT_RAW..PACKET_LEN window in one call), but it is the shape this
   * companion answers: measured on the same board with ESP-IDF's own sdmmc
   * stack as the reference host (tools/test/p4_sdio_host_idf, appendix BZ.5),
   *
   *     window 0x50 x20 -> ESP_OK bytes=00400800 00000000 00000000 ...
   *     window 0x50 x4  -> ESP_OK bytes=00400800      <- the real INT_RAW
   *
   * -- only the first four bytes of a longer register-window read carry data,
   * so a twenty-byte read would hand the transport a zero PACKET_LEN and the
   * caps handshake would never see a packet.  Four-byte transfers also match
   * what the IDF host's own sdmmc_io_read_bytes() ends up doing for the
   * four-byte registers the transport reads most (INT_RAW, PACKET_LEN, TOKEN).
   *
   * The payload window (read_block/write_block) is different and keeps the
   * official shapes -- the companion does move hundreds of bytes there (the
   * reference host read 360 of them, appendix BZ.2).
   */

  if (size == 1)
    {
      ret = sdio_io_rw_direct(c->dev, false, 1, reg, 0, data);
    }
  else
    {
      /* ONE transfer of the whole register window, as the IDF port does.
       *
       * This was briefly split into four-byte transfers on the theory that the
       * companion only answers the first four bytes of a longer window read.
       * Re-measuring cleared that up: the twenty-byte read and the individual
       * four-byte reads return the *same* values, because the registers that
       * looked like padding were genuinely zero at that moment (appendix
       * BZ.5/BY.9).  Splitting therefore bought nothing and multiplied the
       * transfer count by five -- five chances to hit the stall below instead
       * of one.
       */

      ret = sdio_io_rw_extended(c->dev, false, 1, reg, true, data,
                                (int)size, 0);
    }

  {
    static int seen;

    if (reg == 0x00050 && size >= 4 && seen < 8)
      {
        syslog(LOG_INFO,
               "%s: transport INT_RAW read: ret=%d bytes=%02x %02x %02x %02x"
               " (bit23=%u)\n",
               SDIO_TAG, ret, data[0], data[1], data[2], data[3],
               (unsigned)(((uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                           ((uint32_t)data[2] << 16) |
                           ((uint32_t)data[3] << 24)) >> 23 & 1u));
        seen++;
      }
  }

  xfer_guard_disarm();

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
  /* Serialise port access with a bounded wait.
   *
   * The transport does have its own SDIO_DRV_LOCK, but the receive and transmit
   * tasks both reach these functions and an interleaved transfer is what hangs
   * (measured: the bring-up ladder's four-byte read of INT_RAW works while the
   * same call from the transport context never completes).  A bounded wait keeps
   * that from turning into the deadlock an unbounded lock produced earlier
   * (appendix BV.3): on timeout the call proceeds and says so.
   */

  /* Block until the bus is ours, and fail the transfer if it never is.
   *
   * This used to time out after a second and then carry on unlocked, which was
   * a workaround for the wedge of the day (appendices BV.3/BU.6).  With that
   * wedge gone it is a correctness bug: the official transport reads registers,
   * writes registers and moves payloads from more than one task, and an
   * unlocked transfer races the one in flight.  Measured with the timed-out
   * version: the first register read after the hand-over succeeded and every
   * one after it failed with -ETIMEDOUT while the data state machine sat busy
   * (appendix BY.9).  Failing the call is the honest outcome when the lock
   * cannot be taken -- corrupting the transfer in flight is not.
   */

  if (lock_required && nxmutex_lock(&c->lock) < 0)
    {
      syslog(LOG_ERR, "%s: %s: cannot take the bus lock\n",
             SDIO_TAG, __func__);
      return ESP_FAIL;
    }

  /* Mask the address to the slave's register window.
   *
   * The transport passes the *slave's* absolute address (e.g. 0x3ff55050 for
   * INT_RAW) for register accesses, and the SDIO function decodes the low bits
   * of it, so the mask belongs here.  It must NOT be applied to the payload
   * transfers: those address the packet window as
   * ESP_SLAVE_CMD53_END_ADDR - len (see sdio_port_transfer(), appendix BY.2).
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

  /* Symmetric to read_reg(): one CMD52 for a single byte, four-byte byte-mode
   * CMD53 transfers for anything wider, for the same reason (the companion only
   * carries the first four bytes of a longer register-window transfer).  The
   * transport's very next step after reading the interrupt register is to clear
   * it with a four-byte write, and that arrives here as one transfer of four
   * bytes (appendix BY.3/BZ.5).
   */

  if (size == 1)
    {
      ret = sdio_io_rw_direct(c->dev, true, 1, reg, data[0], NULL);
    }
  else
    {
      /* One transfer, symmetric to read_reg() (see the note there). */

      ret = sdio_io_rw_extended(c->dev, true, 1, reg, true, data,
                                (int)size, 0);
    }

  if (lock_required)
    {
      nxmutex_unlock(&c->lock);
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
  int ret;

  if (c == NULL || c->dev == NULL || data == NULL || size == 0)
    {
      return ESP_FAIL;
    }

  (void)lock_required;
  /* Serialise port access with a bounded wait.
   *
   * The transport does have its own SDIO_DRV_LOCK, but the receive and transmit
   * tasks both reach these functions and an interleaved transfer is what hangs
   * (measured: the bring-up ladder's four-byte read of INT_RAW works while the
   * same call from the transport context never completes).  A bounded wait keeps
   * that from turning into the deadlock an unbounded lock produced earlier
   * (appendix BV.3): on timeout the call proceeds and says so.
   */

  /* Block until the bus is ours, and fail the transfer if it never is.
   *
   * This used to time out after a second and then carry on unlocked, which was
   * a workaround for the wedge of the day (appendices BV.3/BU.6).  With that
   * wedge gone it is a correctness bug: the official transport reads registers,
   * writes registers and moves payloads from more than one task, and an
   * unlocked transfer races the one in flight.  Measured with the timed-out
   * version: the first register read after the hand-over succeeded and every
   * one after it failed with -ETIMEDOUT while the data state machine sat busy
   * (appendix BY.9).  Failing the call is the honest outcome when the lock
   * cannot be taken -- corrupting the transfer in flight is not.
   */

  if (lock_required && nxmutex_lock(&c->lock) < 0)
    {
      syslog(LOG_ERR, "%s: %s: cannot take the bus lock\n",
             SDIO_TAG, __func__);
      return ESP_FAIL;
    }

  /* The address is NOT masked here: payload transfers live in the companion's
   * packet window, addressed as ESP_SLAVE_CMD53_END_ADDR - len.
   *
   * The official transport calls this function with ESP_SLAVE_CMD53_END_ADDR -
   * data_left (eh_host_bus_sdio.c), which is 0x1F7C4 for a 60-byte packet.
   * Masking that with ESP_ADDRESS_MASK gives 0x3C4 -- a register -- so the
   * packet was never read out of the companion: its length register kept
   * reporting the same packet, its interrupt stayed asserted, and every later
   * exchange stalled behind it.  Register accesses (read_reg/write_reg) do live
   * in the 10-bit window and are masked there (appendix BY.2).
   */

  /* The port's own mutex is deliberately not taken here.
   *
   * The transport serialises every register and block access with SDIO_DRV_LOCK
   * (eh_host_mutex_lock(sdio_bus_lock)), so this second lock guards nothing --
   * and it became a deadlock point: measured, eh_host_port_sdio_read_reg()
   * entered and never even acquired it, because a transfer that had taken it
   * never returned (appendices BU.6/BV).  Leaving it out keeps the calls
   * serialised by the transport's lock alone.
   */

  ret = sdio_port_transfer(c, false, reg, data, size);

  /* Trace the first few payload reads.
   *
   * These are the transfers the official stack issues once the data path is
   * open, and the pair of addresses printed here is the evidence for the fix:
   * the address the transport asked for (the packet window, 0x1F800 - len of
   * the pending packet) and what the old mask would have turned it into.
   * Capped, so the console stays readable.
   */

  {
    static int traced;

    if (traced < 10)
      {
        syslog(LOG_INFO,
               "%s: read_block addr=0x%05x (masked would be 0x%03x) size=%u "
               "ret=%d first=%02x %02x %02x %02x\n",
               SDIO_TAG, (unsigned)reg, (unsigned)(reg & ESP_ADDRESS_MASK),
               (unsigned)size, ret, data[0], data[1], data[2], data[3]);
        traced++;
      }
  }

  if (lock_required)
    {
      nxmutex_unlock(&c->lock);
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
  int ret;

  if (c == NULL || c->dev == NULL || data == NULL || size == 0)
    {
      return ESP_FAIL;
    }

  (void)lock_required;
  /* Serialise port access with a bounded wait.
   *
   * The transport does have its own SDIO_DRV_LOCK, but the receive and transmit
   * tasks both reach these functions and an interleaved transfer is what hangs
   * (measured: the bring-up ladder's four-byte read of INT_RAW works while the
   * same call from the transport context never completes).  A bounded wait keeps
   * that from turning into the deadlock an unbounded lock produced earlier
   * (appendix BV.3): on timeout the call proceeds and says so.
   */

  /* Block until the bus is ours, and fail the transfer if it never is.
   *
   * This used to time out after a second and then carry on unlocked, which was
   * a workaround for the wedge of the day (appendices BV.3/BU.6).  With that
   * wedge gone it is a correctness bug: the official transport reads registers,
   * writes registers and moves payloads from more than one task, and an
   * unlocked transfer races the one in flight.  Measured with the timed-out
   * version: the first register read after the hand-over succeeded and every
   * one after it failed with -ETIMEDOUT while the data state machine sat busy
   * (appendix BY.9).  Failing the call is the honest outcome when the lock
   * cannot be taken -- corrupting the transfer in flight is not.
   */

  if (lock_required && nxmutex_lock(&c->lock) < 0)
    {
      syslog(LOG_ERR, "%s: %s: cannot take the bus lock\n",
             SDIO_TAG, __func__);
      return ESP_FAIL;
    }

  /* The address is NOT masked here, for the same reason as in read_block():
   * host -> slave payloads go to ESP_SLAVE_CMD53_END_ADDR - data_left, and the
   * mask would redirect them into the register window (appendix BY.2).
   */

  /* The port's own mutex is deliberately not taken here.
   *
   * The transport serialises every register and block access with SDIO_DRV_LOCK
   * (eh_host_mutex_lock(sdio_bus_lock)), so this second lock guards nothing --
   * and it became a deadlock point: measured, eh_host_port_sdio_read_reg()
   * entered and never even acquired it, because a transfer that had taken it
   * never returned (appendices BU.6/BV).  Leaving it out keeps the calls
   * serialised by the transport's lock alone.
   */

  ret = sdio_port_transfer(c, true, reg, data, size);

  if (lock_required)
    {
      nxmutex_unlock(&c->lock);
    }

  return (ret < 0) ? ESP_FAIL : ESP_OK;
}

/* Longest slice handed to the driver's card-interrupt wait, in milliseconds.
 * Keeping it well under 2^31 stops MSEC2TICK() from wrapping.
 */

#define EH_SDIO_INTR_SLICE_MS 1000

/* Slice used when the caller asks to wait forever.  It only matters when the
 * card interrupt does not arrive: the transport then re-reads the companion's
 * interrupt register once per slice, so this is the poll rate (10 Hz -- fast
 * enough for the caps handshake, slow enough not to spin the CPU).
 */

#define EH_SDIO_INTR_POLL_MS 100

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

  /* How the wait ended, counted so the evidence can say whether this link runs
   * on the companion's interrupt or on the polling fallback (appendices BR/BS
   * could not tell the two apart because the fallback reported success).
   */

  static uint32_t intr_hits;
  static uint32_t poll_falls;

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
   * blocking.  Slicing keeps every conversion in range.
   */

  forever   = (ticks_to_wait == EH_HOST_PORT_WAIT_FOREVER);
  remaining = ticks_to_wait;

  for (; ; )
    {
      uint32_t slice;

      if (forever)
        {
          slice = EH_SDIO_INTR_POLL_MS;
        }
      else
        {
          slice = (remaining > EH_SDIO_INTR_SLICE_MS) ? EH_SDIO_INTR_SLICE_MS
                                                      : remaining;
        }

      /* The card interrupt is the path the stack expects: the companion drives
       * DAT1 when it has something for the host.  eh_host_port_sdio_card_init()
       * programs CCCR 0x04 with IENM + FN1 so that this can fire at all, and
       * esp_sdmmc_wait_card_intr() re-arms the controller's SDIO source on entry
       * and masks it again when it asserts, so one assertion is one wakeup.
       */

      if (esp_sdmmc_wait_card_intr(c->dev, slice))
        {
          intr_hits++;

          if (intr_hits <= 8 || (intr_hits % 64) == 0)
            {
              syslog(LOG_INFO, "%s: slave interrupt (hit #%u)\n",
                     SDIO_TAG, (unsigned)intr_hits);
            }

          return ESP_OK;
        }

      /* Nothing inside the slice.
       *
       * A timeout here is not a failure: the transport's next step is to read
       * the companion's interrupt register, which answers the same question.
       * It is counted and reported, though, because "is this link
       * interrupt-driven or polled?" is exactly what was unknown before.
       */

      poll_falls++;

      if (poll_falls <= 8 || (poll_falls % 64) == 0)
        {
          syslog(LOG_INFO, "%s: no card interrupt within %u ms, polling the "
                 "companion (#%u)\n",
                 SDIO_TAG, (unsigned)slice, (unsigned)poll_falls);
        }

      if (forever)
        {
          return ESP_OK;
        }

      if (remaining <= slice)
        {
          return ESP_ERR_TIMEOUT;
        }

      remaining -= slice;
    }
}

#endif /* CONFIG_ESPRESSIF_HOSTED_OFFICIAL */
