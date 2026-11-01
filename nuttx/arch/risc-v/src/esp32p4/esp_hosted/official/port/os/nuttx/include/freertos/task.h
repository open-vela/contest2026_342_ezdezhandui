/****************************************************************************
 * esp_hosted_official/port/os/nuttx/include/freertos/task.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * FreeRTOS task calls on NuttX threads.
 *
 * xTaskCreateWithCaps()/vTaskDeleteWithCaps() differ from their plain forms
 * only in which allocator the stack would like to have used; on NuttX there is
 * one heap, so they collapse onto the same calls.
 *
 ****************************************************************************/

#ifndef __EH_HOSTED_NUTTX_TASK_H
#define __EH_HOSTED_NUTTX_TASK_H

#include <nuttx/config.h>

#include <sched.h>
#include <nuttx/clock.h>
#include <nuttx/kthread.h>
#include <nuttx/signal.h>

#include "freertos/FreeRTOS.h"

#define vTaskDelay(ticks)         nxsig_usleep((useconds_t)(ticks) * 1000)
#define vTaskDelayUntil(p, ticks) vTaskDelay(ticks)

static inline TickType_t xTaskGetTickCount(void)
{
  return (TickType_t)clock_systime_ticks();
}

typedef void (*TaskFunction_t)(void *);

static inline BaseType_t xTaskCreate(TaskFunction_t fn, const char *name,
                                    uint32_t stack, void *arg,
                                    UBaseType_t prio, TaskHandle_t *handle)
{
  int pid = kthread_create(name, prio, stack, fn, (FAR void *)arg);

  if (pid < 0)
    {
      return pdFAIL;
    }

  if (handle != NULL)
    {
      *handle = (TaskHandle_t)(uintptr_t)pid;
    }

  return pdPASS;
}

#define xTaskCreateWithCaps(fn, name, stack, arg, prio, handle, caps) \
  xTaskCreate(fn, name, stack, arg, prio, handle)

static inline void vTaskDelete(TaskHandle_t handle)
{
  if (handle != NULL)
    {
      nxsched_cancel((pid_t)(uintptr_t)handle);
    }
}

#define vTaskDeleteWithCaps(handle) vTaskDelete(handle)

#endif /* __EH_HOSTED_NUTTX_TASK_H */
