/****************************************************************************
 * arch/risc-v/src/esp32p4/drivers/esp_ppa_scale.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * PPA 硬件缩放：把相机的 RGB565 帧缩到预览尺寸（替代 CPU 逐像素缩放）。
 *
 * 用法：应用侧 `extern int esp_ppa_scale_rgb565(...)` 调用。
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <debug.h>

#include <nuttx/clock.h>

#include "driver/ppa.h"
#include "esp_cache.h"

/****************************************************************************
 * Public Data
 ****************************************************************************/

volatile uint32_t g_dbg_ppa_srm_ms;      /* 累计耗时（毫秒） */
volatile uint32_t g_dbg_ppa_srm_ms_max;  /* 单次最大（毫秒） */
volatile int      g_dbg_ppa_srm_rc;      /* 最近一次返回码 */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int esp_ppa_scale_rgb565(const void *src, int sw, int sh,
                         void *dst, int dw, int dh)
{
  static ppa_client_handle_t s_client;
  ppa_srm_oper_config_t cfg;
  esp_err_t ret;
  uint32_t t0;

  if (src == NULL || dst == NULL || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
    {
      return -EINVAL;
    }

  if (s_client == NULL)
    {
      ppa_client_config_t ccfg;

      memset(&ccfg, 0, sizeof(ccfg));
      ccfg.oper_type             = PPA_OPERATION_SRM;
      ccfg.max_pending_trans_num = 1;

      ret = ppa_register_client(&ccfg, &s_client);
      if (ret != ESP_OK)
        {
          _err("PPA: srm client register failed: %d\n", ret);
          g_dbg_ppa_srm_rc = (int)ret;
          return -EIO;
        }

      _info("PPA: SRM client ready\n");
    }

  /* CSI DMA 写入的输入帧：先做 M2C，保证 PPA 读到的是内存里的新数据 */

  (void)esp_cache_msync((void *)src, (size_t)sw * sh * 2,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C);

  memset(&cfg, 0, sizeof(cfg));

  cfg.in.buffer        = src;
  cfg.in.pic_w         = (uint32_t)sw;
  cfg.in.pic_h         = (uint32_t)sh;
  cfg.in.block_w       = (uint32_t)sw;
  cfg.in.block_h       = (uint32_t)sh;
  cfg.in.block_offset_x = 0;
  cfg.in.block_offset_y = 0;
  cfg.in.srm_cm        = PPA_SRM_COLOR_MODE_RGB565;

  cfg.out.buffer        = dst;
  cfg.out.buffer_size   = (uint32_t)dw * dh * 2;
  cfg.out.pic_w         = (uint32_t)dw;
  cfg.out.pic_h         = (uint32_t)dh;
  cfg.out.block_offset_x = 0;
  cfg.out.block_offset_y = 0;
  cfg.out.srm_cm        = PPA_SRM_COLOR_MODE_RGB565;

  cfg.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
  cfg.scale_x        = (float)dw / (float)sw;
  cfg.scale_y        = (float)dh / (float)sh;
  cfg.mirror_x       = false;
  cfg.mirror_y       = false;
  cfg.mode           = PPA_TRANS_MODE_BLOCKING;

  t0  = (uint32_t)TICK2MSEC(clock_systime_ticks());
  ret = ppa_do_scale_rotate_mirror(s_client, &cfg);
  t0  = (uint32_t)TICK2MSEC(clock_systime_ticks()) - t0;

  g_dbg_ppa_srm_ms += t0;
  if (t0 > g_dbg_ppa_srm_ms_max)
    {
      g_dbg_ppa_srm_ms_max = t0;
    }

  g_dbg_ppa_srm_rc = (int)ret;

  /* PPA 写好的输出要 M2C，CPU/LVGL 才能看到 */

  (void)esp_cache_msync(dst, (size_t)dw * dh * 2,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C);

  return ret == ESP_OK ? 0 : -EIO;
}
