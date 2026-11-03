/****************************************************************************
 * arch/risc-v/src/esp32p4/esp_hosted/compat/ppa_compat.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * PPA HAL 的编译期兼容注入（由 hal_esp32p4.cmake 用 -include 强制包含）。
 *
 * upper_hal_ppa 依赖 ESP-IDF 的 __containerof()（从成员指针反推容器指针），
 * 而我们的树里只有 esp_eth 自带一份。这里统一补上，避免改动 vendored 源码。
 *
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_ESP32P4_ESP_HOSTED_COMPAT_PPA_COMPAT_H
#define __ARCH_RISCV_SRC_ESP32P4_ESP_HOSTED_COMPAT_PPA_COMPAT_H

#include <stddef.h>

#ifndef __containerof
#  define __containerof(ptr, type, member) \
     ((type *)((char *)(ptr) - offsetof(type, member)))
#endif

#endif /* __ARCH_RISCV_SRC_ESP32P4_ESP_HOSTED_COMPAT_PPA_COMPAT_H */
