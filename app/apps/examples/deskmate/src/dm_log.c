/****************************************************************************
 * apps/examples/deskmate/src/dm_log.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * DM_LOG 的限速实现。
 *
 * 为什么需要：115200 的同一个控制台既跑应用输出、又跑 esp_hosted/SDIO 等内核
 * 日志。一旦某类日志把控制台灌满，应用自己的 printf 会**阻塞**，UI 线程随之停住
 * （真机实测：画面冻住 + 触摸无响应）。所以 DM_LOG 按"来源 key（__func__）"限速，
 * 每秒最多一行，并把被丢掉的条数汇总出来，保证信息不丢、控制台不炸。
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <nuttx/clock.h>

#include "dm_log.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define DM_LOG_TABLE 24          /* 能区分的调用点数（__func__ 去重） */
#define DM_LOG_INTERVAL_MS 1000  /* 同一 key 每秒最多一行 */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct dm_log_entry_s
{
  const char *key;
  uint32_t    last_ms;
  uint32_t    dropped;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct dm_log_entry_s g_dm_log_table[DM_LOG_TABLE];
static unsigned int g_dm_log_used;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void dm_log_rate(const char *key, const char *fmt, ...)
{
  struct dm_log_entry_s *entry = NULL;
  uint32_t now = (uint32_t)TICK2MSEC(clock_systime_ticks());
  uint32_t dropped;
  va_list  ap;
  unsigned int i;

  for (i = 0; i < g_dm_log_used; i++)
    {
      /* 必须比对**完整**函数名。
       *
       * 这里原本只比前 4 个字符，于是任何同前缀的两个函数会被当成同一个
       * 日志来源而互相限速 —— 实测踩到：`dm_wlan_autostart()` 与
       * `dm_write_wifi_bss()` 都以 "dm_w" 开头，无线自启的几行日志被静默
       * 丢掉，控制台上看不到任何 wlan 证据（限速本身"工作正常"，只是丢了
       * 不属于它的行）。
       */

      if (g_dm_log_table[i].key == key ||
          (g_dm_log_table[i].key != NULL && key != NULL &&
           strcmp(g_dm_log_table[i].key, key) == 0))
        {
          entry = &g_dm_log_table[i];
          break;
        }
    }

  if (entry == NULL && g_dm_log_used < DM_LOG_TABLE)
    {
      entry = &g_dm_log_table[g_dm_log_used++];
      entry->key     = key;
      entry->last_ms = 0;
      entry->dropped = 0;
    }

  /* 表满（调用点太多）时不限速，宁可多打也不能丢信息。 */

  if (entry != NULL && entry->last_ms != 0 &&
      (int32_t)(now - entry->last_ms) < DM_LOG_INTERVAL_MS)
    {
      entry->dropped++;
      return;
    }

  dropped = entry != NULL ? entry->dropped : 0;

  if (entry != NULL)
    {
      entry->dropped = 0;
      entry->last_ms = now;
    }

  printf("[dm] ");
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);

  if (dropped > 0)
    {
      printf("  (+%u 条同源日志被限速)", dropped);
    }

  printf("\n");
  fflush(stdout);
}

unsigned int dm_log_dropped_total(void)
{
  unsigned int total = 0;
  unsigned int i;

  for (i = 0; i < g_dm_log_used; i++)
    {
      total += g_dm_log_table[i].dropped;
    }

  return total;
}
