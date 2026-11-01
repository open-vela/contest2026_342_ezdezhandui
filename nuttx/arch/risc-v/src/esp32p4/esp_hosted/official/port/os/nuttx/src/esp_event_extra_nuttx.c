/****************************************************************************
 * esp_hosted_official/port/os/nuttx/src/esp_event_extra_nuttx.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The one esp_event entry point the tree does not already provide.
 *
 * arch/risc-v/src/common/espressif (through esp-hal-3rdparty) already links
 * esp_event_post(), esp_event_handler_register() and
 * esp_event_handler_unregister(), and esp_timer_*(), esp_read_mac() as well —
 * measured: defining them here is a multiple-definition error at link time.  So
 * this port declares the API in its headers (which the stack compiles against)
 * and implements only what is missing.
 *
 * The default event loop is the missing piece.  The stack registers handlers
 * before posting and the tree's esp_event delivers in the poster's context, so
 * there is nothing to start; returning success is what the stack needs.
 */

#include <nuttx/config.h>

#include "esp_event.h"

#ifdef CONFIG_ESPRESSIF_HOSTED_OFFICIAL

esp_err_t esp_event_loop_create_default(void)
{
  return ESP_OK;
}

#endif /* CONFIG_ESPRESSIF_HOSTED_OFFICIAL */
