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
#include <string.h>
#include <syslog.h>

#include <nuttx/kmalloc.h>
#include <nuttx/net/netdev.h>

#include "eh_common_interface.h"
#include "eh_host_mcu_transport_channels.h"

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

  if (priv->txfn == NULL || dev->d_len == 0)
    {
      return;
    }

  ret = priv->txfn(priv->channel->api_chan, dev->d_buf, dev->d_len);
  if (ret != ESP_OK)
    {
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

  if (priv->bifup && buf != NULL && len > 0 && len <= HOSTED_NETDEV_BUFSIZE)
    {
      memcpy(priv->dev.d_buf, buf, len);
      priv->dev.d_len = (uint16_t)len;
      netdev_input(&priv->dev, NULL, false);
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

  /* A locally administered address until the companion reports the station
   * MAC over the control channel (goal stage 4 wires that through).
   */

  priv->dev.d_mac.ether.ether_addr_octet[0] = 0x02;
  priv->dev.d_mac.ether.ether_addr_octet[1] = 0x00;
  priv->dev.d_mac.ether.ether_addr_octet[2] = 0x00;
  priv->dev.d_mac.ether.ether_addr_octet[3] = 0x00;
  priv->dev.d_mac.ether.ether_addr_octet[4] = 0x00;
  priv->dev.d_mac.ether.ether_addr_octet[5] = 0x01;

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
