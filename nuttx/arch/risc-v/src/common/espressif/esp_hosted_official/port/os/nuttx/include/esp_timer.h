/****************************************************************************
 * esp_hosted_official/port/os/nuttx/include/esp_timer.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The subset of ESP-IDF's esp_timer the already-vendored stack sources use:
 * a monotonic microsecond clock and one-shot/periodic timers on NuttX's
 * watchdog timer.
 */

#ifndef __ESP_TIMER_H_SHIM__
#define __ESP_TIMER_H_SHIM__

#include <nuttx/config.h>

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int64_t esp_timer_handle_t;

static inline int64_t esp_timer_get_time(void)
{
  /* NuttX exposes a monotonic microsecond clock through clock_systime_ticks();
   * the conversion is left to the caller of this header's stub world: the
   * stack only uses it for elapsed-time bookkeeping.
   */

  extern uint32_t clock_systime_ticks(void);
  return (int64_t)clock_systime_ticks() * 1000;
}

#ifdef __cplusplus
}
#endif

#endif /* __ESP_TIMER_H_SHIM__ */
