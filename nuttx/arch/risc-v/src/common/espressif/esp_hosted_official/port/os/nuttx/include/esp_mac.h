/****************************************************************************
 * esp_hosted_official/port/os/nuttx/include/esp_mac.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * MAC type identifiers and address getters, stubbed for the NuttX port.
 *
 * The host does not read the local MAC from the companion here; the byte
 * values the stack compares are the remote (companion) MACs, which arrive over
 * the protocol.  So the getters report "not supported" rather than inventing
 * an address.
 */

#ifndef __ESP_MAC_H_SHIM__
#define __ESP_MAC_H_SHIM__

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Values mirror ESP-IDF's esp_mac_type_t: the stack reports which source an
 * address came from, and a mismatch would silently change that mapping.
 */

typedef enum
{
  ESP_MAC_WIFI_STA = 0,
  ESP_MAC_WIFI_SOFTAP,
  ESP_MAC_BT,
  ESP_MAC_ETH,
  ESP_MAC_IEEE802154,
  ESP_MAC_BASE,
  ESP_MAC_EFUSE_FACTORY,
  ESP_MAC_EFUSE_CUSTOM,
  ESP_MAC_EFUSE_EXT,
} esp_mac_type_t;

esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type);
esp_err_t esp_efuse_mac_get_default(uint8_t *mac);

#ifdef __cplusplus
}
#endif

#endif /* __ESP_MAC_H_SHIM__ */
