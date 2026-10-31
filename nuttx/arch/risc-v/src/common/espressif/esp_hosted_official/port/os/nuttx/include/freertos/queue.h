/****************************************************************************
 * esp_hosted_official/port/os/nuttx/include/freertos/queue.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * FreeRTOS queue API on a NuttX message queue.
 *
 * The stack sends fixed-size items by value, which is exactly what a NuttX
 * message queue does (mqd_t), so the shim is thin: each handle carries the
 * descriptor plus the item size.
 *
 ****************************************************************************/

#ifndef __EH_HOSTED_NUTTX_QUEUE_H
#define __EH_HOSTED_NUTTX_QUEUE_H

#include <nuttx/config.h>

#include <mqueue.h>
#include <fcntl.h>
#include <stdio.h>

#include <nuttx/kmalloc.h>

#include "freertos/FreeRTOS.h"

struct eh_freertos_queue_s
{
  mqd_t   mq;
  size_t  item_size;
};

typedef struct eh_freertos_queue_s *QueueHandle_t;

static inline QueueHandle_t xQueueCreate(UBaseType_t depth, UBaseType_t size)
{
  struct mallinfo info;
  struct mq_attr  attr;
  char            name[24];
  int             n;
  QueueHandle_t   q;

  info = mallinfo();
  n = snprintf(name, sizeof(name), "ehq%d", (int)info.uordblks);
  if (n < 0 || n >= (int)sizeof(name))
    {
      return NULL;
    }

  q = (QueueHandle_t)kmm_zalloc(sizeof(*q));
  if (q == NULL)
    {
      return NULL;
    }

  attr.mq_maxmsg  = depth;
  attr.mq_msgsize = size;
  attr.mq_flags   = 0;

  q->mq = mq_open(name, O_RDWR | O_CREAT, 0666, &attr);
  if (q->mq < 0)
    {
      kmm_free(q);
      return NULL;
    }

  q->item_size = size;
  return q;
}

static inline void vQueueDelete(QueueHandle_t q)
{
  if (q != NULL)
    {
      mq_close(q->mq);
      kmm_free(q);
    }
}

static inline BaseType_t xQueueSend(QueueHandle_t q, const void *item,
                                    TickType_t ticks)
{
  struct timespec ts;

  if (q == NULL)
    {
      return pdFALSE;
    }

  if (ticks == portMAX_DELAY)
    {
      return (mq_send(q->mq, item, q->item_size, 0) == 0) ? pdTRUE : pdFALSE;
    }

  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec  += ticks / 1000;
  ts.tv_nsec += (ticks % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L)
    {
      ts.tv_sec  += 1;
      ts.tv_nsec -= 1000000000L;
    }

  return (mq_timedsend(q->mq, item, q->item_size, 0, &ts) == 0) ?
         pdTRUE : pdFALSE;
}

static inline BaseType_t xQueueReceive(QueueHandle_t q, void *item,
                                       TickType_t ticks)
{
  struct timespec ts;

  if (q == NULL)
    {
      return pdFALSE;
    }

  if (ticks == portMAX_DELAY)
    {
      return (mq_receive(q->mq, item, q->item_size, NULL) >= 0) ?
             pdTRUE : pdFALSE;
    }

  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec  += ticks / 1000;
  ts.tv_nsec += (ticks % 1000) * 1000000L;
  if (ts.tv_nsec >= 1000000000L)
    {
      ts.tv_sec  += 1;
      ts.tv_nsec -= 1000000000L;
    }

  return (mq_timedreceive(q->mq, item, q->item_size, NULL, &ts) >= 0) ?
         pdTRUE : pdFALSE;
}

#endif /* __EH_HOSTED_NUTTX_QUEUE_H */
