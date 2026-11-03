/****************************************************************************
 * apps/examples/deskmate/src/dm_log.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Console logging for DeskMate.  Everything the test scripts assert on is
 * printed through these macros so the evidence is identical on the board, in
 * the simulator and in the goldfish emulator (where only the console is
 * reachable).
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_DESKMATE_DM_LOG_H
#define __APPS_EXAMPLES_DESKMATE_DM_LOG_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdio.h>
#include <syslog.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* DM_LOG 按"来源函数"限速：同一 key 每秒最多一行，被丢掉的条数会附在下一行后面。
 *
 * 理由：115200 控制台同时跑内核日志，任何一类日志打爆它都会让应用自己的 printf
 * 阻塞（真机实测会把 UI 冻住）。限速在 dm_log.c 里实现。
 */

void dm_log_rate(const char *key, const char *fmt, ...);

#define DM_LOG(fmt, ...) dm_log_rate(__func__, fmt, ##__VA_ARGS__)

#define DM_WARN(fmt, ...) \
  do { printf("[dm][warn] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } while (0)

#define DM_ERR(fmt, ...) \
  do { printf("[dm][err] " fmt "\n", ##__VA_ARGS__); \
       syslog(LOG_ERR, "[dm] " fmt, ##__VA_ARGS__); fflush(stdout); } while (0)

/* Marker lines the scripts match on */

#define DM_CHECK(name, ok, fmt, ...) \
  do { printf("[dm-check] %-28s %s " fmt "\n", name, (ok) ? "PASS" : "FAIL", \
              ##__VA_ARGS__); fflush(stdout); } while (0)

#endif /* __APPS_EXAMPLES_DESKMATE_DM_LOG_H */
