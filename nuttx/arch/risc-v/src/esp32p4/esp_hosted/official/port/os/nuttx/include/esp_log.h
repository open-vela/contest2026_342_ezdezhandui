/* SPDX-License-Identifier: Apache-2.0 */

/* ESP-IDF logging macros, for the NuttX port of esp-hosted.
 *
 * Mapped onto syslog with the ESP-IDF tag as part of the message so the
 * stack's own diagnostics stay greppable next to the driver's.
 */

#ifndef __ESP_LOG_H_SHIM__
#define __ESP_LOG_H_SHIM__

#include <syslog.h>

/* Compile-time log level, and the level constants the sources compare against.
 */

#define ESP_LOG_NONE                    0
#define ESP_LOG_ERROR                   1
#define ESP_LOG_WARN                    2
#define ESP_LOG_INFO                    3
#define ESP_LOG_DEBUG                   4
#define ESP_LOG_VERBOSE                 5

#ifndef LOG_LOCAL_LEVEL
#  define LOG_LOCAL_LEVEL               ESP_LOG_INFO
#endif

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

/* Hex dumps are only used by the streaming data path's debug trace.
 *
 * The official stack calls this when its log level is verbose; the call is
 * compiled in unconditionally, so the symbol has to exist here or the streaming
 * build fails to link (measured: "undefined reference to
 * ESP_LOG_BUFFER_HEX_LEVEL" from sdio_data_to_rx_buf_task, appendix BY.12).
 * Emitting the bytes as one line keeps the information without a second
 * formatting helper.
 */

static inline void esp_log_buffer_hex_level(const char *tag, const void *buffer,
                                            unsigned length, int level)
{
  const unsigned char *p = (const unsigned char *)buffer;
  unsigned i;

  syslog(LOG_INFO, "%s: hex (%u bytes):", tag, length);

  for (i = 0; i < length && i < 64; i++)
    {
      syslog(LOG_INFO, " %02x", p[i]);
    }

  syslog(LOG_INFO, "\n");
}

#define ESP_LOG_BUFFER_HEX_LEVEL(tag, buffer, length, level) \
    esp_log_buffer_hex_level(tag, buffer, length, level)
#define ESP_LOG_BUFFER_HEX(tag, buffer, length) \
    esp_log_buffer_hex_level(tag, buffer, length, 4)
#define ESP_LOG_BUFFER_HEXDUMP(tag, buffer, length, level) \
    esp_log_buffer_hex_level(tag, buffer, length, level)

#endif /* __ESP_LOG_H_SHIM__ */
