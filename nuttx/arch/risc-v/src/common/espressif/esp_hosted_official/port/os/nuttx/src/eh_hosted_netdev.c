/****************************************************************************
 * esp_hosted_official/port/os/nuttx/src/eh_hosted_netdev.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * wlan0 for NuttX, carried by the official esp-hosted host stack.
 *
 * How the pieces fit:
 *
 *   NuttX network stack
 *        |  netdev_input()  ^  d_iftransmit()
 *        v                  |
 *   this file (net_driver_s)        <- wlan0
 *        |  channel rx      ^  eh_host_transport_tx()
 *        v                  |
 *   official stack: eh_host_transport_add_channel(ESP_STA_IF, ...)
 *        |  frames
 *        v
 *   transport + SDIO port (our eh_host_port_sdio.c)
 *        |  CMD53 / DAT0..DAT3
 *        v
 *   ESP32-C6 companion
 *
 * Route B does not port esp_netif/lwip: those exist to give ESP-IDF an lwIP
 * netif, and on NuttX the net device is a net_driver_s instead.  So the glue is
 * exactly this file -- the stack's channel API on one side, netdev_input() and
 * d_iftransmit() on the other -- and nothing above it needs to know esp-hosted
 * exists.
 *
 * Packet buffer conventions here are the flat-buffer ones: receive copies into
 * dev->d_buf and calls netdev_input(), transmit reads dev->d_buf/dev->d_len.
 *
 * Status: structurally complete, compiled into the option-on image; its runtime
 * behaviour has not been observed yet because the companion's SDIO data phase is
 * stuck on this board (docs appendix BH) -- the stack cannot even finish its
 * caps handshake, so no frame has been exchanged.
 */

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/kmalloc.h>
#include <nuttx/net/netdev.h>
#include <nuttx/net/ip.h>
#include <nuttx/net/ethernet.h>
#include <nuttx/wireless/wireless.h>

#include "eh_common_interface.h"
#include "eh_host_mcu_transport_channels.h"
#ifdef CONFIG_ESPRESSIF_HOSTED_WIFI
#  include "eh_host_wifi.h"
#endif

/* Scan results are kept here between SIOCSIWSCAN and SIOCGIWSCAN, which is the
 * model NuttX's wireless layer expects (the driver owns the BSS table).
 */

#define HOSTED_MAX_AP 32

#ifdef CONFIG_ESPRESSIF_HOSTED_OFFICIAL

#define NETDEV_TAG "eh_hosted_netdev"

/* Frames handed up must fit the netdev buffer; the companion advertises 1536
 * byte transfers, and an Ethernet frame is at most 1518, so one MTU-sized
 * buffer is enough.
 */

#define HOSTED_NETDEV_MTU 1500
#define HOSTED_NETDEV_BUFSIZE (HOSTED_NETDEV_MTU + 64)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct hosted_netdev_s
{
  struct net_driver_s dev;      /* Must come first: the network stack casts */
  bool                bifup;    /* The interface has been taken up        */
  eh_host_channel_t  *channel;  /* Stack-side channel for ESP_STA_IF      */
  eh_host_channel_tx_fn_t txfn; /* Bound TX entry point                   */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct hosted_netdev_s g_hosted_netdev;

#if defined(CONFIG_NETDEV_IOCTL) && defined(CONFIG_ESPRESSIF_HOSTED_WIFI)
static wifi_ap_record_t g_scan_ap[HOSTED_MAX_AP];
static int               g_scan_count;
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: hosted_netdev_transmit
 *
 * Description:
 *   Send one frame through the stack's STA channel.  The frame is unframed
 *   here; the stack adds the wire header and copies it.
 *
 ****************************************************************************/

static void hosted_netdev_txavail(FAR struct net_driver_s *dev)
{
  FAR struct hosted_netdev_s *priv = (FAR struct hosted_netdev_s *)dev;
  esp_err_t ret;

  /* This NuttX netdev model has no d_iftransmit(): the network stack calls
   * d_txavail() when it has queued a packet in d_buf/d_len, and the driver
   * sends it there.
   */

  {
    static int tx_trace;

    if (tx_trace < 20)
      {
        tx_trace++;
        syslog(LOG_INFO,
               "%s: txavail len=%u txfn=%s channel=%s bifup=%d\n",
               NETDEV_TAG, (unsigned)dev->d_len,
               priv->txfn ? "yes" : "no",
               priv->channel ? "yes" : "no", (int)priv->bifup);
      }
  }

  if (priv->txfn == NULL || dev->d_len == 0)
    {
      return;
    }

  ret = priv->txfn(priv->channel->api_chan, dev->d_buf, dev->d_len);
  if (ret != ESP_OK)
    {
      syslog(LOG_ERR, "%s: tx failed: %d (len %u)\n", NETDEV_TAG, ret,
             (unsigned)dev->d_len);
      NETDEV_TXERRORS(dev);
      return;
    }

  NETDEV_TXPACKETS(dev);
}

/****************************************************************************
 * Name: hosted_netdev_rx
 *
 * Description:
 *   One frame arrived from the companion.  The stack owns the buffer and tells
 *   us whether to free it, so the frame is copied into the netdev buffer and
 *   handed to the network stack before returning.
 *
 ****************************************************************************/

static esp_err_t hosted_netdev_rx(void *h, void *buf, void *buff_to_free,
                                  size_t len)
{
  FAR struct hosted_netdev_s *priv = &g_hosted_netdev;

  (void)h;

  /* Show what actually arrives on this channel, for a bounded while.
   *
   * The companion reports the DHCP address in a DhcpDnsStatus message that its
   * log tags event_id=0 -- the data channel -- while the copies this host's RPC
   * layer receives carry link=0/dhcp=0 (appendix BY.23).  If that message comes
   * through here it is not an Ethernet frame, so printing the first bytes is
   * what tells its shape apart from real traffic (appendix BY.24).
   */

  {
    static int rx_trace;

    if (rx_trace < 40 && len > 0)
      {
        FAR const unsigned char *b = (FAR const unsigned char *)buf;
        char line[3 * 16 + 1];
        unsigned k;
        unsigned n = len < 16 ? (unsigned)len : 16;

        rx_trace++;

        for (k = 0; k < n; k++)
          {
            snprintf(&line[k * 3], 4, " %02x", b[k]);
          }

        line[n * 3] = '\0';
        syslog(LOG_INFO, "%s: rx frame len=%u:%s\n", NETDEV_TAG,
               (unsigned)len, line);
      }
  }

  if (priv->bifup && buf != NULL && len > 0 && len <= HOSTED_NETDEV_BUFSIZE &&
      len >= NET_LL_HDRLEN(&priv->dev))
    {
      FAR struct eth_hdr_s *eth;

      memcpy(priv->dev.d_buf, buf, len);
      priv->dev.d_len = (uint16_t)len;

      /* Hand the frame to the right layer, the way an Ethernet driver does.
       *
       * This used to call netdev_input(&dev, NULL, false): netdev_input takes
       * the L3 input callback as its second argument, so a NULL there is a call
       * through a null pointer -- measured as "Instruction access fault,
       * EPC: 00000000" the moment the interface came up and the first frame
       * arrived (appendix BY.25).  The call was also an implicit declaration
       * here (the header was missing), which is how it went unnoticed.
       */

      eth = (FAR struct eth_hdr_s *)priv->dev.d_buf;

#ifdef CONFIG_NET_IPv4
      if (eth->type == HTONS(ETHTYPE_IP))
        {
          ipv4_input(&priv->dev);
        }
      else
#endif
#ifdef CONFIG_NET_ARP
      if (eth->type == HTONS(ETHTYPE_ARP))
        {
          arp_input(&priv->dev);
        }
      else
#endif
        {
          NETDEV_RXDROPPED(&priv->dev);
          priv->dev.d_len = 0;
        }

      /* A reply the stack wants to send goes straight back out (ARP answers,
       * ICMP echo, and so on), exactly as the other flat-buffer drivers do.
       */

      if (priv->dev.d_len > 0 && priv->txfn != NULL)
        {
          if (priv->txfn(priv->channel->api_chan, priv->dev.d_buf,
                         priv->dev.d_len) != ESP_OK)
            {
              NETDEV_TXERRORS(&priv->dev);
            }

          priv->dev.d_len = 0;
        }

      NETDEV_RXPACKETS(&priv->dev);
    }
  else
    {
      NETDEV_RXDROPPED(&priv->dev);
    }

  if (buff_to_free != NULL)
    {
      kmm_free(buff_to_free);
    }

  return ESP_OK;
}

/****************************************************************************
 * Name: hosted_netdev_ioctl
 *
 * Description:
 *   The wireless commands NuttX issues on an interface: scan, then read the
 *   results, then join.  Each one is a thin mapping onto the stack's Wi-Fi API
 *   (eh_host_wifi_*), which turns them into RPCs to the companion.
 *
 ****************************************************************************/

#if defined(CONFIG_NETDEV_IOCTL) && defined(CONFIG_ESPRESSIF_HOSTED_WIFI)
static int hosted_netdev_ioctl(FAR struct net_driver_s *dev, int cmd,
                               unsigned long arg)
{
  FAR struct iwreq *iwr = (FAR struct iwreq *)arg;
  esp_err_t ret;

  /* Say what arrived before deciding anything.
   *
   * ENOTTY from a wireless command has two very different meanings -- this
   * handler was never reached (the netdev's d_ioctl is unset) or the command
   * number did not match a case here -- and one line settles which (appendix
   * BY.18).
   */

  syslog(LOG_INFO, "%s: ioctl cmd=0x%04x (SIOCSIWSCAN=0x%04x)\n",
         NETDEV_TAG, (unsigned)cmd, (unsigned)SIOCSIWSCAN);

  switch (cmd)
    {
      case SIOCSIWSCAN:
        {
          /* Blocking scan: the caller expects the table to be ready when this
           * returns, and esp-hosted's RPC carries the result back with it.
           */

          uint16_t count = 0;

          ret = eh_host_wifi_scan_start(NULL, true);
          if (ret != ESP_OK)
            {
              syslog(LOG_ERR, "%s: scan failed: %d\n", NETDEV_TAG, ret);
              return -EIO;
            }

          ret = eh_host_wifi_scan_get_ap_num(&count);
          if (ret != ESP_OK)
            {
              return -EIO;
            }

          if (count > HOSTED_MAX_AP)
            {
              count = HOSTED_MAX_AP;
            }

          g_scan_count = 0;

          if (count > 0)
            {
              ret = eh_host_wifi_scan_get_ap_records(&count, g_scan_ap);
              if (ret != ESP_OK)
                {
                  return -EIO;
                }

              g_scan_count = count;
            }

          syslog(LOG_INFO, "%s: scan done, %d AP(s)\n", NETDEV_TAG, g_scan_count);
          return OK;
        }

      case SIOCGIWSCAN:
        {
          /* Marshal the cached table as an iw_event stream.  The caller passes
           * a buffer and its size; whatever does not fit is reported by the
           * length written, which is how the wireless layer detects a short
           * buffer.
           */

          FAR uint8_t *out = (FAR uint8_t *)iwr->u.data.pointer;
          size_t       room = iwr->u.data.length;
          size_t       used = 0;
          int          i;

          for (i = 0; i < g_scan_count; i++)
            {
              FAR wifi_ap_record_t *ap = &g_scan_ap[i];
              struct iw_event ev;
              size_t          need;

              /* The AP's address, so the entry can be identified. */

              memset(&ev, 0, sizeof(ev));
              ev.cmd = SIOCGIWAP;
              ev.u.ap_addr.sa_family = AF_INET;
              memcpy(ev.u.ap_addr.sa_data, ap->bssid, 6);
              need = IW_EV_LEN(ap_addr);

              if (used + need > room)
                {
                  break;
                }

              memcpy(out + used, &ev, need);
              used += need;

              /* The SSID, which is what a join matches on. */

              memset(&ev, 0, sizeof(ev));
              ev.cmd = SIOCGIWESSID;
              ev.u.essid.pointer = (FAR void *)ap->ssid;
              ev.u.essid.length  = strnlen((FAR const char *)ap->ssid, 32);
              ev.u.essid.flags   = 1;
              need = IW_EV_LEN(essid);

              if (used + need > room)
                {
                  break;
                }

              memcpy(out + used, &ev, need);
              used += need;

              /* Channel and signal strength. */

              memset(&ev, 0, sizeof(ev));
              ev.cmd = SIOCGIWFREQ;
              ev.u.freq.m    = ap->primary;
              ev.u.freq.e    = 0;
              ev.u.freq.flags = IW_FREQ_FIXED;
              need = IW_EV_LEN(freq);

              if (used + need > room)
                {
                  break;
                }

              memcpy(out + used, &ev, need);
              used += need;
            }

          iwr->u.data.length = used;
          return OK;
        }

      case SIOCSIWESSID:
        {
          /* Join: fill the station configuration and connect.  The ESSID
           * arrives as a pointer plus a length, not as a C string.
           */

          FAR const uint8_t *ssid = (FAR const uint8_t *)iwr->u.essid.pointer;
          wifi_config_t      cfg;

          if (ssid == NULL || iwr->u.essid.length == 0 ||
              iwr->u.essid.length > 32)
            {
              return -EINVAL;
            }

          memset(&cfg, 0, sizeof(cfg));
          memcpy(cfg.sta.ssid, ssid, iwr->u.essid.length);

          ret = eh_host_wifi_set_config(WIFI_IF_STA, &cfg);
          if (ret != ESP_OK)
            {
              syslog(LOG_ERR, "%s: set_config failed: %d\n", NETDEV_TAG, ret);
              return -EIO;
            }

          ret = eh_host_wifi_connect();
          if (ret != ESP_OK)
            {
              syslog(LOG_ERR, "%s: connect failed: %d\n", NETDEV_TAG, ret);
              return -EIO;
            }

          syslog(LOG_INFO, "%s: joining \"%.*s\"\n", NETDEV_TAG,
                 (int)iwr->u.essid.length, (FAR const char *)ssid);
          return OK;
        }

      case SIOCGIWESSID:
        {
          wifi_config_t cfg;

          if (iwr->u.essid.pointer == NULL)
            {
              return -EINVAL;
            }

          memset(&cfg, 0, sizeof(cfg));
          if (eh_host_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK)
            {
              return -EIO;
            }

          memcpy(iwr->u.essid.pointer, cfg.sta.ssid,
                 strnlen((FAR const char *)cfg.sta.ssid, 32));
          iwr->u.essid.length = strnlen((FAR const char *)cfg.sta.ssid, 32);
          return OK;
        }

      default:
        return -ENOTTY;
    }
}
#endif /* CONFIG_NETDEV_IOCTL && CONFIG_ESPRESSIF_HOSTED_WIFI */

/****************************************************************************
 * Name: hosted_netdev_ifup / hosted_netdev_ifdown
 *
 * Description:
 *   Bring the interface up or down.  The transport is already running by the
 *   time this can be called (the stack starts at boot, the interface when the
 *   network stack configures it), so these only track state.
 *
 ****************************************************************************/

static int hosted_netdev_ifup(FAR struct net_driver_s *dev)
{
  FAR struct hosted_netdev_s *priv = (FAR struct hosted_netdev_s *)dev;

  priv->bifup = true;
  syslog(LOG_INFO, "%s: wlan0 up\n", NETDEV_TAG);
  return OK;
}

static int hosted_netdev_ifdown(FAR struct net_driver_s *dev)
{
  FAR struct hosted_netdev_s *priv = (FAR struct hosted_netdev_s *)dev;

  priv->bifup = false;
  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp_hosted_netdev_register
 *
 * Description:
 *   Create wlan0 and bind it to the stack's STA channel.  Called after the
 *   stack has connected to the companion: the channel has to exist before the
 *   first frame can arrive, and the stack hands frames to it from its own
 *   receive task.
 *
 * Returned Value:
 *   OK on success, a negated errno otherwise.
 *
 ****************************************************************************/

/****************************************************************************
 * Name: esp_hosted_netdev_up
 *
 * Description:
 *   Bring wlan0 up before anything tries to send through it.
 *
 *   NuttX only hands a frame to the driver's d_txavail() when the interface is
 *   IFF_UP; without this the DHCP client's own DISCOVER never left the host
 *   (measured: no d_txavail call at all, and the companion saw nothing from the
 *   host; appendix BY.25).  Registering the interface is not the same thing as
 *   bringing it up.
 *
 ****************************************************************************/

int esp_hosted_netdev_up(void)
{
  FAR struct hosted_netdev_s *priv = &g_hosted_netdev;

  netdev_ifup(&priv->dev);

  syslog(LOG_INFO, "%s: wlan0 up (IFF_UP set)\n", NETDEV_TAG);
  return OK;
}

/****************************************************************************
 * Name: esp_hosted_netdev_apply_ip
 *
 * Description:
 *   Give wlan0 the address the companion obtained over DHCP.
 *
 *   esp-hosted reports it in the DhcpDnsStatus event (dhcp_ip / dhcp_nm /
 *   dhcp_gw, four bytes each, network order), which is the only place the
 *   address appears -- Req_GetDhcpDnsStatus is not answered by this companion
 *   (measured, appendix BY.22).  This is the "event -> netif address" hop the
 *   goal's stage 4 needs so ifconfig shows the interface as configured.
 *
 ****************************************************************************/

int esp_hosted_netdev_apply_ip(FAR const uint8_t *ip, FAR const uint8_t *nm,
                               FAR const uint8_t *gw)
{
  FAR struct hosted_netdev_s *priv = &g_hosted_netdev;

  if (ip == NULL)
    {
      return -EINVAL;
    }

  priv->dev.d_ipaddr  = ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
                        ((uint32_t)ip[2] << 8) | (uint32_t)ip[3];
  priv->dev.d_netmask = (nm != NULL) ?
      (((uint32_t)nm[0] << 24) | ((uint32_t)nm[1] << 16) |
       ((uint32_t)nm[2] << 8) | (uint32_t)nm[3]) : 0xffffff00;
  priv->dev.d_draddr  = (gw != NULL) ?
      (((uint32_t)gw[0] << 24) | ((uint32_t)gw[1] << 16) |
       ((uint32_t)gw[2] << 8) | (uint32_t)gw[3]) : 0;

  netdev_ifup(&priv->dev);

  syslog(LOG_INFO,
         "%s: wlan0 address %u.%u.%u.%u mask %u.%u.%u.%u gw %u.%u.%u.%u "
         "(from the DhcpDnsStatus event)\n",
         NETDEV_TAG, ip[0], ip[1], ip[2], ip[3],
         priv->dev.d_netmask >> 24 & 0xff, (priv->dev.d_netmask >> 16) & 0xff,
         (priv->dev.d_netmask >> 8) & 0xff, priv->dev.d_netmask & 0xff,
         priv->dev.d_draddr >> 24 & 0xff, (priv->dev.d_draddr >> 16) & 0xff,
         (priv->dev.d_draddr >> 8) & 0xff, priv->dev.d_draddr & 0xff);

  return OK;
}

int esp_hosted_netdev_register(void)
{
  FAR struct hosted_netdev_s *priv = &g_hosted_netdev;
  FAR uint8_t *rxbuf;
  int ret;

  if (priv->dev.d_buf != NULL)
    {
      return OK;                      /* Already registered */
    }

  rxbuf = (FAR uint8_t *)kmm_malloc(HOSTED_NETDEV_BUFSIZE);
  if (rxbuf == NULL)
    {
      return -ENOMEM;
    }

  memset(priv, 0, sizeof(*priv));

  priv->dev.d_buf     = rxbuf;
  priv->dev.d_ifup    = hosted_netdev_ifup;
  priv->dev.d_ifdown  = hosted_netdev_ifdown;
  priv->dev.d_txavail = hosted_netdev_txavail;
#if defined(CONFIG_NETDEV_IOCTL) && defined(CONFIG_ESPRESSIF_HOSTED_WIFI)
  priv->dev.d_ioctl   = hosted_netdev_ioctl;
#endif

  /* A locally administered address until the companion reports the station
   * MAC over the control channel (goal stage 4 wires that through).
   */

  priv->dev.d_mac.ether.ether_addr_octet[0] = 0x02;
  priv->dev.d_mac.ether.ether_addr_octet[1] = 0x00;
  priv->dev.d_mac.ether.ether_addr_octet[2] = 0x00;
  priv->dev.d_mac.ether.ether_addr_octet[3] = 0x00;
  priv->dev.d_mac.ether.ether_addr_octet[4] = 0x00;
  priv->dev.d_mac.ether.ether_addr_octet[5] = 0x01;

  /* Name it wlan0 (goal stage 4).
   *
   * netdev_register() takes an already-set d_ifname as the name format
   * (net/netdev/netdev_register.c), so this is the documented way to choose the
   * interface name instead of accepting the "eth" prefix the link type implies.
   */

  strlcpy(priv->dev.d_ifname, "wlan0", IFNAMSIZ);

  ret = netdev_register(&priv->dev, NET_LL_ETHERNET);
  if (ret < 0)
    {
      kmm_free(rxbuf);
      priv->dev.d_buf = NULL;
      syslog(LOG_ERR, "%s: netdev_register failed: %d\n", NETDEV_TAG, ret);
      return ret;
    }

  priv->channel = eh_host_transport_add_channel(NULL, ESP_STA_IF, 0,
                                                &priv->txfn,
                                                hosted_netdev_rx);
  if (priv->channel == NULL)
    {
      syslog(LOG_ERR, "%s: cannot register the STA channel\n", NETDEV_TAG);
      return -EIO;
    }

  syslog(LOG_INFO, "%s: %s registered (STA channel bound)\n",
         NETDEV_TAG, priv->dev.d_ifname);
  return OK;
}

#endif /* CONFIG_ESPRESSIF_HOSTED_OFFICIAL */
