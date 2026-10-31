/****************************************************************************
 * esp_hosted_official/port/os/nuttx/src/esp_mac_nuttx.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MAC getters: the host does not own a Wi-Fi MAC of its own on this board (the
 * radio lives in the companion), so these report "not supported" instead of
 * inventing an address.
 */

#include <nuttx/config.h>

#include <string.h>

#include "esp_mac.h"

#ifdef CONFIG_ESPRESSIF_HOSTED_OFFICIAL

esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type)
{
  (void)type;

  if (mac == NULL)
    {
      return ESP_ERR_INVALID_ARG;
    }

  memset(mac, 0, 6);
  return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_efuse_mac_get_default(uint8_t *mac)
{
  return esp_read_mac(mac, ESP_MAC_WIFI_STA);
}

#endif /* CONFIG_ESPRESSIF_HOSTED_OFFICIAL */
