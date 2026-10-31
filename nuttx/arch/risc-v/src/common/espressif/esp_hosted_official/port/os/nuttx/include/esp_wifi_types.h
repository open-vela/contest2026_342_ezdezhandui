/****************************************************************************
 * esp_hosted_official/port/os/nuttx/include/esp_wifi_types.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi type names the transport sources include but do not use.
 *
 * Route B does not port esp_wifi: the radio is driven over esp-hosted's RPC,
 * not through ESP-IDF's Wi-Fi API.  The two sources that include this header
 * reference no esp_wifi_* symbol (checked), so the file only has to exist.
 */

#ifndef __ESP_WIFI_TYPES_H_SHIM__
#define __ESP_WIFI_TYPES_H_SHIM__

#include <stdint.h>

/* The handful of type names the transport headers declare.  The values mirror
 * ESP-IDF's so that any comparison the stack makes behaves the same.
 */

typedef enum
{
  WIFI_IF_STA = 0,
  WIFI_IF_AP  = 1,
  WIFI_IF_MAX,
} wifi_interface_t;

typedef enum
{
  WIFI_MODE_NULL = 0,
  WIFI_MODE_STA,
  WIFI_MODE_AP,
  WIFI_MODE_APSTA,
  WIFI_MODE_MAX,
} wifi_mode_t;

#endif /* __ESP_WIFI_TYPES_H_SHIM__ */
