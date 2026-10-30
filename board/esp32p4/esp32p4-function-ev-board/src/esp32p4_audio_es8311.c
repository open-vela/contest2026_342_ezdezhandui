/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_audio_es8311.c
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
 * Audio path of the ESP32-P4X-C5-Function-EV-Board
 *
 *   ES8311 (mono codec)  --I2C0-->  control   (SDA GPIO7 / SCL GPIO8, 0x18)
 *   ES8311               --I2S0-->  data      (GDMA backed)
 *   NS4150B (class D)    <--GPIO53--  PA_CTRL
 *
 * Three things about this board are easy to get wrong, so they are spelled
 * out here rather than left to the reader:
 *
 * 1. The I2S nets in the board schematic are named from the *codec's* point
 *    of view, which is the opposite of the host's:
 *
 *      schematic net   ES8311 pin   direction at the codec   ESP32-P4
 *      --------------  -----------  ---------------------   ---------------
 *      I2S_MCLK        MCLK         in  (master clock)      GPIO13  out
 *      I2S_SCLK        SCLK         in  (bit clock)         GPIO12  out
 *      I2S_LRCK        LRCK         in  (word select)       GPIO10  out
 *      I2S_DSDIN       DSDIN        in  (DAC data)          GPIO9   out
 *      I2S_ASDOUT      ASDOUT       out (ADC data)          GPIO11  in
 *
 *    So the host transmit line is GPIO9 ("DSDIN": the codec's DAC *input*)
 *    and the host receive line is GPIO11 ("ASDOUT": the codec's ADC
 *    *output*).  Wiring these two the obvious way round gives a silent
 *    speaker and is a classic bring-up trap on this family of boards.
 *
 * 2. MCLK is not optional.  The ES8311 needs a master clock and the NuttX
 *    driver is built for the "from MCLK pin" case
 *    (CONFIG_ES8311_SRC_MCLK).  With MCLK supplied, the codec derives its
 *    internal clocks from the ratio MCLK / sample rate, so the coefficient
 *    table in drivers/audio/es8311.h only accepts the audio rates whose
 *    256x multiple the I2S peripheral can actually generate
 *    (see g_es8311_coeff_div: 8k/12k/16k/24k/32k/48k at 256x, and the
 *    44.1k/22.05k/11.025k family at other multiples).  esp_i2s.c sets
 *    mclk = rate * 256 for 16-bit data, which is exactly what those entries
 *    expect.
 *
 * 3. The signal path is 16-bit slots, but the ES8311 is a mono codec and
 *    its DAC reads the left slot by default, so the I2S port must be
 *    configured for two slots (the driver's default) and callers should
 *    supply interleaved stereo.  Playback therefore wants 16-bit stereo
 *    WAV at 16 kHz or 24 kHz -- 24 kHz is the better default for speech
 *    because it halves the APLL frequency and the PSRAM bandwidth.
 *
 * Data movement itself needs no code here: esp_i2s.c owns I2S0 and drives
 * it from the GDMA engine, and drivers/audio/es8311.c owns the codec
 * register sequence.  This file is deliberately only the board-level glue
 * between them, which is what keeps the port from forking either driver.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <syslog.h>

#include <nuttx/audio/audio.h>
#include <nuttx/audio/es8311.h>
#include <nuttx/audio/i2s.h>
#include <nuttx/audio/pcm.h>
#include <nuttx/i2c/i2c_master.h>

#include "espressif/esp_gpio.h"
#include "espressif/esp_i2c.h"
#include "espressif/esp_i2s.h"
#include "esp_board_i2c.h"

#include <arch/board/esp32p4_audio_es8311.h>

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_AUDIO_ES8311

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* I2C control interface.  The codec hangs off the board's single shared
 * control bus, which it shares with the GT911 touch controller and the
 * SC2336 camera SCCB port (see board.h).  An address scan of the assembled
 * board reports the ES8311 at 0x18.
 */

#define ES8311_I2C_PORT        ESPRESSIF_I2C0
#define ES8311_I2C_ADDRESS     0x18
#define ES8311_I2C_FREQUENCY   100000

/* I2S data interface (I2S0).  See the net/direction table at the top of
 * this file before changing any of the four data pins.
 */

#define ES8311_I2S_PORT        ESPRESSIF_I2S0

#define ES8311_GPIO_MCLK       13  /* I2S_MCLK   -> codec MCLK   */
#define ES8311_GPIO_BCLK       12  /* I2S_SCLK   -> codec SCLK   */
#define ES8311_GPIO_WS         10  /* I2S_LRCK   -> codec LRCK   */
#define ES8311_GPIO_DOUT       9   /* I2S_DSDIN  -> codec DSDIN  */
#define ES8311_GPIO_DIN        11  /* I2S_ASDOUT <- codec ASDOUT */

/* NS4150B power-amplifier enable (schematic net PA_CTRL, active high, with a
 * 10 kOhm pull-down on the board so the amplifier is off until driven).
 *
 * GPIO53 is shared with the ESP32-P4 "control the PA enable" strap: on the
 * stock board, resistor R79 ties the net to this GPIO.  Moving R79 to R85
 * frees the pin for J1 pin 35 and leaves the amplifier permanently enabled
 * by hardware -- in that configuration, driving the pin has no effect, but
 * it also does no harm.
 */

#define ES8311_GPIO_PA         53

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct es8311_lower_s g_es8311_lower;
static bool g_es8311_initialized;
static bool g_pa_configured;
static bool g_pa_enabled;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp32p4_audio_pa_set
 *
 * Description:
 *   Drive the NS4150B power-amplifier enable line.  See the header for the
 *   rationale; this function itself only owns the GPIO.
 *
 ****************************************************************************/

int esp32p4_audio_pa_set(bool enable)
{
  int ret;

  if (!g_pa_configured)
    {
      ret = esp_configgpio(ES8311_GPIO_PA, OUTPUT);
      if (ret < 0)
        {
          syslog(LOG_ERR,
                 "ERROR: failed to configure PA enable GPIO%d: %d\n",
                 ES8311_GPIO_PA, ret);
          return ret;
        }

      g_pa_configured = true;
    }

  esp_gpiowrite(ES8311_GPIO_PA, enable);
  g_pa_enabled = enable;

  syslog(LOG_INFO, "ES8311: speaker PA %s (GPIO%d)\n",
         enable ? "enabled" : "disabled", ES8311_GPIO_PA);

  return OK;
}

/****************************************************************************
 * Name: esp32p4_audio_es8311_initialize
 *
 * Description:
 *   Bring up the ES8311 codec path and register /dev/audio/pcm0.  See the
 *   header for the contract.
 *
 ****************************************************************************/

int esp32p4_audio_es8311_initialize(void)
{
  FAR struct audio_lowerhalf_s *es8311;
  FAR struct audio_lowerhalf_s *pcm;
  FAR struct i2s_dev_s *i2s;
  FAR struct i2c_master_s *i2c;
  int ret;

  /* Registering an audio device is a one-shot operation: audio_register()
   * would fail on a second attempt and the I2S/I2C handles must not be
   * acquired twice.  NSH can reach bring-up code more than once when the
   * board is re-initialized, so guard it.
   */

  if (g_es8311_initialized)
    {
      return OK;
    }

  /* Acquire the I2S0 data channel.  esp_i2s.c takes its pins from
   * CONFIG_ESPRESSIF_I2S0_* and consumes PCM buffers through GDMA, so there
   * is nothing to configure here beyond the port number.
   */

  i2s = esp_i2sbus_initialize(ES8311_I2S_PORT);
  if (i2s == NULL)
    {
      syslog(LOG_ERR, "ERROR: failed to initialize I2S%d\n",
             ES8311_I2S_PORT);
      return -ENODEV;
    }

  /* Acquire the shared I2C0 control bus.  It is reference counted, so the
   * GT911 and SC2336 drivers holding it do not conflict with this use.
   */

  i2c = esp_i2cbus_initialize(ES8311_I2C_PORT);
  if (i2c == NULL)
    {
      syslog(LOG_ERR, "ERROR: failed to initialize I2C%d\n",
             ES8311_I2C_PORT);
      ret = -ENODEV;
      goto errout_i2s;
    }

  /* Keep the speaker quiet while the codec is reprogrammed.  The codec reset
   * sequence writes its whole register map, which would otherwise be audible
   * as a click through the already-enabled amplifier.
   */

  if (g_pa_enabled)
    {
      esp32p4_audio_pa_set(false);
    }

  /* Wrap the codec.  es8311_initialize() programs the ES8311 over I2C and
   * then configures the I2S port for the sample rate and word width the
   * application asks for at configure() time.
   */

  g_es8311_lower.address   = ES8311_I2C_ADDRESS;
  g_es8311_lower.frequency = ES8311_I2C_FREQUENCY;

  es8311 = es8311_initialize(i2c, i2s, &g_es8311_lower);
  if (es8311 == NULL)
    {
      syslog(LOG_ERR, "ERROR: failed to initialize the ES8311 codec\n");
      ret = -ENODEV;
      goto errout_i2c;
    }

  /* The I2C handle stays acquired for the lifetime of the system on purpose.
   *
   * es8311_initialize() stores the pointer we hand it and uses it again for
   * every later volume or mixer ioctl; it does not take a reference of its
   * own.  esp_i2cbus_initialize() is reference counted and drops the bus on
   * the last uninitialize, so releasing our handle here would leave the codec
   * with a dangling pointer and turn the next volume change into a
   * use-after-free.  Callers of this function are therefore not expected to
   * release anything.
   */

  /* Give the codec a PCM front end so that WAV files can be written to the
   * device directly.
   *
   * CONFIG_SYSTEM_NXLOOPER feeds raw (non-WAV) buffers, which the PCM decoder
   * would reject, so in that configuration the codec is registered bare --
   * the same trade-off drivers/audio/pcm.c documents.
   */

#ifdef CONFIG_SYSTEM_NXLOOPER
  ret = audio_register("pcm0", es8311);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: failed to register /dev/audio/pcm0: %d\n",
             ret);
      goto errout_i2s;
    }
#else
  pcm = pcm_decode_initialize(es8311);
  if (pcm == NULL)
    {
      syslog(LOG_ERR, "ERROR: failed to create the PCM decoder\n");
      ret = -ENODEV;
      goto errout_i2s;
    }

  ret = audio_register("pcm0", pcm);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: failed to register /dev/audio/pcm0: %d\n",
             ret);
      goto errout_i2s;
    }
#endif

  /* Capture is not registered.
   *
   * The capture path is reachable -- a second es8311 instance opens
   * /dev/audio/pcm_in0, CONFIGURE for AUDIO_TYPE_INPUT succeeds (it used to
   * fail with ERANGE; see the driver-side fix in drivers/audio/es8311.c), and
   * buffers are accepted with the correct mode (ES_MODULE_ADC) -- but the RX
   * DMA never completes, so no buffer is ever handed back and the caller ends
   * up holding the console.
   *
   * Registering a device that hangs when used is worse than not registering
   * it, so it stays out until the missing piece is found.  What is already
   * known is written down in docs/02_驱动实现/ES8311音频驱动.md appendix C so the next attempt starts
   * from there instead of from scratch.
   */

  g_es8311_initialized = true;

  /* Only now let the amplifier drive the speaker.  Any turn-on transient has
   * already been absorbed by the codec's own mute state.
   */

  ret = esp32p4_audio_pa_set(true);
  if (ret < 0)
    {
      /* The audio path itself is usable; without the PA it is just quiet.
       * Report it but do not fail the whole bring-up.
       */

      syslog(LOG_WARNING,
             "WARNING: audio registered but the PA could not be enabled\n");
    }

  syslog(LOG_INFO,
         "ES8311 ready: I2S%d (MCLK=%d BCLK=%d WS=%d DOUT=%d DIN=%d), "
         "I2C%d addr 0x%02x -> /dev/audio/pcm0\n",
         ES8311_I2S_PORT,
         ES8311_GPIO_MCLK, ES8311_GPIO_BCLK, ES8311_GPIO_WS,
         ES8311_GPIO_DOUT, ES8311_GPIO_DIN,
         ES8311_I2C_PORT, ES8311_I2C_ADDRESS);


  return OK;

errout_i2c:
  esp_i2cbus_uninitialize(i2c);

errout_i2s:
  return ret;
}

#endif /* CONFIG_ESP32P4_FUNCTION_EV_BOARD_AUDIO_ES8311 */
