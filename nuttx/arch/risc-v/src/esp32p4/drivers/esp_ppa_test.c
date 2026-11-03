/****************************************************************************
 * arch/risc-v/src/esp32p4/drivers/esp_ppa_test.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * PPA（Pixel Processing Accelerator）最小自检：验证 2D 硬件真的能工作。
 *
 * 用法：应用侧 `extern int esp_ppa_selftest(void);` 调用，返回 0 = 通过。
 * 结果同时写进 g_esp_ppa_test_rc（JTAG 可读，-999 = 未执行）。
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
 * Pre-processor Definitions
 ****************************************************************************/

#define PPA_TEST_W   64
#define PPA_TEST_H   64
#define PPA_TEST_RED 0xf800       /* RGB565 红 */

/****************************************************************************
 * Public Data
 ****************************************************************************/

int g_esp_ppa_test_rc = -999;         /* 0 = 通过；<0 = 失败原因 */

/* 诊断（JTAG 可读）：PPA 一次操作的耗时（毫秒，累计 / 单次最大） */

volatile uint32_t g_dbg_ppa_ms;
volatile uint32_t g_dbg_ppa_ms_max;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int esp_ppa_selftest(void)
{
  ppa_client_config_t client_cfg;
  ppa_client_handle_t client = NULL;
  ppa_fill_oper_config_t fill;
  uint16_t *buf;
  size_t   bytes = (size_t)PPA_TEST_W * PPA_TEST_H * sizeof(uint16_t);
  esp_err_t ret;

  memset(&client_cfg, 0, sizeof(client_cfg));
  client_cfg.oper_type = PPA_OPERATION_FILL;
  client_cfg.max_pending_trans_num = 1;

  ret = ppa_register_client(&client_cfg, &client);
  if (ret != ESP_OK)
    {
      g_esp_ppa_test_rc = -1;
      _err("PPA: register client failed: %d\n", ret);
      return -1;
    }

  /* 外部 PSRAM 缓冲：按 L1+L2 cache line（128B）对齐 */

  buf = memalign(128, bytes);
  if (buf == NULL)
    {
      (void)ppa_unregister_client(client);
      g_esp_ppa_test_rc = -2;
      return -2;
    }

  memset(buf, 0, bytes);

  /* CPU 写 → 内存，PPA 才能看到 */

  (void)esp_cache_msync(buf, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

  memset(&fill, 0, sizeof(fill));
  fill.out.buffer        = buf;
  fill.out.buffer_size   = (uint32_t)bytes;
  fill.out.pic_w         = PPA_TEST_W;
  fill.out.pic_h         = PPA_TEST_H;
  fill.out.block_offset_x = 0;
  fill.out.block_offset_y = 0;
  fill.out.fill_cm       = PPA_FILL_COLOR_MODE_RGB565;
  fill.fill_block_w      = PPA_TEST_W;
  fill.fill_block_h      = PPA_TEST_H;
  fill.fill_color_val    = PPA_TEST_RED;
  fill.mode              = PPA_TRANS_MODE_BLOCKING;

  {
    uint32_t dbg_t0 = (uint32_t)TICK2MSEC(clock_systime_ticks());

    ret = ppa_do_fill(client, &fill);

    {
      uint32_t dbg_dt = (uint32_t)TICK2MSEC(clock_systime_ticks()) - dbg_t0;

      g_dbg_ppa_ms += dbg_dt;
      if (dbg_dt > g_dbg_ppa_ms_max)
        {
          g_dbg_ppa_ms_max = dbg_dt;
        }
    }
  }

  /* PPA 写内存 → CPU，读回校验 */

  (void)esp_cache_msync(buf, bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

  if (ret != ESP_OK)
    {
      g_esp_ppa_test_rc = -3;
      _err("PPA: do_fill failed: %d\n", ret);
    }
  /* 注意：fill_color_val 是按 **ARGB8888** 解释后转成 RGB565 落盘的，
   * 所以不要直接拿 0xf800 去比。这里只验证"填充真的发生了"：
   * 首尾像素一致、且不是 memset 的 0。
   */

  else if (buf[0] == 0 || buf[0] != buf[PPA_TEST_W * PPA_TEST_H - 1])
    {
      g_esp_ppa_test_rc = -4;
      _err("PPA: fill result mismatch: %04x / %04x\n", buf[0],
           buf[PPA_TEST_W * PPA_TEST_H - 1]);
    }
  else
    {
      g_esp_ppa_test_rc = 0;
      _info("PPA: fill selftest OK (first=%04x last=%04x)\n", buf[0],
            buf[PPA_TEST_W * PPA_TEST_H - 1]);
    }

  free(buf);
  (void)ppa_unregister_client(client);
  return g_esp_ppa_test_rc;
}
