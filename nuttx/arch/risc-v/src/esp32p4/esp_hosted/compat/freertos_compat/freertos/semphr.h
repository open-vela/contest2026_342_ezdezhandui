/****************************************************************************
 * arch/risc-v/src/esp32p4/esp_hosted/compat/freertos_compat/freertos/semphr.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * FreeRTOS 兼容层的 semphr.h（二值信号量）。
 *
 * ESP-IDF 的 HAL 组件（如 upper_hal_ppa）用 xSemaphoreCreateBinaryWithCaps /
 * xSemaphoreTake / xSemaphoreGive(FromISR) 等做互斥与"传输完成"通知。
 * 这里把它们映射到端口已有的 esp_os 队列：容量 1、元素是 uint32 的队列
 * 就是一个二值信号量（take 收走、give 放进）。
 *
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_ESP32P4_ESP_HOSTED_COMPAT_FREERTOS_SEMPHR_H
#define __ARCH_RISCV_SRC_ESP32P4_ESP_HOSTED_COMPAT_FREERTOS_SEMPHR_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/****************************************************************************
 * Type Definitions
 ****************************************************************************/

typedef esp_os_queue_handle_t SemaphoreHandle_t;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

static inline SemaphoreHandle_t xSemaphoreCreateBinaryWithCaps(uint32_t caps)
{
  return esp_os_queue_create_with_caps(1, sizeof(uint32_t), caps);
}

static inline SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
  return esp_os_queue_create_with_caps(1, sizeof(uint32_t), 0);
}

static inline void vSemaphoreDeleteWithCaps(SemaphoreHandle_t sem)
{
  if (sem != NULL)
    {
      esp_os_queue_delete_with_caps(sem);
    }
}

static inline void vSemaphoreDelete(SemaphoreHandle_t sem)
{
  if (sem != NULL)
    {
      esp_os_queue_delete(sem);
    }
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
  uint32_t value = 1;

  return (esp_os_queue_receive(sem, &value, (uint32_t)ticks) == 0) ?
          pdTRUE : pdFALSE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
  uint32_t value = 1;

  return (esp_os_queue_send(sem, &value, 0) == 0) ? pdTRUE : pdFALSE;
}

static inline BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t sem,
                                               BaseType_t *higher_priority_woken)
{
  uint32_t value = 1;

  return (esp_os_queue_send_from_isr(sem, &value,
                                     (FAR void *)higher_priority_woken) == 0) ?
          pdTRUE : pdFALSE;
}

#endif /* __ARCH_RISCV_SRC_ESP32P4_ESP_HOSTED_COMPAT_FREERTOS_SEMPHR_H */
