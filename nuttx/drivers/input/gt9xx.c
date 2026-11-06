/****************************************************************************
 * drivers/input/gt9xx.c
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

/* Reference:
 * "NuttX RTOS for PinePhone: Touch Panel"
 * https://lupyuen.github.io/articles/touch2
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <sys/types.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <poll.h>
#include <assert.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/kmalloc.h>
#include <nuttx/signal.h>
#include <nuttx/spinlock.h>
#include <nuttx/mutex.h>
#include <nuttx/wdog.h>
#include <nuttx/wqueue.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/input/touchscreen.h>
#include <nuttx/input/gt9xx.h>

/****************************************************************************
 * Pre-Processor Definitions
 ****************************************************************************/

/* Default I2C Frequency is 400 kHz */

#ifndef CONFIG_INPUT_GT9XX_I2C_FREQUENCY
#  define CONFIG_INPUT_GT9XX_I2C_FREQUENCY 400000
#endif

/* Default Number of Poll Waiters is 1 */

#ifndef CONFIG_INPUT_GT9XX_NPOLLWAITERS
#  define CONFIG_INPUT_GT9XX_NPOLLWAITERS 1
#endif

/* Sampling period of the internal event source, in milliseconds.
 *
 * The GT9xx has an INT line, but this board does not route it (see
 * board_gt911_irq_attach(), which is a deliberate no-op), so the driver has to
 * create its own event source or none exists at all.  A plain timer is the
 * only option that works on both wirings: when the board *does* provide an
 * ISR the interrupt handler simply kicks the same worker and the timer becomes
 * a safety net for a missed edge.
 *
 * 8 ms matches a 120 Hz report rate, which is what the controller itself can
 * produce.  The I2C traffic it implies is tiny: one 9-byte read plus one
 * 3-byte write is ~1 ms on this bus at 100 kHz, i.e. ~12 % utilisation.
 * Measured on hardware with a logic analyser: the old 50 ms poll timeout plus
 * 10 ms scheduler ticks yielded 7.1 Hz, so touches shorter than ~140 ms were
 * routinely missed.
 */

#ifndef CONFIG_INPUT_GT9XX_POLL_MS
#  define CONFIG_INPUT_GT9XX_POLL_MS 8
#endif

/* I2C Registers for Goodix GT9XX Touch Panel */

#define GTP_REG_VERSION    0x8140  /* Product ID */
#define GTP_READ_COOR_ADDR 0x814e  /* Touch Panel Status */
#define GTP_POINT1         0x8150  /* Touch Point 1 */

/* Number of touch points reported by this driver.  Although GT9xx hardware
 * can report up to five, only the first point is decoded here, so the value
 * returned through TSIOC_GETMAXPOINTS must stay 1 to keep the sample size
 * the reader is expected to pass in sync with the sample size returned.
 */

#define GTP_MAX_TOUCH_POINTS 1

/* Number of latency histogram buckets exposed through gt9xx_latency_hist().
 * The bucket edges are 2/5/10/20/50/100/500 ms, so index 0 is "under 2 ms"
 * and index 7 is "500 ms or worse".  Splitting the histogram from the average
 * is the whole point: the measured mean transfer is under 1 ms while the mean
 * end-to-end read was 35 ms, and a single average hides that completely.
 */

#define GTP_LAT_BUCKETS 8

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Touch Panel Device */

struct gt9xx_dev_s
{
  /* I2C bus and address for device */

  FAR struct i2c_master_s *i2c;
  uint8_t addr;

  /* Callback for Board-Specific Operations */

  FAR const struct gt9xx_board_s *board;

  /* Device State */

  mutex_t devlock;  /* Mutex to prevent concurrent reads */
  uint8_t cref;     /* Reference Counter for device */
  bool int_pending; /* True if a Touch Interrupt is pending processing */
  uint16_t x;       /* X Coordinate of Last Touch Point */
  uint16_t y;       /* Y Coordinate of Last Touch Point */
  uint8_t flags;    /* Touch Up or Touch Down for Last Touch Point */

  /* Event source.  The panel is polled by the driver itself because the INT
   * line is not routed on this board; see CONFIG_INPUT_GT9XX_POLL_MS.
   */

  struct wdog_s poll_wdog;       /* Re-arms itself every poll_ms */
  struct work_s poll_work;       /* Runs the transfer off the wdog context */
  uint32_t poll_ms;              /* Current sampling period */
  bool polling;                  /* wdog/worker chain is live */

  /* Latest sample, published by the worker and consumed by gt9xx_read().
   * This is what decouples the caller (an LVGL indev callback, a poll()
   * waiter) from the I2C transfer: read() never touches the bus, so a slow or
   * preempted transfer can no longer stall the caller.
   */

  struct touch_sample_s pending;
  bool pending_valid;            /* A sample is waiting to be collected */
  bool contact;                  /* Finger is down as of the last sample */
  uint16_t last_x;               /* Last coordinates, for the synthetic UP */
  uint16_t last_y;

  /* Poll Waiters for device */

  FAR struct pollfd *fds[CONFIG_INPUT_GT9XX_NPOLLWAITERS];

  /* Diagnostics.  Deliberately non-static and deliberately split: the
   * measured transfer time and the measured caller-visible latency differ by
   * more than an order of magnitude on this board, so one number can never
   * tell you which half is broken.
   */

  uint32_t stat_xfers;           /* Completed worker transfers */
  uint32_t stat_xfer_err;        /* Failed worker transfers */
  uint32_t stat_events;          /* DOWN/MOVE/UP samples published */
  uint32_t stat_lock_waits;      /* gather() found the lock busy */
  uint32_t stat_lock_ms_max;     /* worst time spent waiting for the lock */
  uint32_t stat_xfer_us_max;     /* worst single I2C transfer, microseconds */
  uint32_t stat_hist[GTP_LAT_BUCKETS];
  uint32_t stat_ticks;           /* Watchdog firings (sampling attempts) */
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int gt9xx_open(FAR struct file *filep);
static int gt9xx_close(FAR struct file *filep);
static ssize_t gt9xx_read(FAR struct file *filep, FAR char *buffer,
                          size_t buflen);
static int gt9xx_ioctl(FAR struct file *filep, int cmd, unsigned long arg);
static int gt9xx_poll(FAR struct file *filep, FAR struct pollfd *fds,
                      bool setup);
static void gt9xx_poll_timeout(wdparm_t arg);
static void gt9xx_worker(FAR void *arg);
static int gt9xx_gather(FAR struct gt9xx_dev_s *priv);
static void gt9xx_publish(FAR struct gt9xx_dev_s *priv,
                          uint16_t x, uint16_t y);
static void gt9xx_publish_sample(FAR struct gt9xx_dev_s *priv,
                                 FAR const struct touch_sample_s *sample);
static void gt9xx_start_polling(FAR struct gt9xx_dev_s *priv);
static void gt9xx_stop_polling(FAR struct gt9xx_dev_s *priv);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The registered panel, or NULL.  Kept at file scope so gt9xx_stats() can be
 * called by a diagnostic consumer that never owned the file descriptor.
 */

static FAR struct gt9xx_dev_s *g_gt9xx_priv;

/* File Operations for Touch Panel */

static const struct file_operations g_gt9xx_fileops =
{
  gt9xx_open,   /* open */
  gt9xx_close,  /* close */
  gt9xx_read,   /* read */
  NULL,         /* write */
  NULL,         /* seek */
  gt9xx_ioctl,  /* ioctl */
  NULL,         /* truncate */
  NULL,         /* mmap */
  gt9xx_poll,   /* poll */
  NULL,         /* readv */
  NULL          /* writev */
#ifndef CONFIG_DISABLE_PSEUDOFS_OPERATIONS
  , NULL        /* unlink */
#endif
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: gt9xx_i2c_read
 *
 * Description:
 *   Read a Touch Panel Register over I2C.
 *
 * Input Parameters:
 *   dev    - Touch Panel Device
 *   reg    - I2C Register to be read
 *   buf    - Receive Buffer
 *   buflen - Number of bytes to be read
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value is returned on any failure.
 *
 ****************************************************************************/

static int gt9xx_i2c_read(FAR struct gt9xx_dev_s *dev,
                          uint16_t reg,
                          uint8_t *buf,
                          size_t buflen)
{
  int ret;

  /* Send the Register Address, MSB first */

  uint8_t regbuf[2] =
  {
    reg >> 8,   /* First Byte: MSB */
    reg & 0xff  /* Second Byte: LSB */
  };

  /* Compose the I2C Messages */

  struct i2c_msg_s msgv[2] =
  {
    {
      /* Send the I2C Register Address */

      .frequency = CONFIG_INPUT_GT9XX_I2C_FREQUENCY,
      .addr      = dev->addr,
      .flags     = 0,
      .buffer    = regbuf,
      .length    = sizeof(regbuf)
    },
    {
      /* Receive the I2C Register Values */

      .frequency = CONFIG_INPUT_GT9XX_I2C_FREQUENCY,
      .addr      = dev->addr,
      .flags     = I2C_M_READ,
      .buffer    = buf,
      .length    = buflen
    }
  };

  const int msgv_len = sizeof(msgv) / sizeof(msgv[0]);

  iinfo("reg=0x%x, buflen=%ld\n", reg, buflen);
  DEBUGASSERT(dev && dev->i2c && buf);

  /* Execute the I2C Transfer */

  ret = I2C_TRANSFER(dev->i2c, msgv, msgv_len);
  if (ret < 0)
    {
      ierr("I2C Read failed: %d\n", ret);
      return ret;
    }

#ifdef CONFIG_DEBUG_INPUT_INFO
  iinfodumpbuffer("gt9xx_i2c_read", buf, buflen);
#endif /* CONFIG_DEBUG_INPUT_INFO */

  return OK;
}

/****************************************************************************
 * Name: gt9xx_i2c_write
 *
 * Description:
 *   Write to a Touch Panel Register over I2C.
 *
 * Input Parameters:
 *   dev - Touch Panel Device
 *   reg - I2C Register to be written
 *   val - Value to be written
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value is returned on any failure.
 *
 ****************************************************************************/

static int gt9xx_i2c_write(FAR struct gt9xx_dev_s *dev,
                           uint16_t reg,
                           uint8_t val)
{
  int ret;

  /* Register address (MSB first) immediately followed by the value, all in
   * ONE I2C message so the transfer is a single
   * START + addr(W) + reg_hi + reg_lo + val + STOP.
   *
   * This used to be two messages with I2C_M_NOSTART on the second one, which
   * cannot work on this SoC: i2c_transfer() ends every message that does not
   * carry I2C_M_NOSTOP with a STOP, so the bus is already released when the
   * second message is sent.  esp_i2c.c honours I2C_M_NOSTART by skipping the
   * address phase entirely (esp_i2c.c: "if ((msgs[i].flags & I2C_M_NOSTART)
   * != 0) ... esp_i2c_senddata(priv)"), so the controller was asked to push
   * data onto a bus with no active transaction and the wait for completion
   * ran into its timeout.
   *
   * The visible symptom was peculiar: the GT9xx *probe* (a read of 0x8140)
   * and the touch *status* read (0x814e) both succeeded, because reads use the
   * ordinary 2-message write-then-read shape.  Only writes timed out, and the
   * very first thing the old sampling path did after reading the status was
   * gt9xx_set_status(dev, 0) -- so every single read() of /dev/input0 returned
   * -ETIMEDOUT and no touch sample ever reached LVGL.
   */

  uint8_t buf[3] =
  {
    reg >> 8,   /* First byte: register address MSB */
    reg & 0xff, /* Second byte: register address LSB */
    val         /* Third byte: value to be written */
  };

  /* Compose the I2C Message */

  struct i2c_msg_s msgv[1] =
  {
    {
      .frequency = CONFIG_INPUT_GT9XX_I2C_FREQUENCY,
      .addr      = dev->addr,
      .flags     = 0,
      .buffer    = buf,
      .length    = sizeof(buf)
    }
  };

  iinfo("reg=0x%x, val=%d\n", reg, val);
  DEBUGASSERT(dev && dev->i2c);

  /* Execute the I2C Transfer */

  ret = I2C_TRANSFER(dev->i2c, msgv, 1);
  if (ret < 0)
    {
      ierr("I2C Write failed: %d\n", ret);
      return ret;
    }

  return OK;
}

/****************************************************************************
 * Name: gt9xx_probe_device
 *
 * Description:
 *   Read the Product ID from the Touch Panel over I2C.
 *
 * Input Parameters:
 *   dev - Touch Panel Device
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value is returned on any failure.
 *
 ****************************************************************************/

static int gt9xx_probe_device(FAR struct gt9xx_dev_s *dev)
{
  int ret;
  uint8_t id[4];

  /* Read the Product ID */

  ret = gt9xx_i2c_read(dev, GTP_REG_VERSION, id, sizeof(id));
  if (ret < 0)
    {
      ierr("I2C Probe failed: %d\n", ret);
      return ret;
    }

  /* For GT917S: Product ID will be 39 31 37 53, i.e. "917S" */

#ifdef CONFIG_DEBUG_INPUT_INFO
  iinfodumpbuffer("gt9xx_probe_device", id, sizeof(id));
#endif /* CONFIG_DEBUG_INPUT_INFO */

  return OK;
}

/****************************************************************************
 * Name: gt9xx_set_status
 *
 * Description:
 *   Set the Touch Panel Status over I2C.
 *
 * Input Parameters:
 *   dev    - Touch Panel Device
 *   status - Status value to be set
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value is returned on any failure.
 *
 ****************************************************************************/

static int gt9xx_set_status(FAR struct gt9xx_dev_s *dev, uint8_t status)
{
  int ret;

  iinfo("status=%d\n", status);
  DEBUGASSERT(dev);

  /* Write to the Status Register over I2C */

  ret = gt9xx_i2c_write(dev, GTP_READ_COOR_ADDR, status);
  if (ret < 0)
    {
      ierr("Set Status failed: %d\n", ret);
      return ret;
    }

  return OK;
}

/****************************************************************************
 * Name: gt9xx_gather
 *
 * Description:
 *   Perform one sampling transaction against the panel and hand the result
 *   to the event state machine.
 *
 *   The status byte at 0x814E and the first touch point at 0x8150 are
 *   contiguous, so they are fetched in ONE 9-byte read instead of two
 *   separate ones.  The old code used three separate transfers (status,
 *   point, clear); because each transfer re-arbitrates the I2C bus and can be
 *   preempted between messages, that tripled the windows in which the caller's
 *   snapshot could go stale.  Measured with a logic analyser: pure bus time
 *   was 0.99 ms but the end-to-end read ran to 204 ms.
 *
 *   This runs in the LPWORK worker context only.  It must never be called
 *   from gt9xx_read() -- keeping the bus out of the read path is the entire
 *   point of the pending-sample design.
 *
 * Input Parameters:
 *   priv - Touch Panel Device
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value is returned on any failure.
 *
 ****************************************************************************/

static int gt9xx_gather(FAR struct gt9xx_dev_s *priv)
{
  struct touch_sample_s sample;
  uint8_t buf[8];
  uint8_t status_code;
  uint8_t touched_points;
  uint16_t x;
  uint16_t y;
  uint32_t t0;
  int ret;

  DEBUGASSERT(priv != NULL);

  t0 = clock_systime_ticks();

  /* Read the whole status+touch-point block in one transaction.
   *
   * GT9xx register map, sequentially addressable from 0x814E:
   *
   *   0x814E  status       (bit7 = buffer ready, bits3:0 = point count)
   *   0x814F  track id
   *   0x8150  point1 x lo
   *   0x8151  point1 x hi
   *   0x8152  point1 y lo
   *   0x8153  point1 y hi
   *   0x8154  point1 size lo
   *   0x8155  point1 size hi
   *
   * So the coordinates sit at buf[2..5], NOT at buf[1..4]: the status byte and
   * the track id come first.  Reading the point starting at 0x8150 (the old
   * code) put them at 0..3 of that separate buffer; merging the two reads
   * without shifting the indices by two would decode x from the track id and
   * the low half of x -- a plausible-looking but wrong coordinate.
   */

  ret = gt9xx_i2c_read(priv, GTP_READ_COOR_ADDR, buf, sizeof(buf));
  if (ret < 0)
    {
      ierr("Read Touch Panel Status failed: %d\n", ret);
      priv->stat_xfer_err++;
      return ret;
    }

  status_code    = buf[0] & 0x80;
  touched_points = buf[0] & 0x0f;

  /* 0x814E is "read clears" on the GT9xx: the status byte must be written
   * back as 0 after the point data has been consumed, otherwise the
   * controller never latches a new sample.  It is a write, not a re-read, so
   * it cannot be merged into the read above.
   */

  ret = gt9xx_set_status(priv, 0);
  if (ret < 0)
    {
      ierr("Set Touch Panel Status failed: %d\n", ret);
      priv->stat_xfer_err++;
      return ret;
    }

  priv->stat_xfers++;

  {
    uint32_t dt_us = (uint32_t)TICK2USEC(clock_systime_ticks() - t0);

    if (dt_us > priv->stat_xfer_us_max)
      {
        priv->stat_xfer_us_max = dt_us;
      }

    {
      uint32_t dt_ms = (dt_us + 500u) / 1000u;

      if (dt_ms < 2u)
        {
          priv->stat_hist[0]++;
        }
      else if (dt_ms < 5u)
        {
          priv->stat_hist[1]++;
        }
      else if (dt_ms < 10u)
        {
          priv->stat_hist[2]++;
        }
      else if (dt_ms < 20u)
        {
          priv->stat_hist[3]++;
        }
      else if (dt_ms < 50u)
        {
          priv->stat_hist[4]++;
        }
      else if (dt_ms < 100u)
        {
          priv->stat_hist[5]++;
        }
      else if (dt_ms < 500u)
        {
          priv->stat_hist[6]++;
        }
      else
        {
          priv->stat_hist[7]++;
        }
    }
  }

  memset(&sample, 0, sizeof(sample));

  if (status_code != 0 && touched_points >= 1)
    {
      x = (uint16_t)(buf[2] + (buf[3] << 8));
      y = (uint16_t)(buf[4] + (buf[5] << 8));

      /* Bring the coordinates into panel space when the controller is mounted
       * rotated 180 degrees relative to the display (see struct gt9xx_board_s).
       * Without this a touch at the panel's top-left corner lands at the
       * bottom-right of the screen.
       */

      if (priv->board != NULL && priv->board->xmirror > 0 &&
          priv->board->ymirror > 0)
        {
          if (x < priv->board->xmirror)
            {
              x = (uint16_t)(priv->board->xmirror - 1 - x);
            }

          if (y < priv->board->ymirror)
            {
              y = (uint16_t)(priv->board->ymirror - 1 - y);
            }
        }

      gt9xx_publish(priv, x, y);
    }
  else if (priv->contact)
    {
      /* The controller says "no touch" and we believe a finger is down: that
       * is the release edge.  The coordinates are the last known ones, which
       * is what the touchscreen upper half expects for TOUCH_UP.
       */

      memset(&sample, 0, sizeof(sample));
      sample.npoints = 1;
      sample.point[0].id    = 0;
      sample.point[0].x     = priv->last_x;
      sample.point[0].y     = priv->last_y;
      sample.point[0].flags = TOUCH_UP | TOUCH_ID_VALID | TOUCH_POS_VALID;

      priv->contact = false;
      gt9xx_publish_sample(priv, &sample);
    }

  return OK;
}

/****************************************************************************
 * Name: gt9xx_publish
 *
 * Description:
 *   Event state machine: turn a coordinate reading into DOWN or MOVE.
 *
 *   The previous driver only ever produced TOUCH_DOWN and then suppressed
 *   repeated coordinates entirely, so a finger that stayed still and moved
 *   produced no events at all, and a finger that lifted was reported as a
 *   one-shot synthetic UP on the *next* read -- which the application could
 *   only paper over with a fixed 120 ms latch.  Reporting MOVE for as long as
 *   the contact lasts (even when the coordinate is unchanged) is what lets
 *   the application drop those constants: it now knows "still down" from the
 *   event stream instead of guessing from a timer.
 *
 * Input Parameters:
 *   priv - Touch Panel Device
 *   x, y - Coordinates already in panel space
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void gt9xx_publish(FAR struct gt9xx_dev_s *priv,
                          uint16_t x, uint16_t y)
{
  struct touch_sample_s sample;

  memset(&sample, 0, sizeof(sample));
  sample.npoints     = 1;
  sample.point[0].id = 0;
  sample.point[0].x  = x;
  sample.point[0].y  = y;
  sample.point[0].flags = (priv->contact ? TOUCH_MOVE : TOUCH_DOWN) |
                          TOUCH_ID_VALID | TOUCH_POS_VALID;

  priv->contact = true;
  priv->last_x  = x;
  priv->last_y  = y;

  gt9xx_publish_sample(priv, &sample);
}

/****************************************************************************
 * Name: gt9xx_publish_sample
 *
 * Description:
 *   Store the sample for the readers and wake them.
 *
 *   Only the newest sample is kept.  A reader that is not scheduled for a few
 *   milliseconds gets the current finger position rather than a backlog of
 *   stale ones, which is the correct behaviour for a pointer device and is
 *   also what keeps the queue from growing without bound when the UI is busy.
 *
 * Input Parameters:
 *   priv   - Touch Panel Device
 *   sample - Sample to store
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void gt9xx_publish_sample(FAR struct gt9xx_dev_s *priv,
                                 FAR const struct touch_sample_s *sample)
{
  irqstate_t flags;

  flags = enter_critical_section();
  memcpy(&priv->pending, sample, sizeof(priv->pending));
  priv->pending_valid = true;
  priv->int_pending   = true;
  priv->stat_events++;
  leave_critical_section(flags);

  poll_notify(priv->fds, CONFIG_INPUT_GT9XX_NPOLLWAITERS, POLLIN);
}

/****************************************************************************
 * Name: gt9xx_worker
 *
 * Description:
 *   LPWORK callback: run one sampling transaction, then re-arm the watchdog.
 *
 * Input Parameters:
 *   arg - Touch Panel Device
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void gt9xx_worker(FAR void *arg)
{
  FAR struct gt9xx_dev_s *priv = (FAR struct gt9xx_dev_s *)arg;
  uint32_t wait_t0;
  uint32_t wait_ms;
  int ret;

  DEBUGASSERT(priv != NULL);

  /* The watchdog re-arms itself (see gt9xx_poll_timeout), so this worker has
   * no scheduling duty: it only samples.  Chaining the re-arm from here
   * instead would have made the sampling period "tick + queue latency +
   * transfer", which is why an early version plateaued at 23.6 Hz on a 10 ms
   * tick even though the configured period was 8 ms.
   */

  /* The lock only serialises the worker against power transitions in
   * open()/close(); it is held for one transfer and never by a reader, so the
   * wait here is measured and reported rather than being assumed free.
   */

  wait_t0 = clock_systime_ticks();
  ret = nxmutex_lock(&priv->devlock);
  if (ret < 0)
    {
      priv->stat_lock_waits++;
      return;
    }

  wait_ms = (uint32_t)TICK2MSEC(clock_systime_ticks() - wait_t0);

  if (wait_ms > 0)
    {
      priv->stat_lock_waits++;
      if (wait_ms > priv->stat_lock_ms_max)
        {
          priv->stat_lock_ms_max = wait_ms;
        }
    }

  if (priv->cref > 0)
    {
      (void)gt9xx_gather(priv);
    }

  nxmutex_unlock(&priv->devlock);
}

/****************************************************************************
 * Name: gt9xx_poll_timeout
 *
 * Description:
 *   Watchdog callback: queue the sampling worker.
 *
 *   This runs in the timer interrupt, so it may only do the cheap thing:
 *   hand the work to LPWORK.  (Doing the I2C transfer here would put a
 *   ~1 ms bus transaction with a mutex acquisition inside an interrupt.)
 *
 * Input Parameters:
 *   arg - Touch Panel Device
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void gt9xx_poll_timeout(wdparm_t arg)
{
  FAR struct gt9xx_dev_s *priv = (FAR struct gt9xx_dev_s *)arg;

  DEBUGASSERT(priv != NULL);

  if (!priv->polling)
    {
      return;
    }

  /* Re-arm FIRST, then dispatch.
   *
   * The watchdog must not depend on the worker running: a starved worker
   * queue would otherwise stop the whole sampling chain permanently (the
   * re-arm used to live at the end of the worker).  Arming here means the
   * chain keeps ticking no matter what the scheduler does, and a busy queue
   * simply costs one period instead of stopping the driver.
   */

  (void)wd_start(&priv->poll_wdog, MSEC2TICK(priv->poll_ms),
                 gt9xx_poll_timeout, (wdparm_t)priv);

  (void)work_queue(LPWORK, &priv->poll_work, gt9xx_worker, priv, 0);

  priv->stat_ticks++;
}

/****************************************************************************
 * Name: gt9xx_start_polling / gt9xx_stop_polling
 *
 * Description:
 *   Start or stop the internal sampling chain.
 *
 * Input Parameters:
 *   priv - Touch Panel Device
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

static void gt9xx_start_polling(FAR struct gt9xx_dev_s *priv)
{
  if (priv->polling)
    {
      return;
    }

  priv->polling  = true;
  priv->poll_ms  = CONFIG_INPUT_GT9XX_POLL_MS;

  (void)wd_start(&priv->poll_wdog, MSEC2TICK(priv->poll_ms),
                 gt9xx_poll_timeout, (wdparm_t)priv);
}

static void gt9xx_stop_polling(FAR struct gt9xx_dev_s *priv)
{
  priv->polling = false;

  wd_cancel(&priv->poll_wdog);

  (void)work_cancel(LPWORK, &priv->poll_work);
}

/****************************************************************************
 * Name: gt9xx_read
 *
 * Description:
 *   Read a Touch Sample from Touch Panel. Returns either 0 or 1
 *   Touch Points.
 *
 * Input Parameters:
 *   dev    - Touch Panel Device
 *   buffer - Returned Touch Sample (0 or 1 Touch Points)
 *   buflen - Size of buffer
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value is returned on any failure.
 *
 ****************************************************************************/

static ssize_t gt9xx_read(FAR struct file *filep, FAR char *buffer,
                          size_t buflen)
{
  FAR struct inode *inode;
  FAR struct gt9xx_dev_s *priv;
  struct touch_sample_s sample;
  const size_t outlen = sizeof(sample);
  irqstate_t flags;
  bool have;

  /* Returned Touch Sample will have 0 or 1 Touch Points */

  iinfo("buflen=%ld\n", buflen);
  if (buflen < outlen)
    {
      ierr("Buffer should be at least %ld bytes, got %ld bytes\n",
           outlen, buflen);
      return -EINVAL;
    }

  /* Get the Touch Panel Device */

  inode = filep->f_inode;
  DEBUGASSERT(inode->i_private);
  priv = inode->i_private;

  /* Collect the sample the worker published.
   *
   * This used to be a blocking nxmutex_lock() followed by three I2C
   * transfers.  Measured on this board with a logic analyser, the bus part
   * cost 0.99 ms while the call itself took 17.7 ms at p50 and 204 ms at
   * worst -- 97 % of it waiting for the lock or for the scheduler.  Taking the
   * bus out of read() removes that entire class of delay: the transfer now
   * happens on LPWORK whether or not anybody is reading.
   *
   * trylock, not lock: if the worker is mid-transfer the reader must not
   * queue behind it.  Reporting "nothing new" and letting the caller poll
   * again is what keeps a slow transfer from turning into a slow UI.
   */

  if (nxmutex_trylock(&priv->devlock) < 0)
    {
      priv->stat_lock_waits++;
      return -EAGAIN;
    }

  flags = enter_critical_section();
  have = priv->pending_valid;
  if (have)
    {
      memcpy(&sample, &priv->pending, sizeof(sample));
      priv->pending_valid = false;
      priv->int_pending   = false;
    }

  leave_critical_section(flags);

  nxmutex_unlock(&priv->devlock);

  if (!have)
    {
      /* Nothing new since the last read.  Zero points is the correct answer
       * for "no state change", and it is what tells the caller that the
       * finger is still down at the coordinates it already knows about.
       */

      memset(&sample, 0, sizeof(sample));
      sample.npoints = 0;
    }

  memcpy(buffer, &sample, sizeof(sample));
  return outlen;
}

/****************************************************************************
 * Name: gt9xx_ioctl
 *
 * Description:
 *   Report the number of touch points this driver can return.  Readers such
 *   as the LVGL NuttX touchscreen glue size their read buffer with
 *   TSIOC_GETMAXPOINTS, so without this the device cannot be opened by
 *   generic touchscreen clients.
 *
 * Input Parameters:
 *   filep - File Struct for Touch Panel
 *   cmd   - The ioctl command
 *   arg   - The argument of the ioctl command (uint8_t * for
 *           TSIOC_GETMAXPOINTS)
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value is returned on any failure.
 *
 ****************************************************************************/

static int gt9xx_ioctl(FAR struct file *filep, int cmd, unsigned long arg)
{
  iinfo("cmd=%d, arg=%lu\n", cmd, arg);

  switch (cmd)
    {
      case TSIOC_GETMAXPOINTS:
        {
          if (arg == 0)
            {
              return -EINVAL;
            }

          *(FAR uint8_t *)(uintptr_t)arg = GTP_MAX_TOUCH_POINTS;
          return OK;
        }

      default:
        return -ENOTTY;
    }
}

/****************************************************************************
 * Name: gt9xx_open
 *
 * Description:
 *   Open the Touch Panel Device.  If this is the first open, we power on
 *   the Touch Panel, probe for the Touch Panel and enable Touch Panel
 *   Interrupts.
 *
 * Input Parameters:
 *   filep - File Struct for Touch Panel
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value is returned on any failure.
 *
 ****************************************************************************/

static int gt9xx_open(FAR struct file *filep)
{
  FAR struct inode *inode;
  FAR struct gt9xx_dev_s *priv;
  unsigned int use_count;
  int ret;

  /* Get the Touch Panel Device */

  iinfo("\n");
  inode = filep->f_inode;
  DEBUGASSERT(inode->i_private);
  priv = inode->i_private;

  /* Begin Mutex: Lock to prevent concurrent update to Reference Count */

  ret = nxmutex_lock(&priv->devlock);
  if (ret < 0)
    {
      ierr("Lock Mutex failed: %d\n", ret);
      return ret;
    }

  /* Get next Reference Count */

  use_count = priv->cref + 1;
  DEBUGASSERT(use_count < UINT8_MAX && use_count > priv->cref);
  if (use_count == 1)
    {
      /* If first user, power on the Touch Panel */

      DEBUGASSERT(priv->board->set_power != NULL);
      ret = priv->board->set_power(priv->board, true);
      if (ret < 0)
        {
          goto out_lock;
        }

      /* Let Touch Panel power up before probing */

      nxsig_usleep(100 * 1000);

      /* Check that Touch Panel exists on I2C */

      ret = gt9xx_probe_device(priv);
      if (ret < 0)
        {
          /* No such device, power off the Touch Panel */

          priv->board->set_power(priv->board, false);
          goto out_lock;
        }

      /* Enable Touch Panel Interrupts */

      DEBUGASSERT(priv->board->irq_enable);
      priv->board->irq_enable(priv->board, true);

      /* Reset the event state machine and start the sampling chain.
       *
       * The panel was just powered off/on, so any contact state carried over
       * from a previous session is stale: starting in "not touching" avoids
       * synthesising a release for a finger that was never seen.
       */

      priv->contact       = false;
      priv->pending_valid = false;
      priv->int_pending   = false;
      priv->last_x        = 0;
      priv->last_y        = 0;

      gt9xx_start_polling(priv);
    }

  /* Set the Reference Count */

  priv->cref = use_count;

  /* End Mutex: Unlock to allow update to Reference Count */

out_lock:
  nxmutex_unlock(&priv->devlock);
  return ret;
}

/****************************************************************************
 * Name: gt9xx_close
 *
 * Description:
 *   Close the Touch Panel Device.  If this is the final close, we disable
 *   Touch Panel Interrupts and power off the Touch Panel.
 *
 * Input Parameters:
 *   filep - File Struct for Touch Panel
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value is returned on any failure.
 *
 ****************************************************************************/

static int gt9xx_close(FAR struct file *filep)
{
  FAR struct inode *inode;
  FAR struct gt9xx_dev_s *priv;
  int use_count;
  int ret;

  /* Get the Touch Panel Device */

  iinfo("\n");
  inode = filep->f_inode;
  DEBUGASSERT(inode->i_private);
  priv = inode->i_private;

  /* Begin Mutex: Lock to prevent concurrent update to Reference Count */

  ret = nxmutex_lock(&priv->devlock);
  if (ret < 0)
    {
      ierr("Lock Mutex failed: %d\n", ret);
      return ret;
    }

  /* Decrement the Reference Count */

  use_count = priv->cref - 1;
  DEBUGASSERT(use_count >= 0);
  if (use_count == 0)
    {
      /* Stop sampling before cutting the panel's power.
       *
       * The worker would otherwise keep touching the bus for a controller
       * that has just been powered down, and its transfer timeout would hold
       * devlock across the next open().
       */

      gt9xx_stop_polling(priv);

      /* If final user, disable Touch Panel Interrupts */

      DEBUGASSERT(priv->board && priv->board->irq_enable);
      priv->board->irq_enable(priv->board, false);

      /* Power off the Touch Panel */

      DEBUGASSERT(priv->board->set_power);
      priv->board->set_power(priv->board, false);
    }

  /* Set the Reference Count */

  priv->cref = use_count;

  /* End Mutex: Unlock to allow update to Reference Count */

  nxmutex_unlock(&priv->devlock);
  return OK;
}

/****************************************************************************
 * Name: gt9xx_poll
 *
 * Description:
 *   Setup or teardown a poll for the Touch Panel Device.
 *
 * Input Parameters:
 *   filep - File Struct for Touch Panel
 *   fds   - The structure describing the events to be monitored, OR NULL if
 *           this is a request to stop monitoring events.
 *   setup - true: Setup the poll; false: Teardown the poll
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value is returned on any failure.
 *
 ****************************************************************************/

static int gt9xx_poll(FAR struct file *filep, FAR struct pollfd *fds,
                      bool setup)
{
  FAR struct gt9xx_dev_s *priv;
  FAR struct inode *inode;
  bool pending;
  int ret = 0;
  int i;

  /* Get the Touch Panel Device */

  iinfo("setup=%d\n", setup);
  DEBUGASSERT(fds);
  inode = filep->f_inode;
  DEBUGASSERT(inode->i_private);
  priv = inode->i_private;

  /* Begin Mutex: Lock to prevent concurrent update to Poll Waiters */

  ret = nxmutex_lock(&priv->devlock);
  if (ret < 0)
    {
      ierr("Lock Mutex failed: %d\n", ret);
      return ret;
    }

  if (setup)
    {
      /* If Poll Setup: Ignore waits that do not include POLLIN */

      if ((fds->events & POLLIN) == 0)
        {
          ret = -EDEADLK;
          goto out;
        }

      /* Find an available slot for the Poll Waiter */

      for (i = 0; i < CONFIG_INPUT_GT9XX_NPOLLWAITERS; i++)
        {
          /* Found an available slot */

          if (!priv->fds[i])
            {
              /* Bind the poll structure and this slot */

              priv->fds[i] = fds;
              fds->priv = &priv->fds[i];
              break;
            }
        }

      if (i >= CONFIG_INPUT_GT9XX_NPOLLWAITERS)
        {
          /* No slots available */

          fds->priv = NULL;
          ret = -EBUSY;
        }
      else
        {
          /* If Interrupt Pending is set, notify the Poll Waiters */

          pending = priv->int_pending;
          if (pending)
            {
              poll_notify(&fds, 1, POLLIN);
            }
        }
    }
  else if (fds->priv)
    {
      /* If Poll Teardown: Remove the poll setup */

      FAR struct pollfd **slot = (FAR struct pollfd **)fds->priv;
      DEBUGASSERT(slot != NULL);

      *slot = NULL;
      fds->priv = NULL;
    }

  /* End Mutex: Unlock to allow update to Poll Waiters */

out:
  nxmutex_unlock(&priv->devlock);
  return ret;
}

/****************************************************************************
 * Name: gt9xx_isr_handler
 *
 * Description:
 *   Interrupt Handler for Touch Panel.
 *
 * Input Parameters:
 *   irq     - IRQ Number
 *   context - IRQ Context
 *   arg     - Touch Panel Device
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value is returned on any failure.
 *
 ****************************************************************************/

static int gt9xx_isr_handler(int irq, FAR void *context, FAR void *arg)
{
  FAR struct gt9xx_dev_s *priv = (FAR struct gt9xx_dev_s *)arg;
  irqstate_t flags;

  DEBUGASSERT(priv);

  /* Begin Critical Section */

  flags = enter_critical_section();

  /* Set the Interrupt Pending Flag */

  priv->int_pending = true;

  /* End Critical Section */

  leave_critical_section(flags);

  /* Notify the Poll Waiters */

  poll_notify(priv->fds, CONFIG_INPUT_GT9XX_NPOLLWAITERS, POLLIN);
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: gt9xx_register
 *
 * Description:
 *   Register the driver for Goodix GT9XX Touch Panel.  Attach the
 *   Interrupt Handler for the Touch Panel and disable Touch Interrupts.
 *
 * Input Parameters:
 *   devpath      - Device Path (e.g. "/dev/input0")
 *   dev          - I2C Bus
 *   i2c_devaddr  - I2C Address of Touch Panel
 *   board_config - Callback for Board-Specific Operations
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value is returned on any failure.
 *
 ****************************************************************************/

int gt9xx_register(FAR const char *devpath,
                   FAR struct i2c_master_s *i2c_dev,
                   uint8_t i2c_devaddr,
                   const struct gt9xx_board_s *board_config)
{
  struct gt9xx_dev_s *priv;
  int ret = 0;

  iinfo("devpath=%s, i2c_devaddr=%d\n", devpath, i2c_devaddr);
  DEBUGASSERT(devpath != NULL && i2c_dev != NULL && board_config != NULL);

  /* Allocate the Touch Panel Device Structure */

  priv = kmm_zalloc(sizeof(struct gt9xx_dev_s));
  if (!priv)
    {
      ierr("GT9XX Memory Allocation failed\n");
      return -ENOMEM;
    }

  /* Setup the Touch Panel Device Structure */

  priv->addr = i2c_devaddr;
  priv->i2c = i2c_dev;
  priv->board = board_config;
  nxmutex_init(&priv->devlock);

  /* Register the Touch Input Driver */

  ret = register_driver(devpath, &g_gt9xx_fileops, 0666, priv);
  if (ret < 0)
    {
      nxmutex_destroy(&priv->devlock);
      kmm_free(priv);
      ierr("GT9XX Registration failed: %d\n", ret);
      return ret;
    }

  /* Attach the Interrupt Handler */

  DEBUGASSERT(priv->board->irq_attach);
  priv->board->irq_attach(priv->board, gt9xx_isr_handler, priv);

  /* Disable Touch Panel Interrupts */

  DEBUGASSERT(priv->board->irq_enable);
  priv->board->irq_enable(priv->board, false);

  g_gt9xx_priv = priv;

  iinfo("GT9XX Touch Panel registered\n");
  return OK;
}

/****************************************************************************
 * Name: gt9xx_stats
 *
 * Description:
 *   Copy the sampling-path counters to the caller.  Every argument may be
 *   NULL, so a caller that only cares about the histogram can pass NULLs for
 *   the scalar counters (and vice versa).
 *
 *   These exist because the two failure modes are invisible from the outside
 *   and have opposite fixes: a growing xfer_us_max means the bus or the
 *   controller is slow, while a large lock_ms_max or a busy histogram means
 *   the transfer is fine and the *scheduling* is not.  On this board the
 *   measured split was 0.99 ms of bus against 17.7 ms of wall clock, so a
 *   single average would have pointed at the wrong subsystem.
 *
 * Input Parameters:
 *   See include/nuttx/input/gt9xx.h.
 *
 * Returned Value:
 *   Zero (OK) on success; -ENODEV if no panel has been registered.
 *
 ****************************************************************************/

int gt9xx_stats(FAR uint32_t *ticks, FAR uint32_t *xfers,
                FAR uint32_t *xfer_err, FAR uint32_t *events,
                FAR uint32_t *lock_waits, FAR uint32_t *lock_ms_max,
                FAR uint32_t *xfer_us_max, FAR uint32_t *hist)
{
  FAR struct gt9xx_dev_s *priv = g_gt9xx_priv;

  if (priv == NULL)
    {
      return -ENODEV;
    }

  if (xfers != NULL)
    {
      *xfers = priv->stat_xfers;
    }

  if (ticks != NULL)
    {
      *ticks = priv->stat_ticks;
    }

  if (xfer_err != NULL)
    {
      *xfer_err = priv->stat_xfer_err;
    }

  if (events != NULL)
    {
      *events = priv->stat_events;
    }

  if (lock_waits != NULL)
    {
      *lock_waits = priv->stat_lock_waits;
    }

  if (lock_ms_max != NULL)
    {
      *lock_ms_max = priv->stat_lock_ms_max;
    }

  if (xfer_us_max != NULL)
    {
      *xfer_us_max = priv->stat_xfer_us_max;
    }

  if (hist != NULL)
    {
      memcpy(hist, priv->stat_hist, sizeof(priv->stat_hist));
    }

  return OK;
}
