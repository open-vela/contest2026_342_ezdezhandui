/****************************************************************************
 * esp_hosted_official/port/os/nuttx/include/eh_host_port_nuttx_supplement.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * What the stack's translation units need on top of the vendored master config.
 *
 * Force-included after eh_host_port_master_config.h for the stack's own sources
 * (see the port's CMakeLists).  It exists because the Wi-Fi feature includes
 * esp_wifi.h and then uses two things that live in other IDF headers, plus one
 * IDF build constant the tree's header expects to have been configured:
 *
 *   wifi_config_t          -> esp_wifi_types_generic.h
 *   wifi_sta_list_t        -> esp_wifi_ap_get_sta_list.h (which needs the two
 *                             esp_netif station types the port declares in its
 *                             esp_netif_types.h shim)
 *   CONFIG_ESP_WIFI_RX_BA_WIN -> an IDF Wi-Fi build constant (default 6)
 *
 * Pulling in the real headers keeps the types authoritative; nothing here
 * re-defines an IDF type.
 */

#ifndef __EH_HOST_PORT_NUTTX_SUPPLEMENT_H
#define __EH_HOST_PORT_NUTTX_SUPPLEMENT_H

#include "esp_wifi_types_generic.h"

#ifndef CONFIG_ESP_WIFI_RX_BA_WIN
#  define CONFIG_ESP_WIFI_RX_BA_WIN 6
#endif

#endif /* __EH_HOST_PORT_NUTTX_SUPPLEMENT_H */
