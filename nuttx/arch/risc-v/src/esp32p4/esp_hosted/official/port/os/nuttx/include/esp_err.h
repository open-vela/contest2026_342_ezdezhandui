/* SPDX-License-Identifier: Apache-2.0 */

/* ESP-IDF error codes, for the NuttX port of the official esp-hosted stack.
 *
 * The stack uses esp_err_t as "ESP_OK, or negative errno-like value" and only
 * these handful of codes appear in the vendored sources (measured: ESP_OK 80,
 * ESP_FAIL 37, ESP_ERR_INVALID_ARG 6, the rest <= 4 each).  Keeping the same
 * numeric values as ESP-IDF means error paths behave identically after the port.
 */

#ifndef __ESP_ERR_H_SHIM__
#define __ESP_ERR_H_SHIM__

#include <errno.h>
#include <stdint.h>

typedef int esp_err_t;

#define ESP_OK                          0
#define ESP_FAIL                        -1

#define ESP_ERR_NO_MEM                  0x101
#define ESP_ERR_INVALID_ARG             0x102
#define ESP_ERR_INVALID_STATE           0x103
#define ESP_ERR_INVALID_SIZE            0x104
#define ESP_ERR_NOT_FOUND               0x105
#define ESP_ERR_NOT_SUPPORTED           0x106
#define ESP_ERR_TIMEOUT                 0x107
#define ESP_ERR_INVALID_RESPONSE        0x108
#define ESP_ERR_INVALID_CRC             0x109
#define ESP_ERR_INVALID_VERSION         0x10A
#define ESP_ERR_INVALID_MAC             0x10B
#define ESP_ERR_NOT_FINISHED            0x10C
#define ESP_ERR_NOT_ALLOWED             0x10D

/* esp-hosted cites 0x107 as "SDIO unresponsive", i.e. the same code as
 * ESP_ERR_TIMEOUT, which is what the ESP-IDF error table has there.
 */

#define ESP_ERR_ESP_NETIF_BASE          0x5000
#define ESP_ERR_ESP_NETIF_NO_MEM        (ESP_ERR_ESP_NETIF_BASE + 0x1)
#define ESP_ERR_ESP_NETIF_TX_FAILED     (ESP_ERR_ESP_NETIF_BASE + 0x3)

#define ESP_ERROR_CHECK(x)              do {                              \
        esp_err_t rc_ = (x);                                              \
        if (rc_ != ESP_OK) { return rc_; }                                \
    } while (0)

#endif /* __ESP_ERR_H_SHIM__ */
