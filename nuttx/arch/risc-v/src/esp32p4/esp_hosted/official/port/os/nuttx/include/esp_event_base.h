/****************************************************************************
 * esp_hosted_official/port/os/nuttx/include/esp_event_base.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Event base tag, as ESP-IDF declares it.  The stack only uses it to tag the
 * events it posts; the NuttX port's own event plumbing lives in esp_event.h.
 */

#ifndef __ESP_EVENT_BASE_H_SHIM__
#define __ESP_EVENT_BASE_H_SHIM__

#ifdef __cplusplus
extern "C" {
#endif

typedef const char *esp_event_base_t;

/* ESP-IDF puts these next to the type, and the stack's event-bases translation
 * unit includes only this header -- so they have to live here too.
 */

#define ESP_EVENT_DECLARE_BASE(id)     extern esp_event_base_t const id
#define ESP_EVENT_DEFINE_BASE(id)      esp_event_base_t const id = #id

#ifdef __cplusplus
}
#endif

#endif /* __ESP_EVENT_BASE_H_SHIM__ */
