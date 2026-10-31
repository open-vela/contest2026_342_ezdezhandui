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

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_timer_s *esp_timer_handle_t;

typedef void (*esp_timer_cb_t)(void *arg);

typedef enum
{
  ESP_TIMER_TASK = 0,
  ESP_TIMER_ISR,
} esp_timer_dispatch_t;

typedef struct
{
  esp_timer_cb_t       callback;
  void                *arg;
  esp_timer_dispatch_t dispatch_method;
  const char          *name;
  bool                 skip_unhandled_events;
} esp_timer_create_args_t;

static inline int64_t esp_timer_get_time(void)
{
  /* NuttX exposes a monotonic microsecond clock through clock_systime_ticks();
   * the conversion is left to the caller of this header's stub world: the
   * stack only uses it for elapsed-time bookkeeping.
   */

  extern uint32_t clock_systime_ticks(void);
  return (int64_t)clock_systime_ticks() * 1000;
}

esp_err_t esp_timer_create(const esp_timer_create_args_t *args,
                           esp_timer_handle_t *handle);
esp_err_t esp_timer_start_once(esp_timer_handle_t handle, uint64_t timeout_us);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t handle, uint64_t period_us);
esp_err_t esp_timer_stop(esp_timer_handle_t handle);
esp_err_t esp_timer_delete(esp_timer_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif /* __ESP_TIMER_H_SHIM__ */
