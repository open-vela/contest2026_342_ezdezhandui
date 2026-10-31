/****************************************************************************
 * esp_hosted_official/port/os/nuttx/src/esp_event_nuttx.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Default event loop for the NuttX port: a fixed table of handlers, delivery in
 * the poster's context.
 *
 * The official stack uses the event loop for a handful of bring-up events and
 * does not require queueing or ordering beyond "the handler sees the event", so
 * this stays a table rather than a thread and a queue.
 */

#include <nuttx/config.h>

#include <string.h>
#include <syslog.h>

#include "esp_event.h"

#ifdef CONFIG_ESPRESSIF_HOSTED_OFFICIAL

#define ESP_EVENT_MAX_HANDLERS 16

struct esp_event_loop_s
{
  int                created;
  int                count;
  struct
  {
    esp_event_base_t   base;
    int32_t            id;
    esp_event_handler_t handler;
    void              *arg;
  } handlers[ESP_EVENT_MAX_HANDLERS];
};

static struct esp_event_loop_s g_event_loop;

esp_err_t esp_event_loop_create_default(void)
{
  memset(&g_event_loop, 0, sizeof(g_event_loop));
  g_event_loop.created = 1;
  return ESP_OK;
}

esp_err_t esp_event_handler_register(esp_event_base_t base, int32_t id,
                                     esp_event_handler_t handler, void *arg)
{
  if (!g_event_loop.created || handler == NULL ||
      g_event_loop.count >= ESP_EVENT_MAX_HANDLERS)
    {
      return ESP_ERR_INVALID_STATE;
    }

  g_event_loop.handlers[g_event_loop.count].base    = base;
  g_event_loop.handlers[g_event_loop.count].id      = id;
  g_event_loop.handlers[g_event_loop.count].handler = handler;
  g_event_loop.handlers[g_event_loop.count].arg     = arg;
  g_event_loop.count++;

  return ESP_OK;
}

esp_err_t esp_event_handler_unregister(esp_event_base_t base, int32_t id,
                                       esp_event_handler_t handler)
{
  int i;

  for (i = 0; i < g_event_loop.count; i++)
    {
      if (g_event_loop.handlers[i].base == base &&
          g_event_loop.handlers[i].id == id &&
          g_event_loop.handlers[i].handler == handler)
        {
          g_event_loop.handlers[i] = g_event_loop.handlers[g_event_loop.count - 1];
          g_event_loop.count--;
          return ESP_OK;
        }
    }

  return ESP_ERR_NOT_FOUND;
}

esp_err_t esp_event_post(esp_event_base_t base, int32_t id,
                         const void *data, size_t size, uint32_t ticks)
{
  int i;

  (void)size;
  (void)ticks;

  for (i = 0; i < g_event_loop.count; i++)
    {
      if (g_event_loop.handlers[i].base == base &&
          g_event_loop.handlers[i].id == id)
        {
          g_event_loop.handlers[i].handler(g_event_loop.handlers[i].arg,
                                           base, id, (void *)data);
        }
    }

  return ESP_OK;
}

#endif /* CONFIG_ESPRESSIF_HOSTED_OFFICIAL */
