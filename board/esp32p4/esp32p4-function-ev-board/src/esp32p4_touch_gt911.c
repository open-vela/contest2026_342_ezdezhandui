/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_touch_gt911.c
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

/* GT911 capacitive touch controller glue for the AML070JGI50-07403L panel
 * module on the ESP32-P4X-C5-Function-EV-Board.
 *
 * Wiring / behaviour:
 *   - I2C0, SDA = GPIO7, SCL = GPIO8 (shared with the ES8311 codec and the
 *     SC2336 camera control port)
 *   - 7-bit address 0x5d; the GT911 also answers on 0x14 depending on the
 *     state of its INT pin at power-on, so both are probed
 *   - Neither the reset nor the interrupt line is routed to the SoC on this
 *     module, so the controller runs in POLLING mode: the driver performs an
 *     I2C read per sample and there is no interrupt to attach.  LVGL's
 *     NuttX touchscreen glue reads /dev/input0 from its own timer, so
 *     polling is sufficient.
 *
 * The protocol itself lives in the shared NuttX GT9xx driver
 * (drivers/input/gt9xx.c); this file only supplies the board callbacks and
 * the bus/address.
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/input/gt9xx.h>

#include <arch/board/board.h>

#include "espressif/esp_i2c.h"

#include "esp32p4-function-ev-board.h"

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_TOUCHSCREEN

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* GT911 register file: product ID block */

#define GT911_REG_VERSION  0x8140
#define GT911_ID_LEN       4

/* GT911 configuration block.  0x8048/0x804A hold the X/Y output resolution --
 * the coordinate space the controller reports touches in.  The GT9xx driver
 * forwards those coordinates unscaled, so when this does not match the panel
 * the touch appears offset or compressed.  Reading it is the only way to tell
 * a controller-side resolution mismatch apart from a wiring/rotation problem.
 *
 *   0x8048  x_max (u16, little endian)
 *   0x804A  y_max (u16, little endian)
 *   0x804C  touch number
 *   0x804D  module switch 1
 *   0x804E  module switch 2
 */

#define GT911_REG_CONFIG   0x8048
#define GT911_CFG_LEN      8

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int board_gt911_irq_attach(const struct gt9xx_board_s *state,
                                  xcpt_t isr, FAR void *arg);
static void board_gt911_irq_enable(const struct gt9xx_board_s *state,
                                   bool enable);
static int board_gt911_set_power(const struct gt9xx_board_s *state, bool on);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct gt9xx_board_s g_gt911_board =
{
  .irq_attach = board_gt911_irq_attach,
  .irq_enable = board_gt911_irq_enable,
  .set_power  = board_gt911_set_power,

  /* The GT911 module on this panel is mounted rotated 180 degrees relative to
   * the display: measured with the tc example, touching the screen's top-left
   * corner reports the maximum coordinates and vice versa.  The controller's
   * own config block (0x8048/0x804A) reads back 1024x600, i.e. the panel's
   * resolution, so mirroring both axes into that window puts touches back
   * under the finger.
   */

  .xmirror    = 1024,
  .ymirror    = 600,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_gt911_irq_attach
 *
 * Description:
 *   No interrupt line is routed to the SoC on this panel module, so there
 *   is nothing to attach.  The GT9xx driver asserts this callback exists at
 *   registration time; the controller is polled instead.
 *
 * Returned Value:
 *   Zero (OK).
 *
 ****************************************************************************/

static int board_gt911_irq_attach(const struct gt9xx_board_s *state,
                                  xcpt_t isr, FAR void *arg)
{
  UNUSED(state);
  UNUSED(isr);
  UNUSED(arg);

  syslog(LOG_INFO,
         "GT911: no INT line routed; running in polling mode\n");
  return OK;
}

/****************************************************************************
 * Name: board_gt911_irq_enable
 *
 * Description:
 *   No interrupt line is routed to the SoC; nothing to enable or disable.
 *
 ****************************************************************************/

static void board_gt911_irq_enable(const struct gt9xx_board_s *state,
                                   bool enable)
{
  UNUSED(state);
  UNUSED(enable);
}

/****************************************************************************
 * Name: board_gt911_set_power
 *
 * Description:
 *   The panel module has no touch power/reset control line; the controller
 *   is powered from the module's own rails.
 *
 * Returned Value:
 *   Zero (OK).
 *
 ****************************************************************************/

static int board_gt911_set_power(const struct gt9xx_board_s *state, bool on)
{
  UNUSED(state);

  syslog(LOG_INFO, "GT911: power %s (module rail, no host control)\n",
         on ? "on" : "off");
  return OK;
}

/****************************************************************************
 * Name: board_gt911_probe
 *
 * Description:
 *   Read the GT911 product ID register at one I2C address.
 *
 * Input Parameters:
 *   i2c  - The I2C0 master instance.
 *   addr - 7-bit I2C address to probe.
 *
 * Returned Value:
 *   True if a plausible GT9xx product ID was read.
 *
 ****************************************************************************/

static bool board_gt911_probe(FAR struct i2c_master_s *i2c, uint8_t addr)
{
  uint8_t reg[2];
  uint8_t id[GT911_ID_LEN];
  struct i2c_msg_s msgv[2];
  int ret;
  int i;

  reg[0] = (GT911_REG_VERSION >> 8) & 0xff;
  reg[1] = GT911_REG_VERSION & 0xff;

  memset(msgv, 0, sizeof(msgv));

  msgv[0].frequency = BOARD_GT911_I2C_FREQUENCY;
  msgv[0].addr      = addr;
  msgv[0].flags     = 0;
  msgv[0].buffer    = reg;
  msgv[0].length    = sizeof(reg);

  msgv[1].frequency = BOARD_GT911_I2C_FREQUENCY;
  msgv[1].addr      = addr;
  msgv[1].flags     = I2C_M_READ;
  msgv[1].buffer    = id;
  msgv[1].length    = sizeof(id);

  ret = I2C_TRANSFER(i2c, msgv, 2);
  if (ret < 0)
    {
      return false;
    }

  /* A GT9xx answers register 0x8140 with a four-byte product ID such as
   * "911" or "917S".
   *
   * ⚠️ The field is NUL-padded.  A GT911 reports "911" followed by 0x00, so
   * requiring all four bytes to be printable ASCII rejects a perfectly
   * healthy controller.  That is exactly what made this board log
   * "GT911 not detected at 0x5d or 0x14" while the part was in fact
   * acknowledging on the bus -- the I2C read succeeded and the validator
   * threw the answer away.
   *
   * Log the raw bytes so the next person sees the data rather than a
   * conclusion.
   */

  syslog(LOG_INFO, "GT911: raw ID at 0x%02x = %02x %02x %02x %02x\n",
         addr, id[0], id[1], id[2], id[3]);

  if (id[0] < '0' || id[0] > '9')
    {
      return false;             /* every GT9xx product ID starts with a digit */
    }

  for (i = 0; i < 3; i++)
    {
      if (id[i] < 0x20 || id[i] > 0x7e)
        {
          return false;
        }
    }

  /* The fourth byte is either NUL padding or another printable character. */

  if (id[3] != 0x00 && (id[3] < 0x20 || id[3] > 0x7e))
    {
      return false;
    }

  syslog(LOG_INFO, "GT911: product ID \"%c%c%c%c\" at 0x%02x\n",
         id[0], id[1], id[2], id[3], addr);
  return true;
}

/****************************************************************************
 * Name: board_i2c_scan
 *
 * Description:
 *   Probe every legal 7-bit address on I2C0 at the given clock rate and log
 *   the ones that acknowledge.
 *
 *   This exists because "the touch controller does not answer" is ambiguous
 *   on a bus shared with the ES8311 codec and the SC2336 camera: a silent
 *   probe cannot distinguish "controller absent / unpowered" from "controller
 *   present but not answering at this clock rate".  Scanning at two rates
 *   separates those two cases, and a bare 1-byte read is used as the probe so
 *   that no device register can be modified by the scan itself.
 *
 * Input Parameters:
 *   i2c       - The I2C0 master instance.
 *   frequency - Bus clock in Hz.
 *
 * Returned Value:
 *   Number of devices that acknowledged.
 *
 ****************************************************************************/

static int board_i2c_scan(FAR struct i2c_master_s *i2c, uint32_t frequency)
{
  struct i2c_msg_s msg;
  uint8_t dummy;
  uint8_t addr;
  int found = 0;

  memset(&msg, 0, sizeof(msg));
  msg.frequency = frequency;
  msg.flags     = I2C_M_READ;
  msg.buffer    = &dummy;
  msg.length    = 1;

  for (addr = 0x08; addr <= 0x77; addr++)
    {
      msg.addr = addr;
      if (I2C_TRANSFER(i2c, &msg, 1) == OK)
        {
          syslog(LOG_INFO, "I2C0 @%u Hz: ACK from 0x%02x\n",
                 (unsigned int)frequency, addr);
          found++;
        }
    }

  syslog(LOG_INFO, "I2C0 @%u Hz: scan done, %d device(s)\n",
         (unsigned int)frequency, found);
  return found;
}

/****************************************************************************
 * Name: board_gt911_probe_retry
 *
 * Description:
 *   board_gt911_probe() with a few retries.  The GT911 needs some time after
 *   its supply settles before it answers, and the module's reset is only an
 *   RC network (no host control line), so a single early probe can miss it.
 *
 ****************************************************************************/

static bool board_gt911_probe_retry(FAR struct i2c_master_s *i2c,
                                    uint8_t addr)
{
  int i;

  for (i = 0; i < 5; i++)
    {
      if (board_gt911_probe(i2c, addr))
        {
          return true;
        }

      up_mdelay(20);
    }

  return false;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_touchscreen_init
 *
 * Description:
 *   Probe the GT911 on the shared I2C0 bus and register /dev/input0.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int board_touchscreen_init(void)
{
  FAR struct i2c_master_s *i2c;
  uint8_t addr;
  int ret;

  i2c = esp_i2cbus_initialize(ESPRESSIF_I2C0);
  if (i2c == NULL)
    {
      syslog(LOG_ERR, "ERROR: GT911: failed to get I2C0 bus\n");
      return -ENODEV;
    }

  /* Record what is actually on the bus before probing, at both the speed we
   * used to run at and the speed we now use.  This is the difference between
   * "the controller is not wired" and "the controller cannot keep up".
   */

  board_i2c_scan(i2c, 400000);
  board_i2c_scan(i2c, BOARD_GT911_I2C_FREQUENCY);

  /* The GT911 latches its address from the INT pin level at power-on; the
   * module does not route that pin, so try both documented addresses.
   */

  if (board_gt911_probe_retry(i2c, BOARD_GT911_I2C_ADDR))
    {
      addr = BOARD_GT911_I2C_ADDR;
    }
  else if (board_gt911_probe_retry(i2c, BOARD_GT911_I2C_ADDR_ALT))
    {
      addr = BOARD_GT911_I2C_ADDR_ALT;
    }
  else
    {
      /* Neither address answered.  Register anyway so that /dev/input0
       * exists for diagnostics; the driver re-probes on open().
       */

      addr = BOARD_GT911_I2C_ADDR;
      syslog(LOG_ERR,
             "ERROR: GT911 not detected at 0x%02x or 0x%02x "
             "(check I2C0 GPIO%d/GPIO%d and the panel FPC)\n",
             BOARD_GT911_I2C_ADDR, BOARD_GT911_I2C_ADDR_ALT,
             BOARD_I2C0_SDA_GPIO, BOARD_I2C0_SCL_GPIO);
    }

  /* Report the controller's own coordinate space.  The GT9xx driver passes
   * coordinates through untouched, so this is what a touch event is expressed
   * in: if x_max/y_max are not the panel's 1024x600 the pointer will land
   * offset from the finger and the coordinates must be scaled (or the
   * controller reconfigured).
   */

  {
    uint8_t reg[2];
    uint8_t cfg[GT911_CFG_LEN];
    struct i2c_msg_s msgv[2];

    reg[0] = (GT911_REG_CONFIG >> 8) & 0xff;
    reg[1] = GT911_REG_CONFIG & 0xff;
    memset(cfg, 0, sizeof(cfg));

    memset(msgv, 0, sizeof(msgv));
    msgv[0].frequency = BOARD_GT911_I2C_FREQUENCY;
    msgv[0].addr      = addr;
    msgv[0].flags     = 0;
    msgv[0].buffer    = reg;
    msgv[0].length    = sizeof(reg);
    msgv[1].frequency = BOARD_GT911_I2C_FREQUENCY;
    msgv[1].addr      = addr;
    msgv[1].flags     = I2C_M_READ;
    msgv[1].buffer    = cfg;
    msgv[1].length    = sizeof(cfg);

    if (I2C_TRANSFER(i2c, msgv, 2) == OK)
      {
        syslog(LOG_INFO,
               "GT911: cfg x_max=%u y_max=%u touch_num=%u "
               "(panel is 1024x600)\n",
               (unsigned)(cfg[0] | (cfg[1] << 8)),
               (unsigned)(cfg[2] | (cfg[3] << 8)), cfg[4]);
      }
    else
      {
        syslog(LOG_WARNING, "GT911: config block read failed\n");
      }
  }

  ret = gt9xx_register("/dev/input0", i2c, addr, &g_gt911_board);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: failed to register GT911: %d\n", ret);
      return ret;
    }

  syslog(LOG_INFO, "GT911 touchscreen registered at /dev/input0 (0x%02x)\n",
         addr);
  return OK;
}

#endif /* CONFIG_ESP32P4_FUNCTION_EV_BOARD_TOUCHSCREEN */
