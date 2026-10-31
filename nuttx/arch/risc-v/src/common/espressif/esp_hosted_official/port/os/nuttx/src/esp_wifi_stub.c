/****************************************************************************
 * esp_hosted_official/port/os/nuttx/src/esp_wifi_stub.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * No local Wi-Fi: the radio is in the companion, and the host reaches it over
 * esp-hosted's RPC.  These definitions exist so the declared API resolves, and
 * they say plainly that a local radio is not what this port offers.
 */

#include <nuttx/config.h>

#include "esp_wifi.h"

#ifdef CONFIG_ESPRESSIF_HOSTED_OFFICIAL

esp_err_t esp_wifi_start(void)
{
  return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_wifi_stop(void)
{
  return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *config, bool block)
{
  (void)config;
  (void)block;
  return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_wifi_scan_stop(void)
{
  return ESP_ERR_NOT_SUPPORTED;
}

#endif /* CONFIG_ESPRESSIF_HOSTED_OFFICIAL */
