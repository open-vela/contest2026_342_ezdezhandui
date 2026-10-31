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

/* Configuration handed to esp_wifi_init().  The host has no local radio, but
 * the stack's Wi-Fi headers declare one, so the shape has to exist.
 */

typedef struct
{
  void    *event_handler;
  void    *osi_funcs;
  void    *wpa_crypto_funcs;
  int      static_rx_buf_num;
  int      dynamic_rx_buf_num;
  int      tx_buf_type;
  int      static_tx_buf_num;
  int      dynamic_tx_buf_num;
  int      csi_enable;
  int      ampdu_rx_enable;
  int      ampdu_tx_enable;
  int      nvs_enable;
  int      nano_enable;
  int      magic;
} wifi_init_config_t;

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
