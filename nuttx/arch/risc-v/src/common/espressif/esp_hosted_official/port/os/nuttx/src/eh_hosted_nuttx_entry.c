/****************************************************************************
 * esp_hosted_official/port/os/nuttx/src/eh_hosted_nuttx_entry.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Start the official esp-hosted host stack on NuttX.
 *
 * The stack's init brings the transport up through the port layer and then
 * creates its own receive/transmit tasks, so it must not run on the boot path
 * before the SDIO link exists.  The board brings the SDIO link up first
 * (esp_hosted_sdio_initialize()) and calls this immediately after -- by then
 * the card is enumerated and function 1 is enabled, which is exactly what the
 * port's card_init() expects to find.
 *
 * A dedicated kernel thread runs the sequence because eh_host_init() blocks
 * while the companion answers, and the boot must not wait for that.
 */

#include <nuttx/config.h>

#include <sched.h>
#include <syslog.h>

#include <nuttx/kthread.h>

#include "eh_host_core.h"
#include "eh_host_transport_config.h"

#include "eh_hosted_nuttx.h"

#ifdef CONFIG_ESPRESSIF_HOSTED_OFFICIAL

#define ENTRY_TAG "eh_hosted_nuttx"

/* Defaults so this file works in a tree whose Kconfig predates the options
 * (same reasoning as esp_hosted_sdio.c's EN_GPIO fallbacks).
 */

#ifndef CONFIG_ESPRESSIF_HOSTED_OFFICIAL_PRIO
#  define CONFIG_ESPRESSIF_HOSTED_OFFICIAL_PRIO 100
#endif

#ifndef CONFIG_ESPRESSIF_HOSTED_OFFICIAL_STACKSIZE
#  define CONFIG_ESPRESSIF_HOSTED_OFFICIAL_STACKSIZE 4096
#endif

static int eh_hosted_nuttx_thread(int argc, char *argv[])
{
  int ret;

  syslog(LOG_INFO, "%s: starting official esp-hosted host stack\n", ENTRY_TAG);

  /* Resolve the transport configuration before anything reads it.
   *
   * The transport reads its queue sizes (and the rest of the SDIO wiring) out of
   * the config module; nothing else calls the setter, and without this the
   * struct is all zeros -- measured on hardware: the stack logged "provided sdio
   * tx queue size is zero! Setting to 2" and fell back to a two-deep queue,
   * which is a fraction of the port's configured 20.
   */

  ret = (int)eh_host_transport_set_default_config();
  if (ret != 0)
    {
      syslog(LOG_WARNING, "%s: default transport config failed: %d\n",
             ENTRY_TAG, ret);
    }

  ret = eh_host_init(NULL);
  if (ret != 0)
    {
      syslog(LOG_ERR, "%s: eh_host_init failed: %d\n", ENTRY_TAG, ret);
      return ret;
    }

  /* Caps handshake with the companion.  This is the first thing that has to
   * work before any RPC can be issued (goal stage 3).
   */

  ret = eh_host_connect_to_slave();
  if (ret != 0)
    {
      syslog(LOG_ERR, "%s: connect to slave failed: %d\n", ENTRY_TAG, ret);
      return ret;
    }

  syslog(LOG_INFO, "%s: companion connected, transport ready\n", ENTRY_TAG);

  /* wlan0 comes up once there is something to carry: the channel is registered
   * with the stack here, and the network stack finds the interface when it
   * configures it.  Goal stage 4 (scan/connect/DHCP) runs on top of it.
   */

  ret = esp_hosted_netdev_register();
  if (ret < 0)
    {
      syslog(LOG_ERR, "%s: wlan0 registration failed: %d\n", ENTRY_TAG, ret);
      return ret;
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp_hosted_official_start
 *
 * Description:
 *   Create the thread that runs the official stack.  Call it after the SDIO
 *   link has been brought up by esp_hosted_sdio_initialize().
 *
 ****************************************************************************/

int esp_hosted_official_start(void)
{
  int pid = kthread_create("eh_hosted", CONFIG_ESPRESSIF_HOSTED_OFFICIAL_PRIO,
                           CONFIG_ESPRESSIF_HOSTED_OFFICIAL_STACKSIZE,
                           eh_hosted_nuttx_thread, NULL);

  if (pid < 0)
    {
      syslog(LOG_ERR, "%s: cannot create the stack thread: %d\n",
             ENTRY_TAG, pid);
      return pid;
    }

  return OK;
}

#endif /* CONFIG_ESPRESSIF_HOSTED_OFFICIAL */
