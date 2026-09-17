/****************************************************************************
 * arch/risc-v/src/common/espressif/freertos_compat/freertos/FreeRTOS.h
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

#ifndef __ARCH_RISCV_SRC_COMMON_ESPRESSIF_FREERTOS_COMPAT_FREERTOS_FREERTOS_H
#define __ARCH_RISCV_SRC_COMMON_ESPRESSIF_FREERTOS_COMPAT_FREERTOS_FREERTOS_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"
#include "esp_attr.h"

/* The NuttX port of the ESP-IDF OS abstraction layer.  It already provides
 * the FreeRTOS compatible scalar types (BaseType_t, UBaseType_t, TickType_t),
 * pdPASS/portMAX_DELAY, portNUM_PROCESSORS, portMUX_INITIALIZER_UNLOCKED and
 * the esp_os_queue_*() primitives used by this shim.
 */

#include "platform/os.h"

/* esp_os_enter_critical()/esp_os_exit_critical() and their ISR/safe variants
 * are the ESP-IDF critical section macros; they expand to NuttX critical
 * sections (and to a real spinlock on SMP builds).
 */

#include "esp_private/critical_section.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Boolean / status values.  FreeRTOS uses these interchangeably, so keep the
 * numeric values identical (pdTRUE == 1, pdFALSE == 0).
 */

#define pdTRUE                 (1)
#define pdFALSE                (0)
#define pdFAIL                 (0)

#undef pdPASS
#define pdPASS                 (1)

#define errQUEUE_EMPTY         (0)
#define errQUEUE_FULL          (0)

/* The vendored ESP-IDF sources call MAX()/MIN() without including the header
 * that normally provides them (sys/param.h).  Provide the standard
 * definitions only if nothing else already did.
 */

#ifndef MAX
#  define MAX(a, b)            (((a) > (b)) ? (a) : (b))
#endif

#ifndef MIN
#  define MIN(a, b)            (((a) < (b)) ? (a) : (b))
#endif

/* FreeRTOS expresses timeouts in ticks; the ESP-IDF code compiled here
 * divides a millisecond timeout by portTICK_PERIOD_MS to obtain ticks.
 * CONFIG_USEC_PER_TICK is a NuttX tick in microseconds, so its millisecond
 * equivalent is CONFIG_USEC_PER_TICK / 1000.  Never allow the divisor to
 * evaluate to zero (a sub-millisecond tick would otherwise be a division by
 * zero at runtime); fall back to 1 in that case.
 */

#if (CONFIG_USEC_PER_TICK / 1000) > 0
#  define portTICK_PERIOD_MS   (CONFIG_USEC_PER_TICK / 1000)
#else
#  define portTICK_PERIOD_MS   (1)
#endif

/* FreeRTOS critical sections.
 *
 * On an SMP build the ESP-IDF lock type is NuttX's rspinlock_t and the
 * critical section really takes the lock.  On a single core build
 * (CONFIG_FREERTOS_UNICORE, i.e. !CONFIG_SMP) the lock is unnecessary and
 * esp_os_enter_critical() simply disables interrupts - but the object still
 * has to exist and still has to be initializable, so the same rspinlock_t
 * type is used for both cases.  That also keeps
 * portMUX_INITIALIZE(&lock) -> esp_os_spinlock_initialize(&lock) type safe.
 */

typedef OS_SPINLOCK_TYPE portMUX_TYPE;

#define portMUX_INITIALIZE(lock)         esp_os_spinlock_initialize(lock)
#define portMUX_INITIALIZE_ISR(lock)     esp_os_spinlock_initialize(lock)

#define portENTER_CRITICAL(lock)         esp_os_enter_critical(lock)
#define portEXIT_CRITICAL(lock)          esp_os_exit_critical(lock)
#define portENTER_CRITICAL_ISR(lock)     esp_os_enter_critical_isr(lock)
#define portEXIT_CRITICAL_ISR(lock)      esp_os_exit_critical_isr(lock)
#define portENTER_CRITICAL_SAFE(lock)    esp_os_enter_critical_safe(lock)
#define portEXIT_CRITICAL_SAFE(lock)     esp_os_exit_critical_safe(lock)

/* Yielding from an ISR is a no-op in this port: NuttX ISRs run outside of
 * the scheduler and the ESP-IDF shim deliberately does not switch tasks from
 * interrupt context.
 */

#define portYIELD_FROM_ISR(...)  OS_PORT_YIELD_FROM_ISR()

/* FreeRTOS task priority helpers used by some ESP-IDF sources. */

#define tskIDLE_PRIORITY       (0)

/****************************************************************************
 * Public Types
 ****************************************************************************/

typedef void *TaskHandle_t;

/****************************************************************************
 * Inline Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp_freertos_yield_from_isr
 *
 * Description:
 *   Update the "task switched from ISR" flag.  Kept as a helper so that the
 *   queue wrappers can honour the FreeRTOS xQueueReceiveFromISR() contract
 *   even though this port never requests a context switch.
 *
 ****************************************************************************/

static inline void
esp_freertos_note_higher_priority_woken(FAR BaseType_t *higher_priority_task_woken)
{
  if (higher_priority_task_woken != NULL)
    {
      *higher_priority_task_woken = pdFALSE;
    }
}

/* The FreeRTOS queue registry here is backed by the ESP-IDF/NuttX shim in
 * platform/os.h; include the queue wrappers so that a translation unit which
 * only includes freertos/FreeRTOS.h still compiles.
 */

#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#endif /* __ARCH_RISCV_SRC_COMMON_ESPRESSIF_FREERTOS_COMPAT_FREERTOS_FREERTOS_H */
