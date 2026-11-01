/****************************************************************************
 * esp_hosted_official/port/os/nuttx/include/esp_netif.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * esp_netif entry points, as no-op stubs for the NuttX port.
 *
 * The official stack calls these only when EH_HOST_HOST_USES_STATIC_NETIF is
 * set (route B keeps it 0 and wires the frames to a NuttX netdev instead), so
 * the stubs exist to keep a flag-on build linkable rather than to do anything.
 */

#ifndef __ESP_NETIF_H_SHIM__
#define __ESP_NETIF_H_SHIM__

#include "esp_err.h"
#include "esp_netif_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t esp_netif_init(void);
esp_netif_t *esp_netif_new(const esp_netif_config_t *cfg);
void esp_netif_destroy(esp_netif_t *netif);
esp_err_t esp_netif_attach_wifi_station(esp_netif_t *netif);
esp_err_t esp_netif_dhcpc_stop(esp_netif_t *netif);

#ifdef __cplusplus
}
#endif

#endif /* __ESP_NETIF_H_SHIM__ */
