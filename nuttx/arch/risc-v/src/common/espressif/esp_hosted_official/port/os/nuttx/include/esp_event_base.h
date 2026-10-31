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

#ifdef __cplusplus
}
#endif

#endif /* __ESP_EVENT_BASE_H_SHIM__ */
