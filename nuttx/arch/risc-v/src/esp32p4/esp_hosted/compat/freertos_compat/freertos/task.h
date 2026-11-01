/****************************************************************************
 * arch/risc-v/src/common/espressif/freertos_compat/freertos/task.h
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

#ifndef __ARCH_RISCV_SRC_COMMON_ESPRESSIF_FREERTOS_COMPAT_FREERTOS_TASK_H
#define __ARCH_RISCV_SRC_COMMON_ESPRESSIF_FREERTOS_COMPAT_FREERTOS_TASK_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include "platform/os.h"
#include "freertos/FreeRTOS.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Task API subset.  Every operation is routed through the ESP-IDF/NuttX shim
 * in platform/os.h so that behaviour stays identical to the other vendored
 * ESP-IDF drivers already built by this port.
 */

#define vTaskDelay(ticks)                esp_os_task_delay_adapter(ticks)

#define vTaskDelayUntil(prev, ticks)     esp_os_task_delay_adapter(ticks)

#define xTaskGetTickCount()              esp_os_task_get_tick_count()

#define xTaskGetCurrentTaskHandle()      esp_os_task_get_current_handle()

#define vTaskDelete(task)                esp_os_task_delete(task)

#define taskYIELD()                      sched_yield()

#define vTaskSuspendAll()                esp_os_scheduler_disable()

#define xTaskResumeAll()                 esp_os_scheduler_enable()

#define xTaskCreate(func, name, stack, arg, prio, handle) \
  esp_os_create_task_pinned_to_core((func), (name), (stack), (arg), (prio), \
                                    (handle), tskNO_AFFINITY)

#endif /* __ARCH_RISCV_SRC_COMMON_ESPRESSIF_FREERTOS_COMPAT_FREERTOS_TASK_H */
