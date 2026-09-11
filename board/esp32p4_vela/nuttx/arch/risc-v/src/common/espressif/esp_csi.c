/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_csi.c
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
#include <sys/time.h>

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/kmalloc.h>
#include <nuttx/spinlock.h>
#include <nuttx/video/imgdata.h>

#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_private/esp_cache_private.h"

#include "esp_ldo.h"
#include "esp_csi.h"

#ifdef CONFIG_ESPRESSIF_MIPI_CSI

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifdef CONFIG_ESPRESSIF_MIPI_CSI_H_RES
#  define ESP_CSI_DEFAULT_H_RES  CONFIG_ESPRESSIF_MIPI_CSI_H_RES
#else
#  define ESP_CSI_DEFAULT_H_RES  1920
#endif

#ifdef CONFIG_ESPRESSIF_MIPI_CSI_V_RES
#  define ESP_CSI_DEFAULT_V_RES  CONFIG_ESPRESSIF_MIPI_CSI_V_RES
#else
#  define ESP_CSI_DEFAULT_V_RES  1080
#endif

#ifdef CONFIG_ESPRESSIF_MIPI_CSI_LANES
#  define ESP_CSI_DEFAULT_LANES  CONFIG_ESPRESSIF_MIPI_CSI_LANES
#else
#  define ESP_CSI_DEFAULT_LANES  2
#endif

#ifdef CONFIG_ESPRESSIF_MIPI_CSI_LANE_BITRATE_MBPS
#  define ESP_CSI_DEFAULT_MBPS   CONFIG_ESPRESSIF_MIPI_CSI_LANE_BITRATE_MBPS
#else
#  define ESP_CSI_DEFAULT_MBPS   400
#endif

#ifdef CONFIG_ESPRESSIF_MIPI_CSI_QUEUE_ITEMS
#  define ESP_CSI_QUEUE_ITEMS    CONFIG_ESPRESSIF_MIPI_CSI_QUEUE_ITEMS
#else
#  define ESP_CSI_QUEUE_ITEMS    1
#endif

/* Smallest alignment the CSI frame buffers are allowed to use.  The real
 * requirement is the cache line size and is queried at run time with
 * esp_cache_get_alignment(); this is only the fallback.
 */

#define ESP_CSI_MIN_ALIGNMENT    64

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* CSI driver instance.  The imgdata interface has to come first so that the
 * structure can be cast back and forth.
 */

struct esp_csi_priv_s
{
  struct imgdata_s         data;

  struct esp_csi_config_s  config;
  struct esp_ldo_config_t  ldo;

  esp_cam_ctlr_handle_t    ctlr;
  bool                     ldo_acquired;
  bool                     ctlr_created;

  /* Frame buffer handoff between the V4L2 upper half (task context, and the
   * complete_capture() callback) and the DW-GDMA interrupt handler.
   *
   * next_buf  - buffer the upper half wants filled next
   * done_buf  - buffer the DMA finished, waiting to be reported upwards
   *
   * The ESP-IDF CSI HAL calls on_get_new_trans() and *then*
   * on_trans_finished() inside the same interrupt.  Reporting the finished
   * frame from on_get_new_trans() - rather than from on_trans_finished() -
   * lets the upper half return a fresh buffer from within the same
   * interrupt, so no frame has to be discarded.
   */

  volatile uint8_t        *next_buf;
  volatile uint32_t        next_len;
  volatile uint8_t        *done_buf;
  volatile uint32_t        done_len;
  struct timeval           done_ts;
  volatile bool            done_valid;

  size_t                   framebuffer_size;
  size_t                   framebuffer_align;
  uint32_t                 framebuffer_num;

  imgdata_capture_t        callback;
  FAR void                *cbarg;

  volatile bool            streaming;

  /* Diagnostics. */

  volatile uint32_t        nframes;
  volatile uint32_t        ndropped;
  volatile uint32_t        nreported;
};

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* CSI HAL callbacks (run in interrupt context). */

static bool esp_csi_get_new_trans(esp_cam_ctlr_handle_t handle,
                                  FAR esp_cam_ctlr_trans_t *trans,
                                  FAR void *user_data);
static bool esp_csi_trans_finished(esp_cam_ctlr_handle_t handle,
                                   FAR esp_cam_ctlr_trans_t *trans,
                                   FAR void *user_data);

/* imgdata lower half. */

static int esp_csi_data_init(FAR struct imgdata_s *data);
static int esp_csi_data_uninit(FAR struct imgdata_s *data);
static int esp_csi_data_set_buf(FAR struct imgdata_s *data,
                                uint8_t nr_datafmts,
                                FAR imgdata_format_t *datafmts,
                                FAR uint8_t *addr, uint32_t size);
static int
esp_csi_data_validate_frame_setting(FAR struct imgdata_s *data,
                                    uint8_t nr_datafmts,
                                    FAR imgdata_format_t *datafmts,
                                    FAR imgdata_interval_t *interval);
static int esp_csi_data_start_capture(FAR struct imgdata_s *data,
                                      uint8_t nr_datafmts,
                                      FAR imgdata_format_t *datafmts,
                                      FAR imgdata_interval_t *interval,
                                      imgdata_capture_t callback,
                                      FAR void *arg);
static int esp_csi_data_stop_capture(FAR struct imgdata_s *data);
static FAR void *esp_csi_data_alloc(FAR struct imgdata_s *data,
                                    uint32_t align_size, uint32_t size);
static void esp_csi_data_free(FAR struct imgdata_s *data, FAR void *addr);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct imgdata_ops_s g_esp_csi_ops =
{
  .init                   = esp_csi_data_init,
  .uninit                 = esp_csi_data_uninit,
  .set_buf                = esp_csi_data_set_buf,
  .validate_frame_setting = esp_csi_data_validate_frame_setting,
  .start_capture          = esp_csi_data_start_capture,
  .stop_capture           = esp_csi_data_stop_capture,
  .alloc                  = esp_csi_data_alloc,
  .free                   = esp_csi_data_free,
};

static struct esp_csi_priv_s g_esp_csi_priv;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp_csi_now
 *
 * Description:
 *   Take a timestamp for a completed frame.  Called from interrupt context,
 *   so only the tick based clock is used.
 *
 ****************************************************************************/

static void esp_csi_now(FAR struct timeval *ts)
{
  struct timespec nts;

  if (clock_systime_timespec(&nts) == OK)
    {
      ts->tv_sec  = nts.tv_sec;
      ts->tv_usec = nts.tv_nsec / 1000;
    }
  else
    {
      ts->tv_sec  = 0;
      ts->tv_usec = 0;
    }
}

/****************************************************************************
 * Name: esp_csi_get_new_trans
 *
 * Description:
 *   CSI HAL callback, invoked from the DW-GDMA interrupt before the transfer
 *   that just finished is reported.  Two things happen here:
 *
 *     1. the frame that completed during the *previous* transfer is handed
 *        to the V4L2 upper half.  The upper half recycles the used buffer
 *        from inside that callback, so
 *     2. a free buffer is available immediately afterwards and is given to
 *        the CSI DMA.
 *
 *   If no buffer is available the callback returns an empty transaction;
 *   the HAL then keeps streaming into its own backup buffer, and frames
 *   landing there are not reported to the upper half.  Data corruption is
 *   therefore impossible, at the cost of dropping frames.
 *
 ****************************************************************************/

static bool esp_csi_get_new_trans(esp_cam_ctlr_handle_t handle,
                                  FAR esp_cam_ctlr_trans_t *trans,
                                  FAR void *user_data)
{
  FAR struct esp_csi_priv_s *priv = (FAR struct esp_csi_priv_s *)user_data;
  FAR uint8_t *next;
  uint32_t len;

  if (priv->done_valid)
    {
      FAR uint8_t *done = (FAR uint8_t *)priv->done_buf;
      uint32_t dlen = priv->done_len;
      struct timeval ts = priv->done_ts;

      priv->done_valid = false;
      priv->done_buf   = NULL;

      if (done != NULL && priv->callback != NULL)
        {
          priv->callback(0, dlen, &ts, priv->cbarg);
          priv->nreported++;
        }
    }

  next = (FAR uint8_t *)priv->next_buf;
  len  = priv->next_len;

  if (next != NULL && len >= priv->framebuffer_size)
    {
      priv->next_buf = NULL;
      priv->next_len = 0;

      trans->buffer = next;
      trans->buflen = len;
    }
  else
    {
      /* Stay with the HAL backup buffer and drop this frame. */

      trans->buffer = NULL;
      trans->buflen = 0;
      priv->ndropped++;
    }

  return false;
}

/****************************************************************************
 * Name: esp_csi_trans_finished
 *
 * Description:
 *   CSI HAL callback, invoked from the DW-GDMA interrupt for every transfer
 *   that did not land in the driver backup buffer.  The frame is only
 *   recorded here; it is reported to the upper half from
 *   esp_csi_get_new_trans().
 *
 ****************************************************************************/

static bool esp_csi_trans_finished(esp_cam_ctlr_handle_t handle,
                                   FAR esp_cam_ctlr_trans_t *trans,
                                   FAR void *user_data)
{
  FAR struct esp_csi_priv_s *priv = (FAR struct esp_csi_priv_s *)user_data;

  priv->nframes++;

  if (priv->done_valid)
    {
      /* The upper half did not collect the previous frame in time.  Never
       * overwrite a pending frame silently; report the overrun instead.
       */

      priv->ndropped++;
    }

  priv->done_buf   = (FAR uint8_t *)trans->buffer;
  priv->done_len   = trans->received_size;
  priv->done_valid = true;
  esp_csi_now(&priv->done_ts);

  return false;
}

/****************************************************************************
 * Name: esp_csi_data_init
 ****************************************************************************/

static int esp_csi_data_init(FAR struct imgdata_s *data)
{
  FAR struct esp_csi_priv_s *priv = (FAR struct esp_csi_priv_s *)data;
  esp_cam_ctlr_csi_config_t csi_config;
  esp_cam_ctlr_evt_cbs_t cbs;
  size_t align = ESP_CSI_MIN_ALIGNMENT;
  uint32_t out_bpp;
  esp_err_t err;
  int ret;

  if (priv->ctlr_created)
    {
      return OK;
    }

  /* The MIPI PHY is powered from an on-chip LDO channel.  Acquire it before
   * touching the CSI registers.
   */

  if (priv->config.enable_ldo)
    {
      priv->ldo.chan_id    = priv->config.ldo_chan_id;
      priv->ldo.voltage_mv = priv->config.ldo_voltage_mv;

      ret = esp_ldo_channel_acquire(&priv->ldo);
      if (ret < 0)
        {
          _err("ERROR: Failed to acquire LDO channel %u: %d\n",
               priv->config.ldo_chan_id, ret);
          return ret;
        }

      priv->ldo_acquired = true;
    }

  /* Cache alignment required for the frame buffers.  esp_cache_msync()
   * aborts on an unaligned buffer, so this value must be honoured by
   * esp_csi_data_alloc().
   */

  if (esp_cache_get_alignment(MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA,
                              &align) != ESP_OK ||
      align < ESP_CSI_MIN_ALIGNMENT)
    {
      align = ESP_CSI_MIN_ALIGNMENT;
    }

  priv->framebuffer_align = align;

  out_bpp = (priv->config.output_color == CAM_CTLR_COLOR_RAW10) ? 10 :
            (priv->config.output_color == CAM_CTLR_COLOR_RAW12) ? 12 :
            (priv->config.output_color == CAM_CTLR_COLOR_RAW8)  ? 8  : 16;

  priv->framebuffer_size =
    ((size_t)priv->config.h_res * priv->config.v_res * out_bpp + 7) / 8;

  /* Round the frame size up to the cache line so that every frame the upper
   * half carves out of the buffer heap stays aligned.
   */

  priv->framebuffer_size =
    (priv->framebuffer_size + align - 1) & ~(size_t)(align - 1);

  memset(&csi_config, 0, sizeof(csi_config));

  csi_config.ctlr_id                = 0;
  csi_config.clk_src                = MIPI_CSI_PHY_CLK_SRC_DEFAULT;
  csi_config.h_res                  = priv->config.h_res;
  csi_config.v_res                  = priv->config.v_res;
  csi_config.data_lane_num          = priv->config.data_lane_num;
  csi_config.lane_bit_rate_mbps     = priv->config.lane_bit_rate_mbps;
  csi_config.input_data_color_type  =
    (cam_ctlr_color_t)priv->config.input_color;
  csi_config.output_data_color_type =
    (cam_ctlr_color_t)priv->config.output_color;
  csi_config.queue_items            = priv->config.queue_items;
  csi_config.byte_swap_en           = priv->config.byte_swap_en;
  csi_config.input_8bit_swap_en     = priv->config.input_8bit_swap_en;
  csi_config.input_16bit_swap_en    = priv->config.input_16bit_swap_en;
  csi_config.bk_buffer_dis          = !priv->config.keep_backup_buffer;

  err = esp_cam_new_csi_ctlr(&csi_config, &priv->ctlr);
  if (err != ESP_OK)
    {
      _err("ERROR: esp_cam_new_csi_ctlr failed: %d\n", (int)err);
      ret = -EIO;
      goto errout_ldo;
    }

  priv->ctlr_created = true;

  memset(&cbs, 0, sizeof(cbs));
  cbs.on_get_new_trans  = esp_csi_get_new_trans;
  cbs.on_trans_finished = esp_csi_trans_finished;

  err = esp_cam_ctlr_register_event_callbacks(priv->ctlr, &cbs, priv);
  if (err != ESP_OK)
    {
      _err("ERROR: register_event_callbacks failed: %d\n", (int)err);
      ret = -EIO;
      goto errout_ctlr;
    }

  err = esp_cam_ctlr_enable(priv->ctlr);
  if (err != ESP_OK)
    {
      _err("ERROR: esp_cam_ctlr_enable failed: %d\n", (int)err);
      ret = -EIO;
      goto errout_ctlr;
    }

  _info("CSI: %" PRIu32 "x%" PRIu32 ", %u lane(s) @ %" PRIu32 " Mbps, "
        "fb=%zu bytes (align %zu), color %" PRIu32 "->%" PRIu32 "\n",
        priv->config.h_res, priv->config.v_res,
        (unsigned)priv->config.data_lane_num,
        priv->config.lane_bit_rate_mbps, priv->framebuffer_size,
        priv->framebuffer_align, priv->config.input_color,
        priv->config.output_color);

  return OK;

errout_ctlr:
  esp_cam_ctlr_del(priv->ctlr);
  priv->ctlr = NULL;
  priv->ctlr_created = false;

errout_ldo:
  if (priv->ldo_acquired)
    {
      esp_ldo_channel_release(&priv->ldo);
      priv->ldo_acquired = false;
    }

  return ret;
}

/****************************************************************************
 * Name: esp_csi_data_uninit
 ****************************************************************************/

static int esp_csi_data_uninit(FAR struct imgdata_s *data)
{
  FAR struct esp_csi_priv_s *priv = (FAR struct esp_csi_priv_s *)data;

  if (priv->streaming)
    {
      esp_csi_data_stop_capture(data);
    }

  if (priv->ctlr_created)
    {
      esp_cam_ctlr_disable(priv->ctlr);
      esp_cam_ctlr_del(priv->ctlr);
      priv->ctlr = NULL;
      priv->ctlr_created = false;
    }

  if (priv->ldo_acquired)
    {
      esp_ldo_channel_release(&priv->ldo);
      priv->ldo_acquired = false;
    }

  priv->callback   = NULL;
  priv->cbarg      = NULL;
  priv->next_buf   = NULL;
  priv->next_len   = 0;
  priv->done_buf   = NULL;
  priv->done_valid = false;

  return OK;
}

/****************************************************************************
 * Name: esp_csi_data_set_buf
 *
 * Description:
 *   The V4L2 upper half hands over the buffer that should be filled next.
 *   This is called both from task context (start of a capture) and from
 *   interrupt context (from complete_capture(), for every frame that has
 *   been reported).
 *
 ****************************************************************************/

static int esp_csi_data_set_buf(FAR struct imgdata_s *data,
                                uint8_t nr_datafmts,
                                FAR imgdata_format_t *datafmts,
                                FAR uint8_t *addr, uint32_t size)
{
  FAR struct esp_csi_priv_s *priv = (FAR struct esp_csi_priv_s *)data;

  if (addr == NULL || size < priv->framebuffer_size)
    {
      _err("ERROR: buffer too small: %p, %" PRIu32 " < %zu\n", addr, size,
           priv->framebuffer_size);
      return -EINVAL;
    }

  /* Hand exactly one frame to the CSI HAL rather than the whole buffer that
   * the V4L2 upper half passed in.  The HAL cache-syncs the transaction
   * length on every transfer; for a RAW10 1080p frame the upper half
   * reports 2 bytes per pixel (4 MiB) while the frame itself is 2.5 MiB, so
   * using the reported length would invalidate 1.6 MiB of unrelated data on
   * every frame.
   */

  priv->next_len = priv->framebuffer_size;
  priv->next_buf = addr;

  return OK;
}

/****************************************************************************
 * Name: esp_csi_data_validate_frame_setting
 ****************************************************************************/

static int
esp_csi_data_validate_frame_setting(FAR struct imgdata_s *data,
                                    uint8_t nr_datafmts,
                                    FAR imgdata_format_t *datafmts,
                                    FAR imgdata_interval_t *interval)
{
  FAR struct esp_csi_priv_s *priv = (FAR struct esp_csi_priv_s *)data;

  if (datafmts == NULL || nr_datafmts == 0)
    {
      return -EINVAL;
    }

  /* Only the main frame is supported.  Resolution changes are not supported
   * yet: the CSI receiver window is fixed at initialize() time.
   */

  if (datafmts[IMGDATA_FMT_MAIN].width != priv->config.h_res ||
      datafmts[IMGDATA_FMT_MAIN].height != priv->config.v_res)
    {
      _err("ERROR: %ux%u is not supported, CSI is configured for %"
           PRIu32 "x%" PRIu32 "\n",
           datafmts[IMGDATA_FMT_MAIN].width,
           datafmts[IMGDATA_FMT_MAIN].height,
           priv->config.h_res, priv->config.v_res);
      return -EINVAL;
    }

  return OK;
}

/****************************************************************************
 * Name: esp_csi_data_start_capture
 ****************************************************************************/

static int esp_csi_data_start_capture(FAR struct imgdata_s *data,
                                      uint8_t nr_datafmts,
                                      FAR imgdata_format_t *datafmts,
                                      FAR imgdata_interval_t *interval,
                                      imgdata_capture_t callback,
                                      FAR void *arg)
{
  FAR struct esp_csi_priv_s *priv = (FAR struct esp_csi_priv_s *)data;
  int ret;

  if (priv->streaming)
    {
      return OK;
    }

  if (!priv->ctlr_created)
    {
      ret = esp_csi_data_init(data);
      if (ret < 0)
        {
          return ret;
        }
    }

  priv->callback = callback;
  priv->cbarg    = arg;

  /* A buffer must already have been handed over by IMGDATA_SET_BUF().  If it
   * has, esp_csi_get_new_trans() picks it up while the HAL primes the first
   * transfer.
   */

  if (esp_cam_ctlr_start(priv->ctlr) != ESP_OK)
    {
      _err("ERROR: esp_cam_ctlr_start failed\n");
      priv->callback = NULL;
      priv->cbarg    = NULL;
      return -EIO;
    }

  priv->streaming = true;
  return OK;
}

/****************************************************************************
 * Name: esp_csi_data_stop_capture
 ****************************************************************************/

static int esp_csi_data_stop_capture(FAR struct imgdata_s *data)
{
  FAR struct esp_csi_priv_s *priv = (FAR struct esp_csi_priv_s *)data;

  if (!priv->streaming)
    {
      return OK;
    }

  if (esp_cam_ctlr_stop(priv->ctlr) != ESP_OK)
    {
      _err("ERROR: esp_cam_ctlr_stop failed\n");
    }

  priv->streaming = false;

  /* Report a frame that completed but has not been handed upwards yet, so
   * that the upper half does not wait forever for it.
   */

  if (priv->done_valid && priv->callback != NULL)
    {
      struct timeval ts = priv->done_ts;

      priv->done_valid = false;
      priv->done_buf   = NULL;
      priv->callback(0, priv->done_len, &ts, priv->cbarg);
      priv->nreported++;
    }

  priv->callback = NULL;
  priv->cbarg    = NULL;
  priv->next_buf = NULL;
  priv->next_len = 0;

  return OK;
}

/****************************************************************************
 * Name: esp_csi_data_alloc
 *
 * Description:
 *   Frame buffer allocation used by the V4L2 upper half for the buffer heap.
 *   This must be cache line aligned, otherwise esp_cache_msync() on the
 *   ESP32-P4 rejects the buffer and the CSI DMA stops.
 *
 ****************************************************************************/

static FAR void *esp_csi_data_alloc(FAR struct imgdata_s *data,
                                    uint32_t align_size, uint32_t size)
{
  FAR struct esp_csi_priv_s *priv = (FAR struct esp_csi_priv_s *)data;
  size_t align = priv->framebuffer_align;

  if (align < align_size)
    {
      align = align_size;
    }

  return heap_caps_aligned_alloc(align, size,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA |
                                 MALLOC_CAP_8BIT);
}

/****************************************************************************
 * Name: esp_csi_data_free
 ****************************************************************************/

static void esp_csi_data_free(FAR struct imgdata_s *data, FAR void *addr)
{
  heap_caps_free(addr);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp_csi_get_default_config
 ****************************************************************************/

void esp_csi_get_default_config(FAR struct esp_csi_config_s *config)
{
  if (config == NULL)
    {
      return;
    }

  memset(config, 0, sizeof(*config));

  config->h_res               = ESP_CSI_DEFAULT_H_RES;
  config->v_res               = ESP_CSI_DEFAULT_V_RES;
  config->data_lane_num       = ESP_CSI_DEFAULT_LANES;
  config->lane_bit_rate_mbps  = ESP_CSI_DEFAULT_MBPS;

  /* Pass-through: the CSI bridge cannot convert RAW into RGB, so the first
   * bring-up captures the raw sensor stream.
   */

  config->input_color         = CAM_CTLR_COLOR_RAW10;
  config->output_color        = CAM_CTLR_COLOR_RAW10;

  config->byte_swap_en        = false;
  config->input_8bit_swap_en  = false;
  config->input_16bit_swap_en = false;

  config->enable_ldo          = true;
  config->ldo_chan_id         = ESP_CSI_LDO_CHAN_ID;
  config->ldo_voltage_mv      = ESP_CSI_LDO_VOLTAGE;

  config->keep_backup_buffer  = true;
  config->queue_items         = ESP_CSI_QUEUE_ITEMS;
}

/****************************************************************************
 * Name: esp_csi_imgdata_initialize
 ****************************************************************************/

FAR struct imgdata_s *
esp_csi_imgdata_initialize(FAR const struct esp_csi_config_s *config)
{
  FAR struct esp_csi_priv_s *priv = &g_esp_csi_priv;
  int ret;

  if (config == NULL)
    {
      set_errno(EINVAL);
      return NULL;
    }

  if (priv->ctlr_created)
    {
      set_errno(EBUSY);
      return NULL;
    }

  memset(priv, 0, sizeof(*priv));
  memcpy(&priv->config, config, sizeof(priv->config));

  priv->data.ops = &g_esp_csi_ops;

  ret = esp_csi_data_init(&priv->data);
  if (ret < 0)
    {
      set_errno(-ret);
      return NULL;
    }

  return &priv->data;
}

/****************************************************************************
 * Name: esp_csi_imgdata_uninitialize
 ****************************************************************************/

int esp_csi_imgdata_uninitialize(FAR struct imgdata_s *data)
{
  if (data == NULL || data->ops != &g_esp_csi_ops)
    {
      return -EINVAL;
    }

  return esp_csi_data_uninit(data);
}

/****************************************************************************
 * Name: esp_csi_frames
 ****************************************************************************/

void esp_csi_frames(FAR struct imgdata_s *data, FAR uint32_t *delivered,
                    FAR uint32_t *dropped, FAR uint32_t *errors)
{
  FAR struct esp_csi_priv_s *priv;

  if (data == NULL || data->ops != &g_esp_csi_ops)
    {
      return;
    }

  priv = (FAR struct esp_csi_priv_s *)data;

  if (delivered != NULL)
    {
      *delivered = priv->nreported;
    }

  if (dropped != NULL)
    {
      *dropped = priv->ndropped;
    }

  if (errors != NULL)
    {
      *errors = priv->nframes;
    }
}

#endif /* CONFIG_ESPRESSIF_MIPI_CSI */
