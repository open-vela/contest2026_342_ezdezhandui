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

  /* A GT9xx answers with an ASCII product ID (for example "911" or "917S").
   * Anything else means the address is either empty or another device.
   */

  for (i = 0; i < GT911_ID_LEN; i++)
    {
      if (id[i] < 0x20 || id[i] > 0x7e)
        {
          return false;
        }
    }

  syslog(LOG_INFO, "GT911: product ID \"%c%c%c%c\" at 0x%02x\n",
         id[0], id[1], id[2], id[3], addr);
  return true;
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

  /* The GT911 latches its address from the INT pin level at power-on; the
   * module does not route that pin, so try both documented addresses.
   */

  if (board_gt911_probe(i2c, BOARD_GT911_I2C_ADDR))
    {
      addr = BOARD_GT911_I2C_ADDR;
    }
  else if (board_gt911_probe(i2c, BOARD_GT911_I2C_ADDR_ALT))
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
