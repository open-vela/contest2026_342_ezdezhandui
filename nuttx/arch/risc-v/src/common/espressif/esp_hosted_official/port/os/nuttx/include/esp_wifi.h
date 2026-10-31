/****************************************************************************
 * esp_hosted_official/port/os/nuttx/include/esp_wifi.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The slice of ESP-IDF's esp_wifi.h the host stack's headers include.
 *
 * The radio lives in the companion and is driven over esp-hosted's RPC, so the
 * port keeps the stack's own API (eh_host_wifi_*) and does not port
 * esp_wifi_remote.  Two entry points are still referenced, so they are declared
 * here and answered with ESP_ERR_NOT_SUPPORTED in esp_wifi_stub.c rather than
 * left undefined.
 */

#ifndef __ESP_WIFI_H_SHIM__
#define __ESP_WIFI_H_SHIM__

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
  uint8_t ssid[32];
  uint8_t bssid[6];
  uint8_t channel;
  uint8_t hidden;
} wifi_scan_config_t;

typedef struct
{
  uint8_t bssid[6];
  uint8_t ssid[32];
  uint8_t channel;
  int8_t  rssi;
  uint8_t authmode;
} wifi_ap_record_t;

esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool block);
esp_err_t esp_wifi_scan_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* __ESP_WIFI_H_SHIM__ */
