/****************************************************************************
 * arch/risc-v/src/esp32p4/esp_hosted/compat/freertos_compat/freertos/portmacro.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * FreeRTOS 兼容层的 portmacro.h。
 *
 * ESP-IDF 的 HAL 组件（如 upper_hal_ppa）会直接包含 <freertos/portmacro.h>
 * 来拿 portMUX_TYPE / portENTER_CRITICAL* 等临界区原语。这些原语我们的
 * FreeRTOS.h 已经映射到 esp_os_*（见该文件的注释），所以这里只需转包含。
 *
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_ESP32P4_ESP_HOSTED_COMPAT_FREERTOS_PORTMACRO_H
#define __ARCH_RISCV_SRC_ESP32P4_ESP_HOSTED_COMPAT_FREERTOS_PORTMACRO_H

#include <stddef.h>

#include "freertos/FreeRTOS.h"

/* ESP-IDF 的常见宏：从"成员指针"反推"容器指针"。
 * PPA 等 HAL 组件直接用 __containerof()，而我们的树里只有 esp_eth 自带一份，
 * 这里补上（带守卫，重复包含也安全）。
 */

#ifndef __containerof
#  define __containerof(ptr, type, member) \
     ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

#endif /* __ARCH_RISCV_SRC_ESP32P4_ESP_HOSTED_COMPAT_FREERTOS_PORTMACRO_H */
