/****************************************************************************
 * esp_hosted_official/port/os/nuttx/include/esp_wifi_he_types.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi 6 (HE) types, as much of them as the host stack's headers pull in.
 *
 * The companion owns the radio and the 802.11 details travel as opaque RPC
 * payloads, so nothing here needs a HE structure: the file exists because the
 * stack's Wi-Fi headers include it, and an empty definition is what "the host
 * does not care" looks like.
 */

#ifndef __ESP_WIFI_HE_TYPES_H_SHIM__
#define __ESP_WIFI_HE_TYPES_H_SHIM__

#include <stdint.h>

#endif /* __ESP_WIFI_HE_TYPES_H_SHIM__ */
