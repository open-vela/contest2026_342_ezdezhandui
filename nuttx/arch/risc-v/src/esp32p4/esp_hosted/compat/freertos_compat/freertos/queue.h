/****************************************************************************
 * arch/risc-v/src/common/espressif/freertos_compat/freertos/queue.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_COMMON_ESPRESSIF_FREERTOS_COMPAT_FREERTOS_QUEUE_H
#define __ARCH_RISCV_SRC_COMMON_ESPRESSIF_FREERTOS_COMPAT_FREERTOS_QUEUE_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "platform/os.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* The NuttX ESP-IDF shim implements queues on top of NuttX message queues;
 * esp_os_queue_handle_t is an opaque handle to one of those.  As in
 * FreeRTOS, items are copied into and out of the queue by value.
 */

typedef esp_os_queue_handle_t QueueHandle_t;
typedef esp_os_queue_handle_t SemaphoreHandle_t;

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
extern "C"
{
#endif

/****************************************************************************
 * Name: xQueueSend / xQueueReceive / xQueueReceiveFromISR
 *
 * Description:
 *   Minimal FreeRTOS queue API implemented with the ESP-IDF/NuttX shim
 *   primitives in platform/os.h.  Timeouts are expressed in NuttX ticks,
 *   matching what portMAX_DELAY and friend expect.
 *
 ****************************************************************************/

#define xQueueSend(queue, item, ticks_to_wait) \
  ((BaseType_t)(esp_os_queue_send((queue), (void *)(item), \
                                  (uint32_t)(ticks_to_wait)) == ESP_OK))

#define xQueueSendFromISR(queue, item, hptw) \
  ((BaseType_t)(esp_os_queue_send_from_isr((queue), (void *)(item), \
                                           (void *)(hptw)) == ESP_OK))

#define xQueueReceive(queue, item, ticks_to_wait) \
  ((BaseType_t)(esp_os_queue_receive((queue), (void *)(item), \
                                     (uint32_t)(ticks_to_wait)) == ESP_OK))

#define xQueueReceiveFromISR(queue, item, hptw) \
  ((BaseType_t)(esp_os_queue_receive_from_isr((queue), (void *)(item), \
                                              (void *)(hptw)) == ESP_OK))

#define xQueueCreate(items, item_size) \
  esp_os_queue_create_with_caps((items), (item_size), 0)

#define vQueueDelete(queue) esp_os_queue_delete((queue))

/* FreeRTOS returns the number of messages waiting; the NuttX shim does not
 * expose that, so report "not empty" conservatively.
 */

#define uxQueueMessagesWaiting(queue) ((UBaseType_t)1)

#define xQueueReset(queue) esp_os_queue_delete_with_caps(queue)

#ifdef __cplusplus
}
#endif

#endif /* __ARCH_RISCV_SRC_COMMON_ESPRESSIF_FREERTOS_COMPAT_FREERTOS_QUEUE_H */
