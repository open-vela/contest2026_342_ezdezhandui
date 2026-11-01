/****************************************************************************
 * esp_hosted_official/port/os/nuttx/include/freertos/FreeRTOS.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A self-contained FreeRTOS facade for the official esp-hosted host stack on
 * NuttX.
 *
 * Why not reuse arch/risc-v/src/common/espressif/freertos_compat/: that shim
 * is built on platform/os.h, whose implementation lives in the MCUboot subtree
 * that no git repository tracks (docs appendix AT/AI).  A port that depends on
 * it cannot be rebuilt from the deliverable, so this one uses NuttX primitives
 * directly and is found first on the include path (target_include_directories
 * BEFORE, in port/os/nuttx/CMakeLists.txt).
 *
 * Units: this shim treats a FreeRTOS tick as one millisecond, which is what
 * pdMS_TO_TICKS() below assumes and what the existing compat layer does too.
 * The stack only uses ticks for timeouts, so the choice does not change logic.
 *
 ****************************************************************************/

#ifndef __EH_HOSTED_NUTTX_FREERTOS_H
#define __EH_HOSTED_NUTTX_FREERTOS_H

#include <nuttx/config.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t TickType_t;
typedef int      BaseType_t;
typedef unsigned UBaseType_t;

typedef void *TaskHandle_t;

#define pdTRUE                          ((BaseType_t)1)
#define pdFALSE                         ((BaseType_t)0)
#define pdPASS                          pdTRUE
#define pdFAIL                          pdFALSE

#define portMAX_DELAY                   ((TickType_t)0xffffffffu)
#define portTICK_PERIOD_MS              1
#define pdMS_TO_TICKS(ms)               ((TickType_t)(ms))

/* The stack creates one queue item per esp-hosted frame; a small fixed depth is
 * what it asks for, so nothing here needs dynamic growth.
 */

#endif /* __EH_HOSTED_NUTTX_FREERTOS_H */
