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
#include <nuttx/signal.h>
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

#define SC2336_HTS               2250
#define SC2336_VTS               1200
#define SC2336_FPS               30

/* Exposure limits: the integrated exposure is expressed in lines and has to
 * stay below the frame length.  The vendor driver uses VTS - 6 as maximum
 * and 8 as minimum, with 0x37e (894 lines) as the power-on default.
 */

#define SC2336_EXPOSURE_MIN      8
#define SC2336_EXPOSURE_MAX      (SC2336_VTS - 6)
#define SC2336_EXPOSURE_DEFAULT  0x37e

/* Table terminators.  The vendor tables reserve two magic registers: 0xffff
 * ends the table and 0xfffe delays for the number of milliseconds stored in
 * the value field.
 */

#define SC2336_REG_END           0xffff
#define SC2336_REG_DELAY         0xfffe

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct sc2336_reg_s
{
  uint16_t reg;
  uint8_t  val;
};

struct sc2336_framesize_s
{
  uint16_t width;
  uint16_t height;
};

struct sc2336_dev_s
{
  struct imgsensor_s sensor;
  FAR struct i2c_master_s *i2c;
  uint8_t addr;

  /* Values written by start_capture() and reported through get_value(). */

  uint32_t exposure;
  bool     hflip;
  bool     vflip;

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

static const struct sc2336_reg_s g_sc2336_1080p30_raw10[] =
{
  {0x0103, 0x01}, /* Soft reset */

  /* The soft reset has to complete before the register file accepts further
   * writes: the sensor is busy re-initialising itself and silently drops
   * anything sent in that window.  Without this pause the whole table below --
   * including every PLL/MIPI register (0x36e9/0x37f9/0x36xx) and the closing
   * "lock the PLL" writes -- is lost, which leaves the sensor with no MIPI
   * output clock at all.  The visible symptom is not a bus error: I2C works,
   * 0x0100 reads back 1 after "stream on", the CSI controller, bridge and
   * DW-GDMA all report configured and enabled, and yet not one byte ever
   * reaches memory because the CSI PHY can never lock to a clock that is not
   * being generated.
   */

  {SC2336_REG_DELAY, 10}, /* ms: let the soft reset settle */

  {0x0100, 0x00}, /* Stay in sleep */
  {0x36e9, 0x80},
  {0x37f9, 0x80},
  {0x301f, 0x02},
  {0x3106, 0x05},
  {0x320c, 0x08},
  {0x320d, 0xca}, /* HTS = 2250 */
  {0x320e, 0x04},
  {0x320f, 0xb0}, /* VTS = 1200 */
  {0x3248, 0x04},
  {0x3249, 0x0b},
  {0x3253, 0x08},
  {0x3301, 0x09},
  {0x3302, 0xff},
  {0x3303, 0x10},
  {0x3306, 0x60},
  {0x3307, 0x02},
  {0x330a, 0x01},
  {0x330b, 0x10},
  {0x330c, 0x16},
  {0x330d, 0xff},
  {0x3318, 0x02},
  {0x3321, 0x0a},
  {0x3327, 0x0e},
  {0x332b, 0x12},
  {0x3333, 0x10},
  {0x3334, 0x40},
  {0x335e, 0x06},
  {0x335f, 0x0a},
  {0x3364, 0x1f},
  {0x337c, 0x02},
  {0x337d, 0x0e},
  {0x3390, 0x09},
  {0x3391, 0x0f},
  {0x3392, 0x1f},
  {0x3393, 0x20},
  {0x3394, 0x20},
  {0x3395, 0xff},
  {0x33a2, 0x04},
  {0x33b1, 0x80},
  {0x33b2, 0x68},
  {0x33b3, 0x42},
  {0x33f9, 0x70},
  {0x33fb, 0xd0},
  {0x33fc, 0x0f},
  {0x33fd, 0x1f},
  {0x349f, 0x03},
  {0x34a6, 0x0f},
  {0x34a7, 0x1f},
  {0x34a8, 0x42},
  {0x34a9, 0x06},
  {0x34aa, 0x01},
  {0x34ab, 0x23},
  {0x34ac, 0x01},
  {0x34ad, 0x84},
  {0x3630, 0xf4},
  {0x3633, 0x22},
  {0x3639, 0xf4},
  {0x363c, 0x47},
  {0x3670, 0x09},
  {0x3674, 0xf4},
  {0x3675, 0xfb},
  {0x3676, 0xed},
  {0x367c, 0x09},
  {0x367d, 0x0f},
  {0x3690, 0x33},
  {0x3691, 0x33},
  {0x3692, 0x43},
  {0x3698, 0x89},
  {0x3699, 0x96},
  {0x369a, 0xd0},
  {0x369b, 0xd0},
  {0x369c, 0x09},
  {0x369d, 0x0f},
  {0x36a2, 0x09},
  {0x36a3, 0x0f},
  {0x36a4, 0x1f},
  {0x36d0, 0x01},
  {0x36ea, 0x09},
  {0x36eb, 0x0c},
  {0x36ec, 0x1c},
  {0x36ed, 0x28},
  {0x3722, 0xe1},
  {0x3724, 0x41},
  {0x3725, 0xc1},
  {0x3728, 0x20},
  {0x37fa, 0x09},
  {0x37fb, 0x32},
  {0x37fc, 0x11},
  {0x37fd, 0x37},
  {0x3900, 0x0d},
  {0x3905, 0x98},
  {0x391b, 0x81},
  {0x391c, 0x10},
  {0x3933, 0x81},
  {0x3934, 0xc5},
  {0x3940, 0x68},
  {0x3941, 0x00},
  {0x3942, 0x01},
  {0x3943, 0xc6},
  {0x3952, 0x02},
  {0x3953, 0x0f},
  {0x3e01, 0x37},
  {0x3e02, 0xe0}, /* Exposure = 0x37e */
  {0x3e08, 0x1f},
  {0x3e1b, 0x14},
  {0x440e, 0x02},
  {0x4509, 0x38},
  {0x4819, 0x06},
  {0x481b, 0x03},
  {0x481d, 0x0b},
  {0x481f, 0x03},
  {0x4821, 0x08},
  {0x4823, 0x03},
  {0x4825, 0x03},
  {0x4827, 0x03},
  {0x4829, 0x05},
  {0x5799, 0x06},
  {0x5ae0, 0xfe},
  {0x5ae1, 0x40},
  {0x5ae2, 0x30},
  {0x5ae3, 0x28},
  {0x5ae4, 0x20},
  {0x5ae5, 0x30},
  {0x5ae6, 0x28},
  {0x5ae7, 0x20},
  {0x5ae8, 0x3c},
  {0x5ae9, 0x30},
  {0x5aea, 0x28},
  {0x5aeb, 0x3c},
  {0x5aec, 0x30},
  {0x5aed, 0x28},
  {0x5aee, 0xfe},
  {0x5aef, 0x40},
  {0x5af4, 0x30},
  {0x5af5, 0x28},
  {0x5af6, 0x20},
  {0x5af7, 0x30},
  {0x5af8, 0x28},
  {0x5af9, 0x20},
  {0x5afa, 0x3c},
  {0x5afb, 0x30},
  {0x5afc, 0x28},
  {0x5afd, 0x3c},
  {0x5afe, 0x30},
  {0x5aff, 0x28},
  {0x36e9, 0x53},
  {0x37f9, 0x53}, /* Lock the PLL */
};

/* Supported frame sizes.  Only the mode that has a verified register
 * table is listed; the SC2336 has further modes (720p, 800x800, 640x480,
 * 1080p25) that would only need another table here.
 */

static const struct sc2336_framesize_s g_sc2336_framesizes[] =
{
  {1920, 1080}
};

#define SC2336_NFRAMESIZES \
  (sizeof(g_sc2336_framesizes) / sizeof(g_sc2336_framesizes[0]))

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
    .pixel_format = V4L2_PIX_FMT_SBGGR10,
    .width        = 1920,
    .height       = 1080,
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

static int sc2336_write_reg(FAR struct sc2336_dev_s *priv, uint16_t reg,
                            uint8_t val);
static int sc2336_read_reg(FAR struct sc2336_dev_s *priv, uint16_t reg,
                           FAR uint8_t *val);
static int sc2336_write_table(FAR struct sc2336_dev_s *priv,
                              FAR const struct sc2336_reg_s *table);
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
    .description = "RAW10 (BGGR)",
    .pixelformat = V4L2_PIX_FMT_SBGGR10
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
 * Name: sc2336_write_table
 *
 * Description:
 *   Apply a vendor register table in order.  0xffff terminates the table and
 *   0xfffe requests a delay of val milliseconds.
 *
 ****************************************************************************/

static int sc2336_write_table(FAR struct sc2336_dev_s *priv,
                              FAR const struct sc2336_reg_s *table)
{
  FAR const struct sc2336_reg_s *p;
  int ret;

  for (p = table; p->reg != SC2336_REG_END; p++)
    {
      if (p->reg == SC2336_REG_DELAY)
        {
          nxsig_usleep((useconds_t)p->val * 1000);
          continue;
        }

      ret = sc2336_write_reg(priv, p->reg, p->val);
      if (ret < 0)
        {
          _err("ERROR: SC2336: write %04x failed: %d\n", p->reg, ret);
          return ret;
        }
    }

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

  /* The module has its own 24 MHz oscillator and the sensor reset is hard
   * pulled to 3.3 V, so there is no clock or reset line to sequence: only
   * the identification registers have to be read back.
   */

  ret = sc2336_read_reg(priv, SC2336_REG_SENSOR_ID_H, &pid_h);
  if (ret < 0)
    {
      _err("ERROR: SC2336: cannot read the sensor ID: %d\n", ret);
      priv->detected = false;
      return ret;
    }

  ret = sc2336_read_reg(priv, SC2336_REG_SENSOR_ID_L, &pid_l);
  if (ret < 0)
    {
      _err("ERROR: SC2336: cannot read the sensor ID: %d\n", ret);
      priv->detected = false;
      return ret;
    }

  if ((((uint16_t)pid_h << 8) | pid_l) != SC2336_PID)
    {
      _err("ERROR: SC2336: unexpected PID 0x%02x%02x (expected 0x%04x)\n",
           pid_h, pid_l, SC2336_PID);
      priv->detected = false;
      return -ENODEV;
    }

  priv->detected = true;
  return OK;
}

/****************************************************************************
 * Name: sc2336_uninit
 ****************************************************************************/

static int sc2336_uninit(FAR struct imgsensor_s *sensor)
{
  FAR struct sc2336_dev_s *priv = (FAR struct sc2336_dev_s *)sensor;

  if (priv->streaming)
    {
      sc2336_stop_capture(sensor, IMGSENSOR_STREAM_TYPE_VIDEO);
    }

  priv->detected = false;
  return OK;
}

/****************************************************************************
 * Name: sc2336_get_driver_name
 ****************************************************************************/

static FAR const char *sc2336_get_driver_name(FAR struct imgsensor_s *sensor)
{
  return "SC2336";
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
  size_t i;

  if (!priv->detected)
    {
      return -ENODEV;
    }

  if (datafmts == NULL || nr_datafmts == 0)
    {
      return -EINVAL;
    }

  for (i = 0; i < SC2336_NFRAMESIZES; i++)
    {
      if (datafmts[IMGSENSOR_FMT_MAIN].width ==
            g_sc2336_framesizes[i].width &&
          datafmts[IMGSENSOR_FMT_MAIN].height ==
            g_sc2336_framesizes[i].height)
        {
          break;
        }
    }

  if (i == SC2336_NFRAMESIZES)
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

  return OK;
}

/****************************************************************************
 * Name: sc2336_start_capture
 ****************************************************************************/

static int sc2336_start_capture(FAR struct imgsensor_s *sensor,
                                imgsensor_stream_type_t type,
                                uint8_t nr_datafmts,
                                FAR imgsensor_format_t *datafmts,
                                FAR imgsensor_interval_t *interval)
{
  FAR struct sc2336_dev_s *priv = (FAR struct sc2336_dev_s *)sensor;
  int ret;

  syslog(LOG_INFO, "SC2336: start_capture(%d) entered\n", (int)type);

  if (!priv->detected)
    {
      return -ENODEV;
    }

  if (priv->streaming)
    {
      return OK;
    }

  ret = sc2336_validate_frame_setting(sensor, type, nr_datafmts, datafmts,
                                      interval);
  if (ret < 0)
    {
      return ret;
    }

  /* Apply the mode table.  It starts with a soft reset, so the sensor is
   * left in sleep mode afterwards and has to be started explicitly below.
   */

  ret = sc2336_write_table(priv, g_sc2336_1080p30_raw10);
  if (ret < 0)
    {
      return ret;
    }

  /* Bring-up diagnostic: read back the registers the table is supposed to have
   * programmed.  This is what separates "the table never landed" (soft reset
   * still in progress, writes dropped) from "the table landed but the MIPI
   * link never comes up" -- both look identical from the CSI side, where the
   * controller, bridge and DW-GDMA all report configured and enabled yet not a
   * single byte arrives.
   */

  {
    static const uint16_t probe[] =
    {
      0x0100, 0x36e9, 0x37f9,   /* stream on/off, PLL lock regs */
      0x320c, 0x320d,           /* HTS = 2250 */
      0x320e, 0x320f,           /* VTS = 1200 */
      0x4509,                   /* MIPI related */
      0x0103,                   /* soft reset (reads back 0 when done) */

      /* MIPI / output-enable candidates.  The PHY clock lane has been observed
       * going active-high-speed exactly once while the frame counters stay at
       * zero, so the sensor is configured but apparently not streaming.  These
       * are the registers a SmartSens part typically uses to gate the MIPI
       * output; reading them back shows which one is still off.
       */

      0x301f, 0x3018, 0x3031,   /* lane num / mipi enable candidates */
      0x4500, 0x4501, 0x4502, 0x4503, 0x4504, 0x4505, 0x4506, 0x4507,
      0x4508, 0x450a, 0x450b, 0x450c, 0x450d, 0x450e, 0x450f,
      0x4600, 0x4601, 0x4602, 0x4603,
      0x4818, 0x4819, 0x481a, 0x481b, 0x481c, 0x481d,
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

  priv->exposure = SC2336_EXPOSURE_DEFAULT;
  priv->hflip    = false;
  priv->vflip    = false;

  ret = sc2336_write_reg(priv, SC2336_REG_SLEEP_MODE, 0x01);
  if (ret < 0)
    {
      _err("ERROR: SC2336: failed to start streaming: %d\n", ret);
      return ret;
    }

  /* Bring-up diagnostic: confirm the sensor really left sleep by reading the
   * bit back.  Without this "the sensor produced no frames" cannot be told
   * apart from "start_capture was never called" or "the write did not land".
   */

  {
    uint8_t rb = 0;
    int rret = sc2336_read_reg(priv, SC2336_REG_SLEEP_MODE, &rb);

    syslog(LOG_INFO,
           "SC2336: start_capture -> 0x0100=0x01, read back 0x%02x (ret=%d)\n",
           rb, rret);
  }

  /* TEMP-CAMERA-DIAG: enable the sensor's internal colour-bar test pattern
   * (0x4501 bit3, matching sc2336_set_test_pattern() in Espressif's driver).
   *
   * The pattern is generated inside the sensor, so it drives the MIPI
   * transmit path, the CSI host, the bridge and DW-GDMA without depending on
   * the pixel array or on light reaching the lens.  It therefore separates
   * "the capture path is broken" from "the sensor never emits any packet".
   * Set SC2336_TEST_PATTERN to 0 to return to normal capture.
   */

#ifndef SC2336_TEST_PATTERN
#  define SC2336_TEST_PATTERN 0
#endif

#if SC2336_TEST_PATTERN
  {
    uint8_t tp = 0;
    int tret = sc2336_read_reg(priv, 0x4501, &tp);

    if (tret >= 0)
      {
        uint8_t want = (uint8_t)(tp | 0x08);

        tret = sc2336_write_reg(priv, 0x4501, want);
        syslog(LOG_INFO, "SC2336: test pattern 0x4501 0x%02x -> 0x%02x "
               "(ret=%d)\n", tp, want, tret);
      }
    else
      {
        syslog(LOG_ERR, "SC2336: test pattern read failed: %d\n", tret);
      }
  }
#endif

  /* The first frame after leaving sleep is not valid; the sensor needs a
   * few frame times before the output is stable.
   */

  nxsig_usleep(100 * 1000);

  priv->streaming = true;
  return OK;
}

/****************************************************************************
 * Name: sc2336_stop_capture
 ****************************************************************************/

static int sc2336_stop_capture(FAR struct imgsensor_s *sensor,
                               imgsensor_stream_type_t type)
{
  FAR struct sc2336_dev_s *priv = (FAR struct sc2336_dev_s *)sensor;
  int ret;

  if (!priv->streaming)
    {
      return OK;
    }

  ret = sc2336_write_reg(priv, SC2336_REG_SLEEP_MODE, 0x00);
  if (ret < 0)
    {
      _err("ERROR: SC2336: failed to stop streaming: %d\n", ret);
      return ret;
    }

  priv->streaming = false;
  return OK;
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

  if (size != sizeof(int32_t))
    {
      return -EINVAL;
    }

  if (!priv->detected)
    {
      return -ENODEV;
    }

  switch (id)
    {
      case IMGSENSOR_ID_HFLIP_VIDEO:
        return sc2336_set_flip(priv, value.value32 != 0, priv->vflip);

      case IMGSENSOR_ID_VFLIP_VIDEO:
        return sc2336_set_flip(priv, priv->hflip, value.value32 != 0);

      case IMGSENSOR_ID_EXPOSURE_ABSOLUTE:
        return sc2336_set_exposure(priv, (uint32_t)value.value32);

      default:
        return -ENOTTY;
    }
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

  memset(priv, 0, sizeof(*priv));

  priv->sensor.ops    = &g_sc2336_ops;
  priv->sensor.fmtdescs     = g_sc2336_fmtdescs;
  priv->sensor.fmtdescs_num = sizeof(g_sc2336_fmtdescs) /
                              sizeof(g_sc2336_fmtdescs[0]);

  /* 必须声明支持的分辨率/帧率: v4l2_cap 用它们生成默认 frame setting,
   * 不声明就会退回 15 fps 的写死默认值 -> S_FMT 被本驱动自己的校验拒绝。
   */

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

  return OK;
}
