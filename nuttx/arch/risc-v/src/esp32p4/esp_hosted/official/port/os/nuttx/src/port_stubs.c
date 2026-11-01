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

/****************************************************************************
 * Name: esp_err_to_name
 *
 * Description:
 *   The stack logs errors through this name lookup.  Only the codes this port
 *   can produce are named; anything else is reported as "UNKNOWN", which is what
 *   ESP-IDF does for codes it does not know either.
 */

FAR const char *esp_err_to_name(esp_err_t code)
{
  switch (code)
    {
      case ESP_OK:                   return "ESP_OK";
      case ESP_FAIL:                 return "ESP_FAIL";
      case ESP_ERR_NO_MEM:           return "ESP_ERR_NO_MEM";
      case ESP_ERR_INVALID_ARG:      return "ESP_ERR_INVALID_ARG";
      case ESP_ERR_INVALID_STATE:    return "ESP_ERR_INVALID_STATE";
      case ESP_ERR_INVALID_SIZE:     return "ESP_ERR_INVALID_SIZE";
      case ESP_ERR_NOT_FOUND:        return "ESP_ERR_NOT_FOUND";
      case ESP_ERR_NOT_SUPPORTED:    return "ESP_ERR_NOT_SUPPORTED";
      case ESP_ERR_TIMEOUT:          return "ESP_ERR_TIMEOUT";
      case ESP_ERR_INVALID_RESPONSE: return "ESP_ERR_INVALID_RESPONSE";
      case ESP_ERR_INVALID_CRC:      return "ESP_ERR_INVALID_CRC";
      case ESP_ERR_INVALID_VERSION:  return "ESP_ERR_INVALID_VERSION";
      case ESP_ERR_INVALID_MAC:      return "ESP_ERR_INVALID_MAC";
      case ESP_ERR_NOT_FINISHED:     return "ESP_ERR_NOT_FINISHED";
      case ESP_ERR_NOT_ALLOWED:      return "ESP_ERR_NOT_ALLOWED";
      default:                       return "UNKNOWN ERROR";
    }
}
