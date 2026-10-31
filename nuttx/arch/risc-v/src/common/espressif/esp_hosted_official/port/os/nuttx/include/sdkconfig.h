/* SPDX-License-Identifier: Apache-2.0 */

/* esp-hosted's sources include "sdkconfig.h"; on NuttX there is no Kconfig
 * file of that name, so this one exists only to let them build.  Values that
 * actually select behaviour are in eh_host_port_master_config.h.
 */

#ifndef __SDKCONFIG_H_SHIM__
#define __SDKCONFIG_H_SHIM__
#endif

/****************************************************************************
 * ESP-IDF Wi-Fi build constants
 *
 * The tree's own esp_wifi.h (needed by the stack's Wi-Fi feature) expands
 * WIFI_INIT_CONFIG_DEFAULT() into a wifi_init_config_t, and that macro reads
 * these build-time values.  The host has no local radio -- the radio is in the
 * companion and the values travel to it as part of the init RPC -- so these are
 * ESP-IDF's own defaults, kept here so the real header works unmodified instead
 * of being re-typed in this port.
 *
 * Numbers are ESP-IDF v5 defaults; the flags are 0 where the host does not use
 * the feature, which is also IDF's default for them in a plain station build.
 ****************************************************************************/

#define CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM     10
#define CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM    32
#define CONFIG_ESP_WIFI_DYNAMIC_RX_MGMT_BUF      0
#define CONFIG_ESP_WIFI_CACHE_TX_BUFFER_NUM      16
#define CONFIG_ESP_WIFI_TX_BUFFER_TYPE           1
#define CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM     16
#define CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM    32
#define CONFIG_ESP_WIFI_AMPDU_RX_ENABLED         1
#define CONFIG_ESP_WIFI_AMPDU_TX_ENABLED         1
#define CONFIG_ESP_WIFI_AMSDU_TX_ENABLED         0
#define CONFIG_ESP_WIFI_NVS_ENABLED              1
#define CONFIG_ESP_WIFI_CSI_ENABLED              0
#define CONFIG_ESP_WIFI_NAN_ENABLE               0
#define CONFIG_ESP_WIFI_MGMT_SBUF_NUM            16
#define CONFIG_ESP_WIFI_ESPNOW_MAX_ENCRYPT_NUM   7
#define CONFIG_ESP_WIFI_11R_SUPPORT              0
#define CONFIG_ESP_WIFI_BSS_MAX_IDLE_SUPPORT     1
#define CONFIG_ESP_WIFI_ENABLE_DUMP_HESIGB       0
#define CONFIG_ESP_WIFI_ENABLE_WPA3_SAE          1
#define CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT       0
#define CONFIG_ESP_WIFI_FTM_INITIATOR_SUPPORT    1
#define CONFIG_ESP_WIFI_FTM_RESPONDER_SUPPORT    1
#define CONFIG_ESP_WIFI_GCMP_SUPPORT             0
#define CONFIG_ESP_WIFI_GMAC_SUPPORT             0
#define CONFIG_ESP_WIFI_SOFTAP_SUPPORT           1
#define CONFIG_ESP_WIFI_TX_IRAM_OPT              0
#define CONFIG_ESP_WIFI_RX_IRAM_OPT              0
#define CONFIG_ESP_WIFI_SLP_IRAM_OPT             0
#define CONFIG_ESP_WIFI_IRAM_OPT                 1
#define CONFIG_ESP_WIFI_EXTRA_IRAM_OPT           0

/****************************************************************************
 * The remote Wi-Fi build constants
 *
 * The vendor's injected esp_wifi.h (the header set for esp_wifi_remote, which
 * is what a host without a local radio uses) reads these instead of the
 * CONFIG_ESP_WIFI_* names.  Values are ESP-IDF's Wi-Fi defaults: the host has
 * no local radio, and the numbers travel to the companion inside the init RPC.
 ****************************************************************************/

#define CONFIG_WIFI_RMT_11R_SUPPORT                    0
#define CONFIG_WIFI_RMT_AMPDU_RX_ENABLED               1
#define CONFIG_WIFI_RMT_AMPDU_TX_ENABLED               1
#define CONFIG_WIFI_RMT_AMSDU_TX_ENABLED               0
#define CONFIG_WIFI_RMT_BSS_MAX_IDLE_SUPPORT           1
#define CONFIG_WIFI_RMT_CACHE_TX_BUFFER_NUM            16
#define CONFIG_WIFI_RMT_CSI_ENABLED                    0
#define CONFIG_WIFI_RMT_DYNAMIC_RX_BUFFER_NUM          32
#define CONFIG_WIFI_RMT_DYNAMIC_RX_MGMT_BUF            0
#define CONFIG_WIFI_RMT_DYNAMIC_TX_BUFFER_NUM          32
#define CONFIG_WIFI_RMT_ENABLE_DUMP_HESIGB             0
#define CONFIG_WIFI_RMT_ENABLE_WPA3_OWE_SOFTAP         0
#define CONFIG_WIFI_RMT_ENABLE_WPA3_SAE                1
#define CONFIG_WIFI_RMT_ENTERPRISE_SUPPORT             0
#define CONFIG_WIFI_RMT_ESPNOW_MAX_ENCRYPT_NUM         7
#define CONFIG_WIFI_RMT_FTM_INITIATOR_SUPPORT          1
#define CONFIG_WIFI_RMT_FTM_RESPONDER_SUPPORT          1
#define CONFIG_WIFI_RMT_GCMP_SUPPORT                   0
#define CONFIG_WIFI_RMT_GMAC_SUPPORT                   0
#define CONFIG_WIFI_RMT_MGMT_SBUF_NUM                  16
#define CONFIG_WIFI_RMT_NVS_ENABLED                    1
#define CONFIG_WIFI_RMT_PASSIVE_HIDDEN_AP_SUPPORT      0
#define CONFIG_WIFI_RMT_RX_BA_WIN                      6
#define CONFIG_WIFI_RMT_RX_MGMT_BUF_NUM_DEF            4
#define CONFIG_WIFI_RMT_SLP_SAMPLE_BEACON_FEATURE      0
#define CONFIG_WIFI_RMT_SOFTAP_BEACON_MAX_LEN          752
#define CONFIG_WIFI_RMT_STA_DISCONNECTED_PM_ENABLE     0
#define CONFIG_WIFI_RMT_STATIC_RX_BUFFER_NUM           10
#define CONFIG_WIFI_RMT_STATIC_TX_BUFFER_NUM           16
#define CONFIG_WIFI_RMT_TASK_PINNED_TO_CORE_1          0
#define CONFIG_WIFI_RMT_TX_BUFFER_TYPE                 1
#define CONFIG_WIFI_RMT_TX_HETB_QUEUE_NUM              4
