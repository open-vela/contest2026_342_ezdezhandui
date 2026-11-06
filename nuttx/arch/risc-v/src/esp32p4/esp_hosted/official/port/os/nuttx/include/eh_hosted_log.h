/****************************************************************************
 * esp_hosted_official/port/os/nuttx/include/eh_hosted_log.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * One place that decides which esp-hosted port diagnostics are compiled in.
 *
 * Why this exists
 * ---------------
 * This board's only console is the 115200 UART, and the application writes to
 * it too.  A full console blocks printf() and freezes the UI (measured).  The
 * port used to scatter that policy over three different switches:
 *
 *   - CONFIG_ESPRESSIF_HOSTED_LOG_VERBOSE  (per-transfer SDIO traces)
 *   - a local EH_VERBOSE numeric macro     (entry's bring-up lines)
 *   - a bare syslog(LOG_INFO, ...)         (everything else, always on)
 *
 * The third group is the one that flooded the console: 26 sites in the netdev
 * and 14 in the entry file printed INFO on every successful operation, with no
 * switch at all.  Grepping for "eh_hosted_netdev:" found them, but there was
 * nothing to turn off.
 *
 * Two levels, both compile-time
 * -----------------------------
 *   EH_INFO   -- bring-up and state changes: RPC return codes, scan results,
 *                "wlan0 up", "DHCP got x.x.x.x", "address applied".  Default
 *                OFF.  These are the lines a normal boot produced.
 *   EH_TRACE  -- per-frame / per-ioctl detail.  Default OFF, and much louder.
 *
 * When a level is compiled out, the call and its arguments disappear
 * completely: no format string in .rodata, no args evaluated, no runtime
 * branch.  That is the point of doing it in the preprocessor rather than with
 * setlogmask() -- the strings and the varargs marshalling are the cost, and a
 * runtime mask leaves both in the image.
 *
 * Severity is never suppressed by either switch: LOG_ERR and LOG_WARNING are
 * unconditional.  EH_INFO_OR_ERR keeps the failure half of a
 * "print INFO when it worked, ERR when it did not" line, so a failure is still
 * visible with INFO compiled out -- only the success line goes away.
 *
 * Turning them back on
 * --------------------
 *   CONFIG_ESPRESSIF_HOSTED_LOG_INFO=y     -> EH_INFO
 *   CONFIG_ESPRESSIF_HOSTED_LOG_VERBOSE=y  -> EH_TRACE (implies nothing)
 *
 * Both default n in Kconfig; a build that does not mention them gets neither.
 */

#ifndef __EH_HOSTED_LOG_H
#define __EH_HOSTED_LOG_H

#include <syslog.h>

/****************************************************************************
 * Bring-up / state-change lines
 ****************************************************************************/

#ifdef CONFIG_ESPRESSIF_HOSTED_LOG_INFO
#  define EH_INFO(...)          syslog(LOG_INFO, __VA_ARGS__)
#  define EH_INFO_OR_ERR(cond, ...)                                      \
     syslog((cond) ? LOG_INFO : LOG_ERR, __VA_ARGS__)
#  define EH_INFO_OR_WARN(cond, ...)                                     \
     syslog((cond) ? LOG_INFO : LOG_WARNING, __VA_ARGS__)
#else
#  define EH_INFO(...)          do { } while (0)

/* The success half is gone; the failure half is not.  (cond) is evaluated
 * once, and only the error branch formats its arguments.
 */

#  define EH_INFO_OR_ERR(cond, ...)                                      \
     do { if (!(cond)) syslog(LOG_ERR, __VA_ARGS__); } while (0)
#  define EH_INFO_OR_WARN(cond, ...)                                     \
     do { if (!(cond)) syslog(LOG_WARNING, __VA_ARGS__); } while (0)
#endif

/****************************************************************************
 * Per-frame / per-ioctl tracing
 *
 * A single WiFi operation produces dozens of these, so it is a separate
 * opt-in from EH_INFO: wanting the bring-up summary back is not the same as
 * wanting a hex dump per received frame.
 ****************************************************************************/

#ifdef CONFIG_ESPRESSIF_HOSTED_LOG_VERBOSE
#  define EH_TRACE(...)         syslog(LOG_INFO, __VA_ARGS__)
#else
#  define EH_TRACE(...)         do { } while (0)
#endif

#endif /* __EH_HOSTED_LOG_H */
