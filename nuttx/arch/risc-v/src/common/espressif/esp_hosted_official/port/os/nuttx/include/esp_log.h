/* SPDX-License-Identifier: Apache-2.0 */

/* ESP-IDF logging macros, for the NuttX port of esp-hosted.
 *
 * Mapped onto syslog with the ESP-IDF tag as part of the message so the
 * stack's own diagnostics stay greppable next to the driver's.
 */

#ifndef __ESP_LOG_H_SHIM__
#define __ESP_LOG_H_SHIM__

#include <syslog.h>

#define ESP_LOGE(tag, fmt, ...)  syslog(LOG_ERR,  "%s: " fmt, tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...)  syslog(LOG_WARNING, "%s: " fmt, tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...)  syslog(LOG_INFO, "%s: " fmt, tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...)  syslog(LOG_DEBUG, "%s: " fmt, tag, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...)  syslog(LOG_DEBUG, "%s: " fmt, tag, ##__VA_ARGS__)

#define ESP_EARLY_LOGE(tag, fmt, ...)  ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#define ESP_EARLY_LOGW(tag, fmt, ...)  ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define ESP_EARLY_LOGI(tag, fmt, ...)  ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define ESP_EARLY_LOGD(tag, fmt, ...)  ESP_LOGD(tag, fmt, ##__VA_ARGS__)
#define ESP_EARLY_LOGV(tag, fmt, ...)  ESP_LOGV(tag, fmt, ##__VA_ARGS__)

#define ESP_LOG_LEVEL(level, tag, fmt, ...)                              \
    syslog(LOG_INFO, "%s: " fmt, tag, ##__VA_ARGS__)

#endif /* __ESP_LOG_H_SHIM__ */
