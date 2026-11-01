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

#define DM_LOG(fmt, ...) \
  do { printf("[dm] " fmt "\n", ##__VA_ARGS__); fflush(stdout); } while (0)

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
