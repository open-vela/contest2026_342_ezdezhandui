/* SPDX-License-Identifier: Apache-2.0 */

/* ESP-IDF argument-checking macros, for the NuttX port of esp-hosted.
 *
 * Only the handful the vendored sources use are provided; each one returns the
 * ESP-IDF error code the real macro returns, so the stack's error paths do not
 * need to change.
 */

#ifndef __ESP_CHECK_H_SHIM__
#define __ESP_CHECK_H_SHIM__

#include "esp_err.h"

#define ESP_RETURN_ON_FALSE(a, err_code, tag, fmt, ...)                  \
    do {                                                                 \
        if (!(a)) {                                                      \
            return err_code;                                             \
        }                                                                \
    } while (0)

#define ESP_RETURN_ON_ERROR(x, tag, fmt, ...)                            \
    do {                                                                 \
        esp_err_t rc_ = (x);                                             \
        if (rc_ != ESP_OK) {                                             \
            return rc_;                                                  \
        }                                                                \
    } while (0)

#define ESP_GOTO_ON_FALSE(a, err_code, goto_tag, tag, fmt, ...)          \
    do {                                                                 \
        if (!(a)) {                                                      \
            ret = err_code;                                              \
            goto goto_tag;                                               \
        }                                                                \
    } while (0)

#define ESP_GOTO_ON_ERROR(x, goto_tag, tag, fmt, ...)                    \
    do {                                                                 \
        esp_err_t rc_ = (x);                                             \
        if (rc_ != ESP_OK) {                                             \
            ret = rc_;                                                   \
            goto goto_tag;                                               \
        }                                                                \
    } while (0)

#endif /* __ESP_CHECK_H_SHIM__ */
