/****************************************************************************
 * esp_hosted_official/port/os/nuttx/include/esp_netif_types.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The typedef subset of ESP-IDF's esp_netif that the transport sources need
 * unconditionally.
 *
 * Route B does not port esp_netif: the frames go to a NuttX netdev instead.  The
 * transport sources still include this header at the top (and keep a pointer to
 * a netif in a couple of places), while the code that actually calls
 * esp_netif_*() sits behind EH_HOST_HOST_USES_STATIC_NETIF, which the port's
 * master config leaves at 0.  So only the types -- and stubs for the calls that
 * a future wiring may make -- are needed here.
 */

#ifndef __ESP_NETIF_TYPES_H_SHIM__
#define __ESP_NETIF_TYPES_H_SHIM__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_netif_obj esp_netif_t;

typedef enum
{
  ESP_NETIF_DHCP_CLIENT = 1,
  ESP_NETIF_DHCP_SERVER = 2
} esp_netif_dhcp_status_t;

typedef struct esp_netif_inherent_config_s
{
  uint32_t flags;
} esp_netif_inherent_config_t;

typedef struct
{
  const esp_netif_inherent_config_t *base;
  const void                        *driver;
  int                                stack;
} esp_netif_config_t;

extern const esp_netif_inherent_config_t esp_netif_base_default_wifi_sta;

#define ESP_NETIF_BASE_DEFAULT_WIFI_STA   (&esp_netif_base_default_wifi_sta)
#define ESP_NETIF_NETSTACK_DEFAULT_WIFI_STA 0

#ifdef __cplusplus
}
#endif

#endif /* __ESP_NETIF_TYPES_H_SHIM__ */
