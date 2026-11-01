/****************************************************************************
 * esp_hosted_official/port/os/nuttx/include/esp_event.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A minimal default event loop for the NuttX port of esp-hosted.
 *
 * The stack posts its bring-up events (transport up, slave ready, ...) through
 * esp_event_post(); the host core only needs them delivered to the handlers it
 * registers.  Implementation lives in src/esp_event_nuttx.c and is deliberately
 * small: one fixed handler table, delivery in the poster's context.
 */

#ifndef __ESP_EVENT_H_SHIM__
#define __ESP_EVENT_H_SHIM__

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event_base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *esp_event_handler_instance_t;

/* Base declaration/definition macros.  The stack declares its event base with
 * ESP_EVENT_DECLARE_BASE() in a header and defines it once in a source file;
 * both spell out to the same tag pointer on NuttX.
 */

#define ESP_EVENT_DECLARE_BASE(id)     extern esp_event_base_t const id
#define ESP_EVENT_DEFINE_BASE(id)      esp_event_base_t const id = #id
typedef void (*esp_event_handler_t)(void *arg, esp_event_base_t base,
                                    int32_t id, void *data);

typedef struct esp_event_loop_s esp_event_loop_handle_t;

esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_handler_register(esp_event_base_t base, int32_t id,
                                     esp_event_handler_t handler, void *arg);
esp_err_t esp_event_handler_unregister(esp_event_base_t base, int32_t id,
                                       esp_event_handler_t handler);
esp_err_t esp_event_post(esp_event_base_t base, int32_t id,
                         const void *data, size_t size, uint32_t ticks);

#ifdef __cplusplus
}
#endif

#endif /* __ESP_EVENT_H_SHIM__ */
