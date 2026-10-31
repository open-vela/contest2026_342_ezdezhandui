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
#include <nuttx/wireless/wireless.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <nuttx/signal.h>

#include "esp_gpio.h"
#include "esp_sdmmc.h"

#include "eh_host_core.h"
#include "eh_host_transport_config.h"
#include "eh_host_feat_wifi.h"
#include "esp_wifi.h"

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

  /* Who is holding DAT1 low?
   *
   * The wire shows DAT1 asserted continuously (appendix BP), which reads as "the
   * companion keeps its card interrupt raised" -- but the line is bidirectional,
   * so the host's own pad could be the one pulling it.  These three reads tell
   * them apart: input-only with pull-up (the card can still hold it low),
   * output driven high (whoever wins here is driving), then back to the normal
   * bidirectional configuration.
   */

  {
    int d1 = CONFIG_ESPRESSIF_SDMMC_D1;

    esp_configgpio(d1, INPUT | PULLUP);
    nxsig_usleep(2000);
    syslog(LOG_INFO, "%s: D1 test: as input+pullup -> %d\n",
           ENTRY_TAG, esp_gpioread(d1));

    esp_configgpio(d1, OUTPUT);
    esp_gpiowrite(d1, true);
    nxsig_usleep(2000);
    syslog(LOG_INFO, "%s: D1 test: driven high -> readback %d (1 = nothing else pulls it low)\n",
           ENTRY_TAG, esp_gpioread(d1));

    /* Toggle it slowly so the analyser can confirm both the pad control and the
     * channel mapping (ch3 is DAT1): low, high, low, one second each.
     */

    for (int i = 0; i < 3; i++)
      {
        esp_configgpio(d1, OUTPUT);
        esp_gpiowrite(d1, (i & 1) == 0 ? false : true);
        syslog(LOG_INFO, "%s: D1 toggle %d -> wrote %d, read %d\n",
               ENTRY_TAG, i, (i & 1) == 0 ? 0 : 1, esp_gpioread(d1));
        nxsig_usleep(1000000);
      }

    esp_configgpio(d1, INPUT | OUTPUT | PULLUP | DRIVE_3);
    nxsig_usleep(2000);
    syslog(LOG_INFO, "%s: D1 test: restored -> %d\n",
           ENTRY_TAG, esp_gpioread(d1));
  }

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

  /* Report what the transport will actually see.  The bus driver reads its
   * queue sizes from this structure, and on hardware it logged them as zero
   * (appendix BM.4); printing them here says whether the defaults are in place
   * before init, which is where the discrepancy has to be settled.
   */

  {
    FAR struct eh_host_sdio_config *cfg = NULL;

    if (eh_host_sdio_get_config(&cfg) == EH_HOST_TRANSPORT_RC_OK && cfg != NULL)
      {
        syslog(LOG_INFO,
               "%s: transport config: sdio tx_q=%u rx_q=%u width=%u clk=%u kHz\n",
               ENTRY_TAG, (unsigned)cfg->tx_queue_size,
               (unsigned)cfg->rx_queue_size, (unsigned)cfg->bus_width,
               (unsigned)cfg->clock_freq_khz);
      }
    else
      {
        syslog(LOG_WARNING, "%s: cannot read the transport config\n", ENTRY_TAG);
      }
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
      /* Sample the card-interrupt line again: the first sample (before init)
       * read high, so if it reads low here the companion raised the interrupt
       * during/after our traffic -- which is the normal "come and read" signal,
       * and the failure is that the host cannot service it (appendix BP).
       */

      syslog(LOG_ERR,
             "%s: connect to slave failed: %d (D1 now = %d, 0 = companion asking for service)\n",
             ENTRY_TAG, ret, esp_gpioread(CONFIG_ESPRESSIF_SDMMC_D1));
      /* Run the same register read from this thread.
       *
       * The bring-up ladder (boot thread, before the stack) reads INT_RAW fine,
       * while the transport's read of the same register hangs (appendix BV.11).
       * Doing it here -- a task, with the stack already up but not in a transfer
       * -- separates "task context" from "the stack's transfer state" as the
       * cause.
       */

      {
        extern int eh_host_port_sdio_read_reg(void *, uint32_t, uint8_t *,
                                              uint16_t, bool);
        uint8_t rb[4] = { 0, 0, 0, 0 };
        int rr = eh_host_port_sdio_read_reg(NULL, 0x3ff55050u, rb, 4, true);

        syslog(LOG_INFO,
               "%s: post-failure INT_RAW probe: ret=%d bytes=%02x %02x %02x %02x\n",
               ENTRY_TAG, rr, rb[0], rb[1], rb[2], rb[3]);
      }

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

  /* Bring the radio up over RPC (goal stage 4).
   *
   * The caps handshake only opens the transport; the Wi-Fi subsystem on the
   * companion is initialized by host-side RPCs.  eh_host_wifi_init() sends
   * Req_WifiInit and waits for the response -- that request/response pair is the
   * "one real RPC round trip" goal stage 3 asks for -- and the mode/start calls
   * put the station into STA so the netdev's scan/connect ioctls have something
   * to talk to.
   */

  {
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t          werr;

    werr = eh_host_wifi_init(&init_cfg);
    syslog(werr == ESP_OK ? LOG_INFO : LOG_ERR,
           "%s: RPC wifi_init -> %d\n", ENTRY_TAG, werr);

    if (werr == ESP_OK)
      {
        werr = eh_host_wifi_set_mode(WIFI_MODE_STA);
        syslog(werr == ESP_OK ? LOG_INFO : LOG_ERR,
               "%s: RPC wifi_set_mode(STA) -> %d\n", ENTRY_TAG, werr);
      }

    if (werr == ESP_OK)
      {
        werr = eh_host_wifi_start();
        syslog(werr == ESP_OK ? LOG_INFO : LOG_ERR,
               "%s: RPC wifi_start -> %d\n", ENTRY_TAG, werr);
      }

    /* One scan through the netdev's own ioctl path (goal stage 4).
     *
     * This is exactly what a NuttX application does -- a socket plus
     * SIOCSIWSCAN on "wlan0" -- so a passing run proves the whole chain:
     * wlan0's netdev ioctl -> esp-hosted RPC -> the scanner on the companion ->
     * results back into the interface's table.  It needs no shell command in the
     * image, which is why it is here rather than in an app.
     */

    if (werr == ESP_OK)
      {
        struct iwreq iwr;
        int          fd;
        int          rc;

        memset(&iwr, 0, sizeof(iwr));
        strlcpy(iwr.ifr_name, "wlan0", IFNAMSIZ);

        fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0)
          {
            syslog(LOG_ERR, "%s: scan: no socket: %d\n", ENTRY_TAG, errno);
          }
        else
          {
            rc = ioctl(fd, SIOCSIWSCAN, (unsigned long)(uintptr_t)&iwr);
            syslog(rc == 0 ? LOG_INFO : LOG_ERR,
                   "%s: SIOCSIWSCAN(wlan0) -> %d\n", ENTRY_TAG, rc);
            close(fd);
          }
      }
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
