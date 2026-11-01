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
#include "eh_host_feat_rpc.h"
#include "eh_host_feat_rpc_ext_v2_types.h"
#include "gen_v2.h"
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

/****************************************************************************
 * Name: hosted_dhcp_event
 *
 * Description:
 *   The DhcpDnsStatus event carries the address the companion got over DHCP.
 *   Handing it to wlan0 is what makes the interface usable -- and it is the only
 *   way, because Req_GetDhcpDnsStatus is not answered by this companion
 *   (measured, appendix BY.22).  The callback runs on the RPC dispatch thread
 *   with a const view of the message; it only reads it.
 *
 ****************************************************************************/

static void hosted_dhcp_event(FAR const void *ctrl_cmd, FAR void *ctx)
{
  FAR const eh_rpc_ctrl_cmd_t *c = (FAR const eh_rpc_ctrl_cmd_t *)ctrl_cmd;
  FAR const eh_rpc_dhcp_dns_t *e;

  UNUSED(ctx);

  if (c == NULL)
    {
      return;
    }

  e = &c->u.dhcp_dns;

  syslog(LOG_INFO,
         "%s: DhcpDnsStatus iface=%d link=%d dhcp=%d ip=%u.%u.%u.%u\n",
         ENTRY_TAG, (int)e->iface, (int)e->net_link_up, (int)e->dhcp_up,
         e->dhcp_ip.len == 4 ? e->dhcp_ip.data[0] : 0,
         e->dhcp_ip.len == 4 ? e->dhcp_ip.data[1] : 0,
         e->dhcp_ip.len == 4 ? e->dhcp_ip.data[2] : 0,
         e->dhcp_ip.len == 4 ? e->dhcp_ip.data[3] : 0);

  if (e->dhcp_up && e->dhcp_ip.len == 4)
    {
      (void)esp_hosted_netdev_apply_ip(e->dhcp_ip.data,
                                       e->dhcp_nm.len == 4 ? e->dhcp_nm.data
                                                           : NULL,
                                       e->dhcp_gw.len == 4 ? e->dhcp_gw.data
                                                           : NULL);
    }
}

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

  /* Listen for the address the companion gets over DHCP. */

  if (eh_host_feat_rpc_register_event(RPC_ID__Event_DhcpDnsStatus,
                                      hosted_dhcp_event, NULL) != 0)
    {
      syslog(LOG_WARNING, "%s: could not subscribe to DhcpDnsStatus\n",
             ENTRY_TAG);
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

    /* Do NOT re-initialize the companion's Wi-Fi here.
     *
     * Measured: Req_WifiInit makes the companion re-run its Wi-Fi init ("WiFi
     * init config changed, reinitializing" on its console), which stops the
     * radio its own application had already started.  Every later call then
     * answers ESP_ERR_WIFI_NOT_STARTED (0x3002) -- scan included -- and the
     * Req_WifiStart that would fix it is a synchronous call the companion never
     * answers (appendix BY.18).  The radio is already initialized and started by
     * the slave application, so the host only sets the mode.
     */

    syslog(LOG_INFO, "%s: leaving the companion's Wi-Fi initialized/started "
           "(no RPC wifi_init)\n", ENTRY_TAG);
    (void)init_cfg;
    werr = ESP_OK;

    if (werr == ESP_OK)
      {
        werr = eh_host_wifi_set_mode(WIFI_MODE_STA);
        syslog(werr == ESP_OK ? LOG_INFO : LOG_ERR,
               "%s: RPC wifi_set_mode(STA) -> %d\n", ENTRY_TAG, werr);
      }

    /* No eh_host_wifi_start() here.
     *
     * It blocks: Req_WifiStart is a synchronous RPC and the companion never
     * answers this one, because the slave firmware flashed on this board is the
     * "Wi-Fi station" example, whose own application has already started the
     * radio -- a second start from the host side never returns (measured: the
     * call hangs; every later step, including the scan, never runs; appendix
     * BY.17).  The radio is up, the mode is STA, so scan/connect RPCs can go
     * straight out; when this board gets the pure-RPC (network adapter) slave
     * firmware, this call belongs back here.
     */

    syslog(LOG_INFO, "%s: skipping RPC wifi_start (the slave app already started "
           "the radio)\n", ENTRY_TAG);

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
            /* Retry for a while.
             *
             * The companion's own application keeps trying to join a SSID that
             * is not on the air, and IDF refuses a scan while a connection
             * attempt is in flight: its console says it plainly --
             * "sta_scan: STA is connecting, scan are not allowed!" -- and
             * esp_wifi_scan_start returns ESP_ERR_WIFI_MODE (12294) for it.  That
             * loop ends after six tries (about eighteen seconds), so the scan is
             * retried until it is allowed (appendix BY.19).
             */

            for (int attempt = 0; attempt < 5; attempt++)
              {
                if (attempt > 0)
                  {
                    nxsig_usleep(6 * 1000 * 1000);
                  }

                errno = 0;
                rc = ioctl(fd, SIOCSIWSCAN, (unsigned long)(uintptr_t)&iwr);
                syslog(rc == 0 ? LOG_INFO : LOG_ERR,
                       "%s: SIOCSIWSCAN(wlan0) try %d -> %d (errno=%d)\n",
                       ENTRY_TAG, attempt + 1, rc, errno);

                if (rc == 0)
                  {
                    break;
                  }
              }

            close(fd);

            /* Read the companion's mode back, force STA, read it again, then
             * ask for the scan.
             *
             * The scan came back ESP_ERR_WIFI_MODE (0x3006) from the companion
             * while set_mode(STA) had answered 0, so the two disagree about the
             * mode and the read-back says which way (appendix BY.19).
             */

            {
              wifi_mode_t mode = WIFI_MODE_NULL;
              esp_err_t   mrc;

              mrc = eh_host_wifi_get_mode(&mode);
              syslog(mrc == ESP_OK ? LOG_INFO : LOG_ERR,
                     "%s: RPC wifi_get_mode -> %d mode=%d\n", ENTRY_TAG, mrc,
                     (int)mode);

              mrc = eh_host_wifi_set_mode(WIFI_MODE_STA);
              syslog(mrc == ESP_OK ? LOG_INFO : LOG_ERR,
                     "%s: RPC wifi_set_mode(STA) again -> %d\n", ENTRY_TAG, mrc);

              mode = WIFI_MODE_NULL;
              mrc = eh_host_wifi_get_mode(&mode);
              syslog(mrc == ESP_OK ? LOG_INFO : LOG_ERR,
                     "%s: RPC wifi_get_mode after set -> %d mode=%d\n",
                     ENTRY_TAG, mrc, (int)mode);
            }

            /* Start the companion's Wi-Fi best-effort, then ask for the scan.
             *
             * The scan answered ESP_ERR_WIFI_MODE (0x3006) while the companion
             * reported mode = STA, which is what its handler returns when the
             * radio was never started *through the RPC path*: the slave
             * application started it on its own, and Req_WifiStart's reply never
             * arrives (a blocking wait only burns the timeout -- measured,
             * appendix BY.19).  Sending it best-effort tells the companion to
             * start and gives up on the reply, which is all this needs.
             */

            {
              eh_rpc_ctrl_cmd_t *wreq = eh_rpc_ctrl_cmd_alloc();

              if (wreq != NULL)
                {
                  void *resp = NULL;

                  (void)eh_host_feat_rpc_request_sync_best_effort(
                          RPC_ID__Req_WifiStart, wreq, &resp);
                  syslog(LOG_INFO, "%s: Req_WifiStart sent (best effort), "
                         "resp=%s\n", ENTRY_TAG, resp ? "yes" : "no");

                  if (resp != NULL)
                    {
                      eh_rpc_ctrl_cmd_free(resp);
                    }
                }
            }

            /* If the netdev path refused it, ask the stack directly.
             *
             * The two failures look identical from the ioctl's return value but
             * have nothing in common: a rejection here is NuttX's netdev layer
             * (ENOTTY/ENODEV/ENETDOWN), while a non-zero return below is the
             * esp-hosted RPC itself.  One line each, so the next run says which
             * one it is (appendix BY.18).
             */

            {
              esp_err_t dir = eh_host_wifi_scan_start(NULL, true);

              syslog(dir == ESP_OK ? LOG_INFO : LOG_ERR,
                     "%s: direct eh_host_wifi_scan_start -> %d\n",
                     ENTRY_TAG, dir);

              /* And the results, which is what the scan is for. */

              if (dir == ESP_OK)
                {
                  static wifi_ap_record_t aps[8];
                  uint16_t count = 0;

                  dir = eh_host_wifi_scan_get_ap_num(&count);
                  syslog(dir == ESP_OK ? LOG_INFO : LOG_ERR,
                         "%s: scan_get_ap_num -> %d count=%u\n",
                         ENTRY_TAG, dir, (unsigned)count);

                  if (dir == ESP_OK && count > 0)
                    {
                      uint16_t n = count > 8 ? 8 : count;

                      dir = eh_host_wifi_scan_get_ap_records(&n, aps);
                      syslog(dir == ESP_OK ? LOG_INFO : LOG_ERR,
                             "%s: scan_get_ap_records -> %d n=%u\n",
                             ENTRY_TAG, dir, (unsigned)n);

                      bool joined = false;

                      for (unsigned i = 0; dir == ESP_OK && i < n; i++)
                        {
                          syslog(LOG_INFO,
                                 "%s: AP[%u] ssid=\"%s\" rssi=%d ch=%u auth=%d%s\n",
                                 ENTRY_TAG, i, (FAR const char *)aps[i].ssid,
                                 (int)aps[i].rssi, (unsigned)aps[i].primary,
                                 (int)aps[i].authmode,
                                 aps[i].authmode == WIFI_AUTH_OPEN ?
                                     "  <-- open" : "");

                          /* Join the first open AP found (goal stage 4).
                           *
                           * An encrypted one would need credentials this board
                           * does not have; an open one exercises exactly the
                           * same RPC path (set_config + connect) with nothing to
                           * configure.
                           */

                          if (!joined && aps[i].authmode == WIFI_AUTH_OPEN)
                            {
                              wifi_config_t ccfg;
                              esp_err_t     crc;

                              memset(&ccfg, 0, sizeof(ccfg));
                              memcpy(ccfg.sta.ssid, aps[i].ssid,
                                     sizeof(ccfg.sta.ssid));

                              crc = eh_host_wifi_set_config(WIFI_IF_STA, &ccfg);
                              syslog(crc == ESP_OK ? LOG_INFO : LOG_ERR,
                                     "%s: RPC wifi_set_config(\"%s\") -> %d\n",
                                     ENTRY_TAG, (FAR const char *)aps[i].ssid,
                                     crc);

                              if (crc == ESP_OK)
                                {
                                  crc = eh_host_wifi_connect();
                                  syslog(crc == ESP_OK ? LOG_INFO : LOG_ERR,
                                         "%s: RPC wifi_connect -> %d\n",
                                         ENTRY_TAG, crc);
                                  joined = crc == ESP_OK;
                                }
                            }
                        }
                    }
                }
            }
          }
      }
  }

  /* Connect test (goal stage 4's connect / DHCP evidence).
   *
   * The credentials are NOT in this file: they come from the build's own config
   * (ESP_WIFI_TEST_SSID / _PASSWORD / _SSID_ALT), so nothing
   * secret is part of the delivered tree.  Empty SSID means "do not run it".
   *
   * Two names are tried in turn because the companion is a 2.4 GHz-only part: an
   * AP that exists only on 5 GHz never shows up in its scan, and the same
   * router's 2.4 GHz interface is the one that can actually be joined.
   */

#ifdef ESP_WIFI_TEST_SSID
  if (strlen(ESP_WIFI_TEST_SSID) > 0)
    {
      FAR const char *names[2] =
        {
          ESP_WIFI_TEST_SSID,
#ifdef ESP_WIFI_TEST_SSID_ALT
          ESP_WIFI_TEST_SSID_ALT
#else
          ""
#endif
        };

      for (int t = 0; t < 2; t++)
        {
          wifi_config_t ccfg;
          wifi_ap_record_t info;
          esp_err_t     crc;
          bool          up = false;

          if (names[t][0] == '\0')
            {
              continue;
            }

          memset(&ccfg, 0, sizeof(ccfg));
          strlcpy((FAR char *)ccfg.sta.ssid, names[t], sizeof(ccfg.sta.ssid));
          strlcpy((FAR char *)ccfg.sta.password,
                  ESP_WIFI_TEST_PASSWORD,
                  sizeof(ccfg.sta.password));

          crc = eh_host_wifi_set_config(WIFI_IF_STA, &ccfg);
          syslog(crc == ESP_OK ? LOG_INFO : LOG_ERR,
                 "%s: RPC wifi_set_config(\"%s\") -> %d\n",
                 ENTRY_TAG, names[t], crc);

          if (crc == ESP_OK)
            {
              crc = eh_host_wifi_connect();
              syslog(crc == ESP_OK ? LOG_INFO : LOG_ERR,
                     "%s: RPC wifi_connect(\"%s\") -> %d\n",
                     ENTRY_TAG, names[t], crc);
            }

          /* The association shows up as sta_get_ap_info succeeding; give it a
           * dozen seconds, which is what the companion's own scan+join takes.
           */

          for (int w = 0; crc == ESP_OK && w < 12; w++)
            {
              nxsig_sleep(1);

              memset(&info, 0, sizeof(info));
              if (eh_host_wifi_sta_get_ap_info(&info) == ESP_OK)
                {
                  syslog(LOG_INFO,
                         "%s: connected to \"%s\" ssid=\"%s\" rssi=%d ch=%u\n",
                         ENTRY_TAG, names[t], (FAR const char *)info.ssid,
                         (int)info.rssi, (unsigned)info.primary);
                  up = true;
                  break;
                }
            }

          if (up)
            {
              /* Ask the companion for its DHCP/DNS status and print the raw
               * reply.  The address is in there; printing the bytes first shows
               * the layout the generated struct uses, so the next step can apply
               * it to wlan0 instead of guessing field offsets (appendix BY.22).
               */

              eh_rpc_ctrl_cmd_t *dreq = eh_rpc_ctrl_cmd_alloc();

              if (dreq != NULL)
                {
                  void *dresp = NULL;

                  if (eh_host_feat_rpc_request_sync(RPC_ID__Req_GetDhcpDnsStatus,
                                                    dreq, &dresp) == 0 &&
                      dresp != NULL)
                    {
                      FAR const unsigned char *db = (FAR const unsigned char *)
                                                    dresp;
                      char line[3 * 24 + 1];
                      unsigned k;

                      for (k = 0; k < 24; k++)
                        {
                          snprintf(&line[k * 3], 4, " %02x", db[k]);
                        }

                      line[24 * 3] = '\0';
                      syslog(LOG_INFO, "%s: dhcp status raw:%s\n",
                             ENTRY_TAG, line);
                    }

                  if (dresp != NULL)
                    {
                      eh_rpc_ctrl_cmd_free(dresp);
                    }
                }

              break;
            }

          syslog(LOG_WARNING, "%s: could not join \"%s\"\n",
                 ENTRY_TAG, names[t]);
        }
    }

#endif /* ESP_WIFI_TEST_SSID */

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
