/****************************************************************************
 * esp_hosted_official/port/os/nuttx/include/freertos/semphr.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * FreeRTOS semaphore/mutex API on NuttX primitives (nxsem / nxmutex).
 *
 * A handle is a small heap object holding the NuttX primitive, because
 * FreeRTOS hands out opaque pointers and the stack does not care what is
 * behind them.
 *
 ****************************************************************************/

#ifndef __EH_HOSTED_NUTTX_SEMPHR_H
#define __EH_HOSTED_NUTTX_SEMPHR_H

#include <nuttx/config.h>

#include <nuttx/kmalloc.h>
#include <nuttx/mutex.h>
#include <nuttx/semaphore.h>

#include "freertos/FreeRTOS.h"

struct eh_freertos_sem_s
{
  bool    is_mutex;
  union
  {
    sem_t   sem;
    mutex_t mutex;
  } u;
};

typedef struct eh_freertos_sem_s *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
  struct eh_freertos_sem_s *s =
    (struct eh_freertos_sem_s *)kmm_zalloc(sizeof(*s));

  if (s == NULL)
    {
      return NULL;
    }

  s->is_mutex = false;

  if (nxsem_init(&s->u.sem, 0, 0) < 0)
    {
      kmm_free(s);
      return NULL;
    }

  return s;
}

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
  struct eh_freertos_sem_s *s =
    (struct eh_freertos_sem_s *)kmm_zalloc(sizeof(*s));

  if (s == NULL)
    {
      return NULL;
    }

  s->is_mutex = true;

  if (nxmutex_init(&s->u.mutex) < 0)
    {
      kmm_free(s);
      return NULL;
    }

  return s;
}

static inline void vSemaphoreDelete(SemaphoreHandle_t s)
{
  if (s == NULL)
    {
      return;
    }

  if (s->is_mutex)
    {
      nxmutex_destroy(&s->u.mutex);
    }
  else
    {
      nxsem_destroy(&s->u.sem);
    }

  kmm_free(s);
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks)
{
  int ret;
  int timeout = (ticks == portMAX_DELAY) ? -1 : (int)ticks;

  if (s == NULL)
    {
      return pdFALSE;
    }

  if (s->is_mutex)
    {
      ret = (timeout < 0) ? nxmutex_lock(&s->u.mutex)
                          : nxmutex_timedlock(&s->u.mutex, timeout);
    }
  else
    {
      ret = nxsem_tickwait(&s->u.sem, timeout < 0 ? UINT_MAX : MSEC2TICK(timeout));
    }

  return (ret >= 0) ? pdTRUE : pdFALSE;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
  int ret;

  if (s == NULL)
    {
      return pdFALSE;
    }

  ret = s->is_mutex ? nxmutex_unlock(&s->u.mutex) : nxsem_post(&s->u.sem);
  return (ret >= 0) ? pdTRUE : pdFALSE;
}

static inline BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s,
                                              BaseType_t *woken)
{
  if (s != NULL && !s->is_mutex && nxsem_post(&s->u.sem) >= 0)
    {
      return pdTRUE;
    }

  return pdFALSE;
}

#endif /* __EH_HOSTED_NUTTX_SEMPHR_H */
