/****************************************************************************
 * esp_hosted_official/port/os/nuttx/src/esp_timer_nuttx.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * esp_timer stubs for the NuttX port.
 *
 * The already-vendored stack sources create one timer (the RPC UID refresh); the
 * port neither needs it for correctness nor can honour periodic delivery
 * cheaply.  Creating it succeeds and starting it is accepted so the bring-up
 * path keeps its shape, but the callback is never fired.  When that timer
 * becomes load-bearing (RPC), this is where a NuttX wdog belongs.
 */

#include <nuttx/config.h>

#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"

#ifdef CONFIG_ESPRESSIF_HOSTED_OFFICIAL

struct esp_timer_s
{
  esp_timer_create_args_t args;
  bool                    running;
};

esp_err_t esp_timer_create(const esp_timer_create_args_t *args,
                           esp_timer_handle_t *handle)
{
  struct esp_timer_s *t;

  if (args == NULL || handle == NULL || args->callback == NULL)
    {
      return ESP_ERR_INVALID_ARG;
    }

  t = (struct esp_timer_s *)calloc(1, sizeof(*t));
  if (t == NULL)
    {
      return ESP_ERR_NO_MEM;
    }

  t->args = *args;
  *handle = t;
  return ESP_OK;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t handle, uint64_t timeout_us)
{
  (void)timeout_us;

  if (handle == NULL)
    {
      return ESP_ERR_INVALID_ARG;
    }

  handle->running = true;
  return ESP_OK;
}

esp_err_t esp_timer_start_periodic(esp_timer_handle_t handle, uint64_t period_us)
{
  return esp_timer_start_once(handle, period_us);
}

esp_err_t esp_timer_stop(esp_timer_handle_t handle)
{
  if (handle == NULL)
    {
      return ESP_ERR_INVALID_ARG;
    }

  handle->running = false;
  return ESP_OK;
}

esp_err_t esp_timer_delete(esp_timer_handle_t handle)
{
  if (handle == NULL)
    {
      return ESP_ERR_INVALID_ARG;
    }

  free(handle);
  return ESP_OK;
}

#endif /* CONFIG_ESPRESSIF_HOSTED_OFFICIAL */
