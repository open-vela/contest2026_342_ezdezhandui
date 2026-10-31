/****************************************************************************
 * esp_hosted_official/port/os/nuttx/src/port_stubs.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * stubs for ESP-IDF entry points that the port does not implement.  Only
 * reached when a feature flag that route B keeps off is turned on.
 */

#include <nuttx/config.h>

#include "esp_err.h"
#include "esp_netif.h"

const esp_netif_inherent_config_t esp_netif_base_default_wifi_sta = { 0 };

esp_err_t esp_netif_init(void)
{
  return ESP_OK;
}

esp_netif_t *esp_netif_new(const esp_netif_config_t *cfg)
{
  (void)cfg;
  return (esp_netif_t *)0;
}

void esp_netif_destroy(esp_netif_t *netif)
{
  (void)netif;
}

esp_err_t esp_netif_attach_wifi_station(esp_netif_t *netif)
{
  (void)netif;
  return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_netif_dhcpc_stop(esp_netif_t *netif)
{
  (void)netif;
  return ESP_ERR_NOT_SUPPORTED;
}
