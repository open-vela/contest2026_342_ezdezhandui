/****************************************************************************
 * drivers/video/sc2336.c
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
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <sys/types.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <debug.h>
#include <unistd.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/mutex.h>
#include <nuttx/signal.h>
#include <nuttx/wqueue.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/video/video.h>
#include <nuttx/video/imgsensor.h>
#include <nuttx/video/sc2336.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifdef CONFIG_SC2336_I2CADDR
#  define SC2336_I2C_ADDRESS  CONFIG_SC2336_I2CADDR
#else
#  define SC2336_I2C_ADDRESS  SC2336_I2C_ADDR
#endif

#ifdef CONFIG_SC2336_FREQUENCY
#  define SC2336_I2C_FREQUENCY CONFIG_SC2336_FREQUENCY
#else
#  define SC2336_I2C_FREQUENCY 100000
#endif

/* Register address width: the SC2336 uses 16-bit register addresses and
 * 8-bit data on its SCCB interface (which is I2C-like but not fully
 * compliant - hence the dedicated bus access helpers below).
 */

#define SC2336_REGADDR_LEN    2

/* Sensor identification. */

#define SC2336_REG_SENSOR_ID_H   0x3107
#define SC2336_REG_SENSOR_ID_L   0x3108
#define SC2336_PID               0xcb3a

/* Registers used by the driver after the initialization table has been
 * applied.
 */

#define SC2336_REG_SLEEP_MODE    0x0100  /* Stream on/off (1/0) */
#define SC2336_REG_SHUTTER_H     0x3e00  /* Exposure [15:12] */
#define SC2336_REG_SHUTTER_M     0x3e01  /* Exposure [11:4]  */
#define SC2336_REG_SHUTTER_L     0x3e02  /* Exposure [3:0] in [7:4] */
#define SC2336_REG_FLIP_MIRROR   0x3221  /* [2:1] mirror, [6:5] vflip */

/* Frame timing of the 1080p30 mode. */

#define SC2336_HTS               2240
#define SC2336_VTS               1500
#define SC2336_FPS               30

/* Settling time after stream-on: the first frames after leaving sleep are not
 * valid, so the capture path waits a few frame periods before the first
 * buffer is expected to be complete.
 */

#define SC2336_STREAM_SETTLE_MS  100

/* Settle time between the mode table (MIPI PLL enable) and stream-on. */
#define SC2336_PLL_SETTLE_MS     50

/* Exposure limits: the integrated exposure is expressed in lines and has to
 * stay below the frame length.  The vendor driver uses VTS - 6 as maximum
 * and 8 as minimum, with 0x37e (894 lines) as the power-on default.
 */

#define SC2336_EXPOSURE_MIN      8
#define SC2336_EXPOSURE_MAX      (SC2336_VTS - 6)
#define SC2336_EXPOSURE_DEFAULT  0x37e

/* Tables are applied with an explicit length (SC2336_NREGS_1080P), not a sentinel.
 * The old 0xffff terminator was forgotten once already, which made the writer
 * walk past the end of the array and issue wild SCCB writes; a length cannot
 * be forgotten without the compiler noticing the array is empty.
 *
 * A 0xfffe entry delays for the number of milliseconds stored in val instead
 * of writing a register.
 */

#define SC2336_REG_DELAY         0xfffe

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct sc2336_reg_s
{
  uint16_t reg;
  uint8_t  val;
};

struct sc2336_mode_s
{
  uint16_t width;
  uint16_t height;
  FAR const struct sc2336_reg_s *table;
  size_t   nentries;
};

struct sc2336_dev_s
{
  struct imgsensor_s sensor;
  FAR struct i2c_master_s *i2c;
  uint8_t addr;

  /* Serialises SCCB access and the streaming state machine.  Created by
   * sc2336_initialize(); the instance is a singleton, so the mutex lives as
   * long as the driver.
   */

  mutex_t  lock;
  bool     lock_ready;

  /* Values written by start_capture() and reported through get_value(). */

  uint32_t exposure;
  bool     hflip;
  bool     vflip;

  /* Mode table currently programmed into the sensor.  The table is applied
   * at VIDIOC_S_FMT time, before the CSI controller is created/started, so
   * the D-PHY is initialised with a stable sensor clock (as esp_video does).
   */

  FAR const struct sc2336_mode_s *applied_mode;

  bool     streaming;
  bool     detected;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Initialization sequence for 1920x1080 @ 30 fps, 2 MIPI data lanes, RAW10.
 *
 * Source: Espressif esp-video-components / esp_cam_sensor v2.5.0,
 *   sensors/sc2336/private_include/
 *     sc2336_mipi_2lane_24Minput_1920x1080_raw10_30fps.h
 * (blob 11875772d8d7fab10150f3f914bab6610acee520).  The table is reproduced
 * verbatim, in the original order; the file's own header comment identifies
 * the tuning set as "cleaned_0x02_SC2336_MIPI_24Minput_2lane_405Mbps_10bit_
 * 1920x1080_30fps", i.e. 405 Mbps per lane.
 *
 * Note that 0x36e9 and 0x37f9 are written twice on purpose (0x80 unlocks the
 * PLL while the table is being applied, 0x53 locks it again at the end).
 */

/* 1280x720 @ 30 fps, 2 MIPI data lanes, RAW8, 336 Mbps.
 *
 * This is the mode the factory ESP-IDF firmware runs on this board; it was
 * verified against the reference capture_stream example together with a CSI
 * host/bridge register snapshot.  Source of the table: esp-video-components,
 *   sc2336_mipi_2lane_24Minput_1280x720_raw8_30fps.h
 */

static const struct sc2336_reg_s g_sc2336_720p30_raw8[] =
{
  /* The soft reset (0x0103) is issued by sc2336_probe() with its 5 ms
   * settling delay, exactly like the reference esp_cam_sensor driver, whose
   * write trace contains a single 0x0103 followed by a settle gap.  Issuing
   * it a second time here and continuing immediately would restart the
   * sensor's internal state machine while the PLL/analogue blocks are being
   * programmed.
   */

  {0x0100, 0x00}, {0x36e9, 0x80}, {0x37f9, 0x80}, {0x301f, 0x8e}, {0x3031, 0x08},
  {0x3037, 0x00}, {0x3106, 0x05}, {0x3200, 0x01}, {0x3201, 0x34}, {0x3202, 0x00}, {0x3203, 0xb4},
  {0x3204, 0x06}, {0x3205, 0x53}, {0x3206, 0x03}, {0x3207, 0x8b}, {0x3208, 0x05}, {0x3209, 0x00},
  {0x320a, 0x02}, {0x320b, 0xd0}, {0x320c, 0x08}, {0x320d, 0xc0}, {0x320e, 0x04}, {0x320f, 0xe2},
  {0x3210, 0x00}, {0x3211, 0x10}, {0x3212, 0x00}, {0x3213, 0x04}, {0x3248, 0x04}, {0x3249, 0x0b},
  {0x3253, 0x08}, {0x3301, 0x09}, {0x3302, 0xff}, {0x3303, 0x10}, {0x3306, 0x68}, {0x3307, 0x02},
  {0x330a, 0x01}, {0x330b, 0x18}, {0x330c, 0x16}, {0x330d, 0xd3}, {0x3318, 0x02}, {0x3321, 0x0a},
  {0x3327, 0x0e}, {0x332b, 0x12}, {0x3333, 0x10}, {0x3334, 0x40}, {0x335e, 0x06}, {0x335f, 0x0a},
  {0x3364, 0x1f}, {0x337c, 0x02}, {0x337d, 0x0e}, {0x3390, 0x09}, {0x3391, 0x0f}, {0x3392, 0x1f},
  {0x3393, 0x20}, {0x3394, 0x20}, {0x3395, 0xff}, {0x33a2, 0x04}, {0x33b1, 0x80}, {0x33b2, 0x68},
  {0x33b3, 0x42}, {0x33f9, 0x78}, {0x33fb, 0xe0}, {0x33fc, 0x0f}, {0x33fd, 0x1f}, {0x349f, 0x03},
  {0x34a6, 0x0f}, {0x34a7, 0x1f}, {0x34a8, 0x42}, {0x34a9, 0x06}, {0x34aa, 0x01}, {0x34ab, 0x28},
  {0x34ac, 0x01}, {0x34ad, 0x90}, {0x3630, 0xf4}, {0x3633, 0x22}, {0x3639, 0xf4}, {0x363c, 0x47},
  {0x3670, 0x09}, {0x3674, 0xf4}, {0x3675, 0xfb}, {0x3676, 0xed}, {0x367c, 0x09}, {0x367d, 0x0f},
  {0x3690, 0x22}, {0x3691, 0x22}, {0x3692, 0x22}, {0x3698, 0x89}, {0x3699, 0x96}, {0x369a, 0xd0},
  {0x369b, 0xd0}, {0x369c, 0x09}, {0x369d, 0x0f}, {0x36a2, 0x09}, {0x36a3, 0x0f}, {0x36a4, 0x1f},
  {0x36d0, 0x01}, {0x36ea, 0x0e}, {0x36eb, 0x0a}, {0x36ec, 0x1a}, {0x36ed, 0x18}, {0x3722, 0xe1},
  {0x3724, 0x41}, {0x3725, 0xc1}, {0x3728, 0x20}, {0x37fa, 0x15}, {0x37fb, 0x32}, {0x37fc, 0x11},
  {0x37fd, 0x17}, {0x3900, 0x0d}, {0x3905, 0x98}, {0x391b, 0x81}, {0x391c, 0x10}, {0x3933, 0x81},
  {0x3934, 0xc5}, {0x3940, 0x68}, {0x3941, 0x00}, {0x3942, 0x01}, {0x3943, 0xc6}, {0x3952, 0x02},
  {0x3953, 0x0f}, {0x3e01, 0x37}, {0x3e02, 0xe0}, {0x3e08, 0x1f}, {0x3e1b, 0x14}, {0x4509, 0x38},
  {0x4819, 0x06}, {0x481b, 0x04}, {0x481d, 0x0c}, {0x481f, 0x03}, {0x4821, 0x0a}, {0x4823, 0x03},
  {0x4825, 0x03}, {0x4827, 0x03}, {0x4829, 0x05}, {0x5799, 0x06}, {0x5ae0, 0xfe}, {0x5ae1, 0x40},
  {0x5ae2, 0x30}, {0x5ae3, 0x28}, {0x5ae4, 0x20}, {0x5ae5, 0x30}, {0x5ae6, 0x28}, {0x5ae7, 0x20},
  {0x5ae8, 0x3c}, {0x5ae9, 0x30}, {0x5aea, 0x28}, {0x5aeb, 0x3c}, {0x5aec, 0x30}, {0x5aed, 0x28},
  {0x5aee, 0xfe}, {0x5aef, 0x40}, {0x5af4, 0x30}, {0x5af5, 0x28}, {0x5af6, 0x20}, {0x5af7, 0x30},
  {0x5af8, 0x28}, {0x5af9, 0x20}, {0x5afa, 0x3c}, {0x5afb, 0x30}, {0x5afc, 0x28}, {0x5afd, 0x3c},
  {0x5afe, 0x30}, {0x5aff, 0x28}, {0x36e9, 0x54}, {0x37f9, 0x54},
};

#define SC2336_NREGS_720P_RAW8 (sizeof(g_sc2336_720p30_raw8) / \
                                sizeof(g_sc2336_720p30_raw8[0]))

/* Supported frame sizes.  Only the mode that has a verified register
 * table is listed; the SC2336 has further modes (720p, 800x800, 640x480,
 * 1080p25) that would only need another table here.
 */

static const struct sc2336_mode_s g_sc2336_modes[] =
{
  {1280, 720, g_sc2336_720p30_raw8, SC2336_NREGS_720P_RAW8},
};

#define SC2336_NMODES (sizeof(g_sc2336_modes) / sizeof(g_sc2336_modes[0]))

/* Frame sizes advertised through VIDIOC_ENUM_FRAMESIZES.  v4l2_cap.c takes
 * these from imgsensor_s.frmsizes; without this array the V4L2 upper half
 * falls back to a hard-coded QVGA and the enumeration does not describe the
 * mode the driver actually programs.
 */

static const struct v4l2_frmsizeenum g_sc2336_frmsizes[] =
{
  {
    .index        = 0,
    .buf_type     = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    .pixel_format = V4L2_PIX_FMT_SBGGR8,
    .type         = V4L2_FRMSIZE_TYPE_DISCRETE,
    .discrete     =
    {
      .width  = 1280,
      .height = 720
    }
  }
};

#define SC2336_NFRMSIZES \
  (sizeof(g_sc2336_frmsizes) / sizeof(g_sc2336_frmsizes[0]))

/* Advertised frame intervals.
 *
 * ⚠️ 这个数组不是"可选的元数据", 缺了它 VIDIOC_S_FMT 会直接失败。
 *
 * v4l2_cap.c 的 initialize_frame_setting() 在驱动没有声明 frmintervals 时,
 * 会退回到一个**写死的 15 fps** 默认值 (v4l2_cap.c:967-968):
 *
 *     else
 *       {
 *         interval->denominator = 15;
 *         interval->numerator   = 1;
 *       }
 *
 * 而这个 15 fps 会被原样送进 sc2336_validate_frame_setting(), 那里的
 * 规则是"只接受 1/30"(见 SC2336_FPS) -> 校验失败 -> -EINVAL。
 * 应用侧看到的只是 `Failed to VIDIOC_S_FMT: errno = 22`, 完全看不出原因;
 * 打开 CONFIG_DEBUG_VIDEO_ERROR 后才打印
 *     "ERROR: SC2336: only 30 fps is supported"。
 *
 * 所以驱动必须把"我支持 1/30"显式声明出来, 上层的默认值才会取对。
 * 2026-09-14 真机实测: 补上本数组前 S_FMT 必失败, 补上后通过。
 */

static const struct v4l2_frmivalenum g_sc2336_frmintervals[] =
{
  {
    .index        = 0,
    .buf_type     = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    .pixel_format = V4L2_PIX_FMT_SBGGR8,
    .width        = 1280,
    .height       = 720,
    .type         = V4L2_FRMIVAL_TYPE_DISCRETE,
    .discrete     =
    {
      .numerator   = 1,
      .denominator = SC2336_FPS
    }
  }
};

#define SC2336_NFRMINTERVALS \
  (sizeof(g_sc2336_frmintervals) / sizeof(g_sc2336_frmintervals[0]))

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static bool sc2336_is_available(FAR struct imgsensor_s *sensor);
static int sc2336_init(FAR struct imgsensor_s *sensor);
static int sc2336_uninit(FAR struct imgsensor_s *sensor);
static FAR const char *
sc2336_get_driver_name(FAR struct imgsensor_s *sensor);
static int sc2336_validate_frame_setting(FAR struct imgsensor_s *sensor,
                                         imgsensor_stream_type_t type,
                                         uint8_t nr_datafmts,
                                         FAR imgsensor_format_t *datafmts,
                                         FAR imgsensor_interval_t *interval);
static int sc2336_start_capture(FAR struct imgsensor_s *sensor,
                                imgsensor_stream_type_t type,
                                uint8_t nr_datafmts,
                                FAR imgsensor_format_t *datafmts,
                                FAR imgsensor_interval_t *interval);
static int sc2336_stop_capture(FAR struct imgsensor_s *sensor,
                               imgsensor_stream_type_t type);
static int
sc2336_get_supported_value(FAR struct imgsensor_s *sensor, uint32_t id,
                           FAR imgsensor_supported_value_t *value);
static int sc2336_get_value(FAR struct imgsensor_s *sensor, uint32_t id,
                            uint32_t size, FAR imgsensor_value_t *value);
static int sc2336_set_value(FAR struct imgsensor_s *sensor, uint32_t id,
                            uint32_t size, imgsensor_value_t value);

/* Register helpers. */

static void sc2336_lock(FAR struct sc2336_dev_s *priv);
static void sc2336_unlock(FAR struct sc2336_dev_s *priv);
static int sc2336_write_reg(FAR struct sc2336_dev_s *priv, uint16_t reg,
                            uint8_t val);
static int sc2336_read_reg(FAR struct sc2336_dev_s *priv, uint16_t reg,
                           FAR uint8_t *val);
static int sc2336_write_table(FAR struct sc2336_dev_s *priv,
                              FAR const struct sc2336_reg_s *table,
                              size_t nentries);
static FAR const struct sc2336_mode_s *sc2336_find_mode(uint32_t width,
                                                        uint32_t height);
static int sc2336_apply_mode(FAR struct sc2336_dev_s *priv,
                             FAR const struct sc2336_mode_s *mode);
static bool sc2336_table_lookup(FAR const struct sc2336_reg_s *table,
                                size_t nentries, uint16_t reg,
                                FAR uint8_t *val);
static int sc2336_verify_table(FAR struct sc2336_dev_s *priv,
                               FAR const struct sc2336_mode_s *mode);
static int sc2336_set_stream(FAR struct sc2336_dev_s *priv, bool on);
static int sc2336_set_exposure(FAR struct sc2336_dev_s *priv,
                               uint32_t exposure);
static int sc2336_set_flip(FAR struct sc2336_dev_s *priv, bool hflip,
                           bool vflip);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct imgsensor_ops_s g_sc2336_ops =
{
  .is_available           = sc2336_is_available,
  .init                   = sc2336_init,
  .uninit                 = sc2336_uninit,
  .get_driver_name        = sc2336_get_driver_name,
  .validate_frame_setting = sc2336_validate_frame_setting,
  .start_capture          = sc2336_start_capture,
  .stop_capture           = sc2336_stop_capture,
  .get_supported_value    = sc2336_get_supported_value,
  .get_value              = sc2336_get_value,
  .set_value              = sc2336_set_value,
};

/* Describe the single pixel format this driver currently supports.  The V4L2
 * upper half reports these through VIDIOC_ENUM_FMT.
 */

static const struct v4l2_fmtdesc g_sc2336_fmtdescs[] =
{
  {
    .index       = 0,
    .type        = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    .flags       = 0,
    .description = "RAW8 (BGGR)",
    .pixelformat = V4L2_PIX_FMT_SBGGR8
  }
};

static struct sc2336_dev_s g_sc2336_priv;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sc2336_write_reg
 *
 * Description:
 *   Write one 8-bit value to a 16-bit register address over SCCB.
 *
 ****************************************************************************/

static int sc2336_write_reg(FAR struct sc2336_dev_s *priv, uint16_t reg,
                            uint8_t val)
{
  struct i2c_msg_s msg;
  uint8_t buf[SC2336_REGADDR_LEN + 1];
  int ret;

  buf[0] = (uint8_t)(reg >> 8);
  buf[1] = (uint8_t)(reg & 0xff);
  buf[2] = val;

  msg.frequency = SC2336_I2C_FREQUENCY;
  msg.addr      = priv->addr;
  msg.flags     = 0;
  msg.buffer    = buf;
  msg.length    = sizeof(buf);

  ret = I2C_TRANSFER(priv->i2c, &msg, 1);

#ifdef CONFIG_SC2336_DEBUG
  _info("SC2336: W %04x = %02x ret=%d\n", reg, val, ret);
#endif

  return ret < 0 ? ret : OK;
}

/****************************************************************************
 * Name: sc2336_read_reg
 *
 * Description:
 *   Read one 8-bit value from a 16-bit register address over SCCB.  SCCB has
 *   no repeated-START support, so the transfer is done as two separate I2C
 *   messages: address write, then data read.
 *
 ****************************************************************************/

static int sc2336_read_reg(FAR struct sc2336_dev_s *priv, uint16_t reg,
                           FAR uint8_t *val)
{
  struct i2c_msg_s msg[2];
  uint8_t addr[SC2336_REGADDR_LEN];
  int ret;

  addr[0] = (uint8_t)(reg >> 8);
  addr[1] = (uint8_t)(reg & 0xff);

  msg[0].frequency = SC2336_I2C_FREQUENCY;
  msg[0].addr      = priv->addr;
  msg[0].flags     = 0;
  msg[0].buffer    = addr;
  msg[0].length    = sizeof(addr);

  msg[1].frequency = SC2336_I2C_FREQUENCY;
  msg[1].addr      = priv->addr;
  msg[1].flags     = I2C_M_READ;
  msg[1].buffer    = val;
  msg[1].length    = 1;

  ret = I2C_TRANSFER(priv->i2c, msg, 2);

#ifdef CONFIG_SC2336_DEBUG
  _info("SC2336: R %04x -> %02x ret=%d\n", reg, *val, ret);
#endif

  return ret < 0 ? ret : OK;
}

/****************************************************************************
 * Name: sc2336_lock / sc2336_unlock
 *
 * Description:
 *   Serialise SCCB transactions and streaming-state changes.  Before
 *   sc2336_initialize() has created the mutex (lock_ready == false) the
 *   helpers are no-ops, which is safe because no other context can hold a
 *   reference to the sensor yet.
 *
 ****************************************************************************/

static void sc2336_lock(FAR struct sc2336_dev_s *priv)
{
  if (priv->lock_ready)
    {
      nxmutex_lock(&priv->lock);
    }
}

static void sc2336_unlock(FAR struct sc2336_dev_s *priv)
{
  if (priv->lock_ready)
    {
      nxmutex_unlock(&priv->lock);
    }
}

/****************************************************************************
 * Name: sc2336_write_table
 *
 * Description:
 *   Apply a register table in order.  The length is explicit: this writer
 *   deliberately does not look for a 0xffff sentinel, because a table that
 *   forgot it made the writer read past the end of its array and write the
 *   following .rodata words to the sensor over SCCB.
 *
 *   A 0xfffe entry delays for val milliseconds instead of writing.
 *
 ****************************************************************************/

static int sc2336_write_table(FAR struct sc2336_dev_s *priv,
                              FAR const struct sc2336_reg_s *table,
                              size_t nentries)
{
  size_t i;
  int ret;

  if (table == NULL || nentries == 0)
    {
      return -EINVAL;
    }

  for (i = 0; i < nentries; i++)
    {
      if (table[i].reg == SC2336_REG_DELAY)
        {
          nxsig_usleep((useconds_t)table[i].val * 1000);
          continue;
        }

      ret = sc2336_write_reg(priv, table[i].reg, table[i].val);
      if (ret < 0)
        {
          _err("ERROR: SC2336: table[%u/%u] write %04x=%02x failed: %d\n",
               (unsigned)i, (unsigned)nentries, table[i].reg, table[i].val,
               ret);
          return ret;
        }
    }

  return OK;
}

/****************************************************************************
 * Name: sc2336_verify_table
 *
 * Description:
 *   Read back a handful of registers the mode table is required to have
 *   programmed.  This distinguishes "the writes landed" from "streaming is on
 *   but the sensor never got configured", which look the same from the CSI
 *   side.
 *
 *   Mismatches are logged but do not abort the capture: register values can
 *   differ between sensor revisions, and refusing to stream on a diagnostic
 *   mismatch would turn a capture problem into a probe failure.
 *
 ****************************************************************************/

static bool sc2336_table_lookup(FAR const struct sc2336_reg_s *table,
                                size_t nentries, uint16_t reg,
                                FAR uint8_t *val)
{
  bool found = false;
  size_t i;

  for (i = 0; i < nentries; i++)
    {
      if (table[i].reg == reg)
        {
          /* Keep scanning: a table may write the same register twice (the
           * PLL unlock/lock pair), and the last write is the final value.
           */

          *val = table[i].val;
          found = true;
        }
    }

  return found;
}

static int sc2336_verify_table(FAR struct sc2336_dev_s *priv,
                               FAR const struct sc2336_mode_s *mode)
{
  static const uint16_t key[] =
  {
    0x36e9,           /* PLL lock control */
    0x37f9,           /* PLL lock control */
    0x4509,           /* MIPI output settings */
    0x320c, 0x320d,   /* HTS */
    0x320e, 0x320f,   /* VTS */
  };

  unsigned int i;
  unsigned int checked = 0;
  unsigned int bad = 0;

  if (mode == NULL)
    {
      return -EINVAL;
    }

  for (i = 0; i < sizeof(key) / sizeof(key[0]); i++)
    {
      uint8_t want = 0;
      uint8_t val  = 0;
      int ret;

      if (!sc2336_table_lookup(mode->table, mode->nentries, key[i], &want))
        {
          /* The 720p table does not carry every 1080p register. */

          continue;
        }

      ret = sc2336_read_reg(priv, key[i], &val);
      if (ret < 0)
        {
          _err("ERROR: SC2336: verify read %04x failed: %d\n", key[i], ret);
          continue;
        }

      checked++;

      if (val != want)
        {
          bad++;
          _err("ERROR: SC2336: verify %04x = %02x, expected %02x\n",
               key[i], val, want);
        }
    }

  /* 0x0100 must still be 0 before stream-on, for every mode. */

  {
    uint8_t sleepval = 0xff;
    int ret = sc2336_read_reg(priv, SC2336_REG_SLEEP_MODE, &sleepval);

    if (ret >= 0)
      {
        checked++;
        if (sleepval != 0x00)
          {
            bad++;
            _err("ERROR: SC2336: verify 0100 = %02x, expected 00\n",
                 sleepval);
          }
      }
  }

  syslog(LOG_INFO, "SC2336: %ux%u verify %u/%u match, %u mismatch\n",
         mode->width, mode->height, checked - bad, checked, bad);

  return bad == 0 ? OK : -EIO;
}

#ifdef CONFIG_SC2336_DEBUG
/****************************************************************************
 * Name: sc2336_dump_table_mismatches
 *
 * Description:
 *   Read back every register the mode table wrote and report the ones whose
 *   value does not stick.  Duplicate writes (the PLL unlock/lock pair) are
 *   compared against the final value.
 *
 ****************************************************************************/

static void sc2336_dump_table_mismatches(FAR struct sc2336_dev_s *priv,
                                         FAR const struct sc2336_mode_s *mode)
{
  unsigned int bad = 0;
  size_t i;

  for (i = 0; i < mode->nentries; i++)
    {
      uint8_t val = 0;
      uint8_t want = 0;
      int ret;

      if (mode->table[i].reg == SC2336_REG_DELAY)
        {
          continue;
        }

      (void)sc2336_table_lookup(mode->table, mode->nentries,
                                mode->table[i].reg, &want);

      ret = sc2336_read_reg(priv, mode->table[i].reg, &val);
      if (ret < 0)
        {
          _err("ERROR: SC2336: readback %04x failed: %d\n",
               mode->table[i].reg, ret);
          continue;
        }

      if (val != want)
        {
          bad++;
          if (bad <= 20)
            {
              syslog(LOG_ERR,
                     "SC2336: readback %04x = %02x, wrote %02x\n",
                     mode->table[i].reg, val, want);
            }
        }
    }

  syslog(LOG_INFO, "SC2336: full readback, %u mismatch(es)\n", bad);
}
#endif /* CONFIG_SC2336_DEBUG */

/****************************************************************************
 * Name: sc2336_set_stream
 *
 * Description:
 *   Enter or leave streaming (0x0100 = 1/0) and update the cached state.
 *
 ****************************************************************************/

static int sc2336_set_stream(FAR struct sc2336_dev_s *priv, bool on)
{
  int ret;

  if (on)
    {
      /* The reference stack leaves a long gap between the mode table (which
       * enables the MIPI PLL) and stream-on, because it also brings up the
       * ISP and the CSI controller in between.  Starting the MIPI TX too
       * soon leaves the data lanes parked in HS: the D-PHY reports activity
       * but the host parses no packet at all.
       */

      nxsig_usleep(SC2336_PLL_SETTLE_MS * 1000);
    }

  ret = sc2336_write_reg(priv, SC2336_REG_SLEEP_MODE, on ? 0x01 : 0x00);
  if (ret < 0)
    {
      _err("ERROR: SC2336: failed to %s streaming: %d\n",
           on ? "start" : "stop", ret);
      return ret;
    }

  priv->streaming = on;
  return OK;
}

/****************************************************************************
 * Name: sc2336_set_exposure
 *
 * Description:
 *   Program the integration time.  The SC2336 spreads the 16-bit exposure
 *   over three registers: 0x3e00[3:0] = exposure[15:12],
 *   0x3e01 = exposure[11:4], 0x3e02[7:4] = exposure[3:0].
 *
 ****************************************************************************/

static int sc2336_set_exposure(FAR struct sc2336_dev_s *priv,
                               uint32_t exposure)
{
  uint8_t val;
  int ret;

  if (exposure < SC2336_EXPOSURE_MIN)
    {
      exposure = SC2336_EXPOSURE_MIN;
    }
  else if (exposure > SC2336_EXPOSURE_MAX)
    {
      exposure = SC2336_EXPOSURE_MAX;
    }

  /* 0x3e02 keeps whatever is in bits [3:0] (the vendor driver masks only the
   * upper nibble), so it is read back before the new value is written.
   */

  ret = sc2336_read_reg(priv, SC2336_REG_SHUTTER_L, &val);
  if (ret < 0)
    {
      return ret;
    }

  val = (val & 0x0f) | ((uint8_t)(exposure & 0x0f) << 4);

  ret = sc2336_write_reg(priv, SC2336_REG_SHUTTER_H,
                         (uint8_t)(exposure >> 12) & 0x0f);
  if (ret < 0)
    {
      return ret;
    }

  ret = sc2336_write_reg(priv, SC2336_REG_SHUTTER_M,
                         (uint8_t)(exposure >> 4) & 0xff);
  if (ret < 0)
    {
      return ret;
    }

  ret = sc2336_write_reg(priv, SC2336_REG_SHUTTER_L, val);
  if (ret < 0)
    {
      return ret;
    }

  priv->exposure = exposure;
  return OK;
}

/****************************************************************************
 * Name: sc2336_set_flip
 *
 * Description:
 *   Set the mirror / vertical flip bits.  0x3221 bits [2:1] control the
 *   horizontal mirror and bits [6:5] the vertical flip.
 *
 ****************************************************************************/

static int sc2336_set_flip(FAR struct sc2336_dev_s *priv, bool hflip,
                           bool vflip)
{
  uint8_t val;
  int ret;

  ret = sc2336_read_reg(priv, SC2336_REG_FLIP_MIRROR, &val);
  if (ret < 0)
    {
      return ret;
    }

  val &= ~(0x06 | 0x60);

  if (hflip)
    {
      val |= 0x06;
    }

  if (vflip)
    {
      val |= 0x60;
    }

  ret = sc2336_write_reg(priv, SC2336_REG_FLIP_MIRROR, val);
  if (ret < 0)
    {
      return ret;
    }

  priv->hflip = hflip;
  priv->vflip = vflip;
  return OK;
}

/****************************************************************************
 * Name: sc2336_is_available
 ****************************************************************************/

static bool sc2336_is_available(FAR struct imgsensor_s *sensor)
{
  FAR struct sc2336_dev_s *priv = (FAR struct sc2336_dev_s *)sensor;

  return priv->detected;
}

/****************************************************************************
 * Name: sc2336_init
 *
 * Description:
 *   Probe the sensor and put it into a known state.
 *
 *   Power-on sequence of this board (confirmed against the module):
 *     1. The module has its own 24 MHz oscillator and its RESET line is hard
 *        pulled to 3.3 V, so the host drives neither XCLK nor RESET.
 *     2. Force the sensor into sleep (0x0100 = 0) so a previous run cannot
 *        leave it streaming.
 *     3. Read the identification registers; a mismatch fails the probe.
 *   The mode table performs its own soft reset (0x0103 = 1) when a capture
 *   starts, followed by the vendor-mandated settling delay.
 *
 ****************************************************************************/

static int sc2336_init(FAR struct imgsensor_s *sensor)
{
  FAR struct sc2336_dev_s *priv = (FAR struct sc2336_dev_s *)sensor;
  uint8_t pid_h;
  uint8_t pid_l;
  int ret;

  if (priv->i2c == NULL)
    {
      return -ENODEV;
    }

  sc2336_lock(priv);

  /* Mirror the reference driver's probe order: soft reset first, wait for
   * it to settle, then leave the sensor asleep.  The reset is best effort;
   * the identification read below is what decides whether the sensor is
   * really there.
   */

  ret = sc2336_write_reg(priv, 0x0103, 0x01);
  if (ret < 0)
    {
      _err("ERROR: SC2336: soft reset at probe failed: %d\n", ret);
    }

  nxsig_usleep(5 * 1000);

  ret = sc2336_write_reg(priv, SC2336_REG_SLEEP_MODE, 0x00);
  if (ret < 0)
    {
      _err("ERROR: SC2336: cannot force sleep at probe: %d\n", ret);
    }

  priv->streaming = false;

  ret = sc2336_read_reg(priv, SC2336_REG_SENSOR_ID_H, &pid_h);
  if (ret < 0)
    {
      _err("ERROR: SC2336: cannot read the sensor ID: %d\n", ret);
      priv->detected = false;
      goto out;
    }

  ret = sc2336_read_reg(priv, SC2336_REG_SENSOR_ID_L, &pid_l);
  if (ret < 0)
    {
      _err("ERROR: SC2336: cannot read the sensor ID: %d\n", ret);
      priv->detected = false;
      goto out;
    }

  if ((((uint16_t)pid_h << 8) | pid_l) != SC2336_PID)
    {
      _err("ERROR: SC2336: unexpected PID 0x%02x%02x (expected 0x%04x)\n",
           pid_h, pid_l, SC2336_PID);
      priv->detected = false;
      ret = -ENODEV;
      goto out;
    }

  priv->detected     = true;
  priv->applied_mode = NULL;
  priv->exposure     = SC2336_EXPOSURE_DEFAULT;
  priv->hflip    = false;
  priv->vflip    = false;
  ret = OK;

out:
  sc2336_unlock(priv);
  return ret;
}

/****************************************************************************
 * Name: sc2336_uninit
 *
 * Description:
 *   Stop streaming and drop the "detected" state.  The caller holds no lock:
 *   this function takes it itself and uses the internal stream helper rather
 *   than the public stop_capture(), which would try to lock again.
 *
 ****************************************************************************/

static int sc2336_uninit(FAR struct imgsensor_s *sensor)
{
  FAR struct sc2336_dev_s *priv = (FAR struct sc2336_dev_s *)sensor;
  int ret;

  sc2336_lock(priv);

  if (priv->streaming)
    {
      ret = sc2336_set_stream(priv, false);
    }
  else
    {
      /* Best effort: leave the sensor asleep even if the driver believed it
       * was not streaming (for example after an aborted start).
       */

      ret = sc2336_write_reg(priv, SC2336_REG_SLEEP_MODE, 0x00);
    }

  priv->detected     = false;
  priv->applied_mode = NULL;
  sc2336_unlock(priv);

  return ret < 0 ? ret : OK;
}

/****************************************************************************
 * Name: sc2336_get_driver_name
 ****************************************************************************/

static FAR const char *sc2336_get_driver_name(FAR struct imgsensor_s *sensor)
{
  return "SC2336";
}

/****************************************************************************
 * Name: sc2336_find_mode
 *
 * Description:
 *   Map a requested frame size to the register table that produces it.
 *
 ****************************************************************************/

static FAR const struct sc2336_mode_s *sc2336_find_mode(uint32_t width,
                                                        uint32_t height)
{
  size_t i;

  for (i = 0; i < SC2336_NMODES; i++)
    {
      if (g_sc2336_modes[i].width == width &&
          g_sc2336_modes[i].height == height)
        {
          return &g_sc2336_modes[i];
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: sc2336_apply_mode
 *
 * Description:
 *   Program the mode table once per requested mode.  Called from
 *   validate_frame_setting() (VIDIOC_S_FMT) and, as a fallback, from
 *   start_capture().  The caller holds the driver lock.
 *
 ****************************************************************************/

static int sc2336_apply_mode(FAR struct sc2336_dev_s *priv,
                             FAR const struct sc2336_mode_s *mode)
{
  int ret;

  if (priv->applied_mode == mode)
    {
      return OK;
    }

  syslog(LOG_INFO, "SC2336: apply mode %ux%u, %u table entries\n",
         mode->width, mode->height, (unsigned)mode->nentries);

  ret = sc2336_write_table(priv, mode->table, mode->nentries);
  if (ret < 0)
    {
      return ret;
    }

  /* Always read back the key registers: cheap, and it makes "the table
   * landed" visible in dmesg without a debug build.
   */

  (void)sc2336_verify_table(priv, mode);

#ifdef CONFIG_SC2336_DEBUG
  /* Full register dump for bring-up. */

  {
    static const uint16_t probe[] =
    {
      0x0100, 0x36e9, 0x37f9,   /* stream on/off, PLL lock regs */
      0x320c, 0x320d,           /* HTS */
      0x320e, 0x320f,           /* VTS */
      0x4509,                   /* MIPI related */
      0x0103,                   /* soft reset (reads back 0 when done) */
      0x301f, 0x3018, 0x3031,   /* lane num / MIPI enable candidates */
      0x4501, 0x4509, 0x4819, 0x481b, 0x481d,
      0x3e00, 0x3e01, 0x3e02    /* exposure */
    };

    unsigned int i;

    for (i = 0; i < sizeof(probe) / sizeof(probe[0]); i++)
      {
        uint8_t v = 0;
        int r = sc2336_read_reg(priv, probe[i], &v);

        syslog(LOG_INFO, "SC2336: reg %04x = %02x (ret=%d)\n",
               probe[i], v, r);
      }
  }

  sc2336_dump_table_mismatches(priv, mode);
#endif /* CONFIG_SC2336_DEBUG */

  priv->exposure   = SC2336_EXPOSURE_DEFAULT;
  priv->hflip      = false;
  priv->vflip      = false;
  priv->applied_mode = mode;

  /* Let the sensor PLL settle after the table's unlock/lock writes.  The
   * reference stack has the S_FMT -> STREAMON application gap here.
   */

  nxsig_usleep(50 * 1000);
  return OK;
}

/****************************************************************************
 * Name: sc2336_validate_frame_setting
 ****************************************************************************/

static int sc2336_validate_frame_setting(FAR struct imgsensor_s *sensor,
                                         imgsensor_stream_type_t type,
                                         uint8_t nr_datafmts,
                                         FAR imgsensor_format_t *datafmts,
                                         FAR imgsensor_interval_t *interval)
{
  FAR struct sc2336_dev_s *priv = (FAR struct sc2336_dev_s *)sensor;
  FAR const struct sc2336_mode_s *mode;

  if (!priv->detected)
    {
      return -ENODEV;
    }

  if (datafmts == NULL || nr_datafmts == 0)
    {
      return -EINVAL;
    }

  mode = sc2336_find_mode(datafmts[IMGSENSOR_FMT_MAIN].width,
                          datafmts[IMGSENSOR_FMT_MAIN].height);
  if (mode == NULL)
    {
      _err("ERROR: SC2336: %ux%u is not supported\n",
           datafmts[IMGSENSOR_FMT_MAIN].width,
           datafmts[IMGSENSOR_FMT_MAIN].height);
      return -EINVAL;
    }

  /* The register table fixes the frame rate at 30 fps; reject anything else
   * rather than silently capturing at a different rate.
   */

  if (interval != NULL && interval->denominator != 0 &&
      (interval->numerator != 1 || interval->denominator != SC2336_FPS))
    {
      _err("ERROR: SC2336: only %u fps is supported\n", SC2336_FPS);
      return -EINVAL;
    }

  /* Program the mode table here (VIDIOC_S_FMT) instead of at STREAMON so the
   * CSI controller is created and started only after the sensor output is
   * configured, matching the reference esp_video ordering.
   *
   * No driver lock here: the V4L2 upper half serialises the ioctls with its
   * own mutex, and start_capture() calls this function while already holding
   * the driver lock (taking it again would deadlock).
   */

  return sc2336_apply_mode(priv, mode);
}

#ifdef CONFIG_SC2336_DEBUG
/****************************************************************************
 * Name: sc2336_delayed_stream_check
 *
 * Description:
 *   Diagnostic: read 0x0100 two seconds after stream-on.  If the sensor has
 *   silently left streaming, the MIPI transmitter state is a sensor-side
 *   problem (reset/brown-out); if it still reads 1, the sensor is in stream
 *   mode and the missing packets are a transmit/PHY issue.
 *
 ****************************************************************************/

static struct work_s g_sc2336_check_work;

static void sc2336_delayed_stream_check(FAR void *arg)
{
  FAR struct sc2336_dev_s *priv = (FAR struct sc2336_dev_s *)arg;
  uint8_t val = 0;
  int ret = sc2336_read_reg(priv, SC2336_REG_SLEEP_MODE, &val);

  syslog(LOG_INFO, "SC2336: t+2s 0x0100=%02x (ret=%d)\n", val, ret);
}
#endif /* CONFIG_SC2336_DEBUG */

/****************************************************************************
 * Name: sc2336_start_capture
 *
 * Description:
 *   Apply the mode table and put the sensor into streaming.  The table starts
 *   with a soft reset (0x0103 = 1) and a 10 ms delay, so the sensor is in
 *   sleep when the table ends and has to be started explicitly.
 *
 ****************************************************************************/

static int sc2336_start_capture(FAR struct imgsensor_s *sensor,
                                imgsensor_stream_type_t type,
                                uint8_t nr_datafmts,
                                FAR imgsensor_format_t *datafmts,
                                FAR imgsensor_interval_t *interval)
{
  FAR struct sc2336_dev_s *priv = (FAR struct sc2336_dev_s *)sensor;
  FAR const struct sc2336_mode_s *mode;
  uint8_t rb = 0;
  int vret;
  int ret;

  sc2336_lock(priv);

  syslog(LOG_INFO, "SC2336: start_capture(%d) entered\n", (int)type);

  if (!priv->detected)
    {
      ret = -ENODEV;
      goto out;
    }

  if (priv->streaming)
    {
      ret = OK;
      goto out;
    }

  ret = sc2336_validate_frame_setting(sensor, type, nr_datafmts, datafmts,
                                      interval);
  if (ret < 0)
    {
      goto out;
    }

  /* The table is applied with an explicit length; there is no 0xffff
   * terminator to get out of sync with the array.
   */

  mode = sc2336_find_mode(datafmts[IMGSENSOR_FMT_MAIN].width,
                          datafmts[IMGSENSOR_FMT_MAIN].height);
  if (mode == NULL)
    {
      ret = -EINVAL;
      goto out;
    }

  ret = sc2336_apply_mode(priv, mode);
  if (ret < 0)
    {
      goto out;
    }

  ret = sc2336_set_stream(priv, true);
  if (ret < 0)
    {
      goto out;
    }

  /* Confirm the sensor really left sleep.  Without this "no frames" cannot be
   * told apart from "the stream-on write never landed".
   */

  vret = sc2336_read_reg(priv, SC2336_REG_SLEEP_MODE, &rb);
  syslog(LOG_INFO, "SC2336: stream-on 0x0100 read back 0x%02x (ret=%d)\n",
         rb, vret);

  if (vret >= 0 && rb != 0x01)
    {
      _err("ERROR: SC2336: sensor did not enter streaming (0x0100=0x%02x)\n",
           rb);
      (void)sc2336_set_stream(priv, false);
      ret = -EIO;
      goto out;
    }

#ifdef CONFIG_SC2336_DEBUG
  work_queue(LPWORK, &g_sc2336_check_work, sc2336_delayed_stream_check, priv,
             MSEC2TICK(2000));
#endif

  nxsig_usleep(SC2336_STREAM_SETTLE_MS * 1000);

  ret = OK;

out:
  sc2336_unlock(priv);
  return ret;
}

/****************************************************************************
 * Name: sc2336_stop_capture
 ****************************************************************************/

static int sc2336_stop_capture(FAR struct imgsensor_s *sensor,
                               imgsensor_stream_type_t type)
{
  FAR struct sc2336_dev_s *priv = (FAR struct sc2336_dev_s *)sensor;
  int ret;

  sc2336_lock(priv);

  if (!priv->streaming)
    {
      ret = OK;
      goto out;
    }

  ret = sc2336_set_stream(priv, false);

out:
  sc2336_unlock(priv);
  return ret;
}

/****************************************************************************
 * Name: sc2336_get_supported_value
 ****************************************************************************/

static int sc2336_get_supported_value(FAR struct imgsensor_s *sensor,
                                      uint32_t id,
                                      FAR imgsensor_supported_value_t *value)
{
  if (value == NULL)
    {
      return -EINVAL;
    }

  switch (id)
    {
      case IMGSENSOR_ID_HFLIP_VIDEO:
      case IMGSENSOR_ID_VFLIP_VIDEO:
        value->type             = IMGSENSOR_CTRL_TYPE_BOOLEAN;
        value->u.range.minimum  = 0;
        value->u.range.maximum  = 1;
        value->u.range.step     = 1;
        value->u.range.default_value = 0;
        return OK;

      case IMGSENSOR_ID_EXPOSURE_ABSOLUTE:
        value->type             = IMGSENSOR_CTRL_TYPE_INTEGER;
        value->u.range.minimum  = SC2336_EXPOSURE_MIN;
        value->u.range.maximum  = SC2336_EXPOSURE_MAX;
        value->u.range.step     = 1;
        value->u.range.default_value = SC2336_EXPOSURE_DEFAULT;
        return OK;

      default:
        return -ENOTTY;
    }
}

/****************************************************************************
 * Name: sc2336_get_value
 ****************************************************************************/

static int sc2336_get_value(FAR struct imgsensor_s *sensor, uint32_t id,
                            uint32_t size, FAR imgsensor_value_t *value)
{
  FAR struct sc2336_dev_s *priv = (FAR struct sc2336_dev_s *)sensor;

  if (value == NULL || size != sizeof(int32_t))
    {
      return -EINVAL;
    }

  switch (id)
    {
      case IMGSENSOR_ID_HFLIP_VIDEO:
        value->value32 = priv->hflip ? 1 : 0;
        return OK;

      case IMGSENSOR_ID_VFLIP_VIDEO:
        value->value32 = priv->vflip ? 1 : 0;
        return OK;

      case IMGSENSOR_ID_EXPOSURE_ABSOLUTE:
        value->value32 = priv->exposure;
        return OK;

      default:
        return -ENOTTY;
    }
}

/****************************************************************************
 * Name: sc2336_set_value
 ****************************************************************************/

static int sc2336_set_value(FAR struct imgsensor_s *sensor, uint32_t id,
                            uint32_t size, imgsensor_value_t value)
{
  FAR struct sc2336_dev_s *priv = (FAR struct sc2336_dev_s *)sensor;
  int ret;

  if (size != sizeof(int32_t))
    {
      return -EINVAL;
    }

  sc2336_lock(priv);

  if (!priv->detected)
    {
      ret = -ENODEV;
      goto out;
    }

  switch (id)
    {
      case IMGSENSOR_ID_HFLIP_VIDEO:
        ret = sc2336_set_flip(priv, value.value32 != 0, priv->vflip);
        break;

      case IMGSENSOR_ID_VFLIP_VIDEO:
        ret = sc2336_set_flip(priv, priv->hflip, value.value32 != 0);
        break;

      case IMGSENSOR_ID_EXPOSURE_ABSOLUTE:
        ret = sc2336_set_exposure(priv, (uint32_t)value.value32);
        break;

      default:
        ret = -ENOTTY;
        break;
    }

out:
  sc2336_unlock(priv);
  return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: sc2336_chipid
 ****************************************************************************/

int sc2336_chipid(FAR struct i2c_master_s *i2c, uint8_t addr)
{
  struct sc2336_dev_s probe;
  uint8_t pid_h;
  uint8_t pid_l;
  int ret;

  if (i2c == NULL)
    {
      return -EINVAL;
    }

  memset(&probe, 0, sizeof(probe));
  probe.i2c  = i2c;
  probe.addr = addr;

  ret = sc2336_read_reg(&probe, SC2336_REG_SENSOR_ID_H, &pid_h);
  if (ret < 0)
    {
      return ret;
    }

  ret = sc2336_read_reg(&probe, SC2336_REG_SENSOR_ID_L, &pid_l);
  if (ret < 0)
    {
      return ret;
    }

  return ((int)pid_h << 8) | pid_l;
}

/****************************************************************************
 * Name: sc2336_initialize
 ****************************************************************************/

FAR struct imgsensor_s *sc2336_initialize(FAR struct i2c_master_s *i2c,
                                          uint8_t addr)
{
  FAR struct sc2336_dev_s *priv = &g_sc2336_priv;
  int ret;

  if (i2c == NULL)
    {
      set_errno(EINVAL);
      return NULL;
    }

  /* Never wipe a live instance: it may be streaming and its mutex is in use.
   * A second initialize() on a detected sensor is a caller bug, not a reason
   * to corrupt the state of the running capture.
   */

  if (priv->lock_ready && priv->detected)
    {
      set_errno(EBUSY);
      return NULL;
    }

  if (priv->lock_ready)
    {
      nxmutex_destroy(&priv->lock);
      priv->lock_ready = false;
    }

  memset(priv, 0, sizeof(*priv));

  nxmutex_init(&priv->lock);
  priv->lock_ready = true;

  priv->sensor.ops    = &g_sc2336_ops;
  priv->sensor.fmtdescs     = g_sc2336_fmtdescs;
  priv->sensor.fmtdescs_num = sizeof(g_sc2336_fmtdescs) /
                              sizeof(g_sc2336_fmtdescs[0]);

  /* 必须声明支持的分辨率/帧率: v4l2_cap 用它们生成默认 frame setting,
   * 不声明就会退回 15 fps 的写死默认值 -> S_FMT 被本驱动自己的校验拒绝。
   */

  priv->sensor.frmsizes         = g_sc2336_frmsizes;
  priv->sensor.frmsizes_num     = SC2336_NFRMSIZES;

  priv->sensor.frmintervals     = g_sc2336_frmintervals;
  priv->sensor.frmintervals_num = SC2336_NFRMINTERVALS;

  priv->i2c  = i2c;
  priv->addr = addr;

  /* Probe straight away so that a camera that is not fitted or not powered
   * is reported by the board bring-up code instead of at first capture.
   */

  ret = sc2336_init(&priv->sensor);
  if (ret < 0)
    {
      set_errno(-ret);
      return NULL;
    }

  return &priv->sensor;
}

/****************************************************************************
 * Name: sc2336_uninitialize
 ****************************************************************************/

int sc2336_uninitialize(void)
{
  FAR struct sc2336_dev_s *priv = &g_sc2336_priv;

  if (priv->sensor.ops == NULL)
    {
      return -EINVAL;
    }

  sc2336_uninit(&priv->sensor);

  priv->i2c  = NULL;
  priv->addr = 0;
  priv->sensor.ops = NULL;

  if (priv->lock_ready)
    {
      nxmutex_destroy(&priv->lock);
      priv->lock_ready = false;
    }

  return OK;
}
