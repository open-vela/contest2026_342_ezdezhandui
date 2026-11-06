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
#include <nuttx/net/net.h>
#include <nuttx/net/netdev.h>
#include <nuttx/net/ip.h>
#include <nuttx/net/ethernet.h>
#include <nuttx/wireless/wireless.h>
#include <nuttx/net/dns.h>
#include <netutils/dhcpc.h>
#include <nuttx/wqueue.h>
#include <nuttx/sched.h>
#include <nuttx/clock.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/route.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "eh_common_interface.h"
#include "eh_host_sys.h"
#include "eh_host_mcu_transport_channels.h"
#include "eh_hosted_log.h"
#ifdef CONFIG_ESPRESSIF_HOSTED_WIFI
#  include "eh_host_wifi.h"
#endif

/* Scan results are kept here between SIOCSIWSCAN and SIOCGIWSCAN, which is the
 * model NuttX's wireless layer expects (the driver owns the BSS table).
 */

#define HOSTED_MAX_AP 32

#ifdef CONFIG_ESPRESSIF_HOSTED_OFFICIAL

#define NETDEV_TAG "eh_hosted_netdev"

/* 每个收到的以太网帧都打一行 hex —— 真机上一秒钟几十行，必须门控。
 * 用 ESPRESSIF_HOSTED_LOG_VERBOSE 统一控制。
 */

#ifdef CONFIG_ESPRESSIF_HOSTED_LOG_VERBOSE
#  define EH_NETDEV_TRACE(...) syslog(LOG_INFO, __VA_ARGS__)
#else
#  define EH_NETDEV_TRACE(...) do { } while (0)
#endif

/* 扫描链路的诊断寄存器（真机取证用）。
 *
 * 板上 115200 console 会被 SDIO 日志占满（nsh_consolewrite 发不出 buffer），
 * 所以这些值故意做成**非 static 全局**：用 `tools/test/hw_var_read.sh` 按符号名
 * 从 ELF 取地址、OpenOCD 读 RAM，就能在不依赖串口的情况下看清整条链。
 */

int g_dbg_wifi_setmode_rc;     /* 扫描前 set_mode(STA) 的返回值 */
int g_dbg_wifi_mode_rc;        /* 回读 get_mode 的返回值 */
int g_dbg_wifi_mode;           /* 回读到的模式（2 = WIFI_MODE_STA） */
int g_dbg_wifi_scan_rc;        /* eh_host_wifi_scan_start 的返回值 */
int g_dbg_wifi_apnum_rc;       /* scan_get_ap_num 的返回值 */
int g_dbg_wifi_apnum;          /* scan_get_ap_num 给出的 AP 数 */
int g_dbg_wifi_scan_calls;     /* 走进扫描分支的次数 */

/* 连接（含密码）与 AP 能力实验的诊断（同样非 static，JTAG 可读） */

/* 哨兵初值 -999：把"分支根本没跑到"与"跑到了但结果为空"区分开
 * （两者默认都是 0，上一轮就是因此无法判定 ENCODE 到底有没有到达驱动）。
 */

#define DM_DBG_UNSET (-999)

int g_dbg_wifi_enc_rc = DM_DBG_UNSET;      /* SIOCSIWENCODE(EXT) 的返回 */
int g_dbg_wifi_psk_len = DM_DBG_UNSET;     /* 收到的密码长度 */
int g_dbg_wifi_setcfg_rc = DM_DBG_UNSET;   /* set_config(STA) 的返回 */
int g_dbg_wifi_connect_rc = DM_DBG_UNSET;  /* WifiConnect RPC 的返回 */
int g_dbg_wifi_rssi = DM_DBG_UNSET;        /* 已关联 AP 的 RSSI */
int g_dbg_ap_setmode_rc = DM_DBG_UNSET;    /* set_mode(WIFI_MODE_AP) 的返回 */
int g_dbg_ap_setcfg_rc = DM_DBG_UNSET;     /* set_config(WIFI_IF_AP) 的返回 */
int g_dbg_ap_restore_rc = DM_DBG_UNSET;    /* 实验后恢复 set_mode(STA) 的返回 */

/* 2026-11-05：逐帧诊断日志默认关闭（每帧一行会刷爆控制台，把 ping/DHCP 结果冲出屏幕）。
 * 2026-11-06：原来这里用一个本地的 EH_NETDEV_TRACE 数值开关去门控 EH_TRACE，但
 * EH_NETDEV_TRACE 在上面的 #ifdef CONFIG_ESPRESSIF_HOSTED_LOG_VERBOSE 分支里已经被
 * 定义成**函数式宏**了 —— 于是 `#ifndef EH_NETDEV_TRACE` 恒不成立、`#if
 * EH_NETDEV_TRACE` 恒为假（函数式宏在 #if 里的值永远当 0），注释说的"改成 1 重新
 * 编译即可恢复"其实一次都没生效过。现在直接复用 eh_hosted_log.h 里的 EH_TRACE，
 * 唯一开关是 CONFIG_ESPRESSIF_HOSTED_LOG_VERBOSE。
 */

static char g_sta_psk[65];     /* 暂存的 STA 密码：ENCODE 先到、ESSID 后到 */

/* 2026-11-05：静态地址兜底只做一次（见 hosted_netdev_txpoll 里的推导） */
static bool g_static_fixup_done;

/* 由本文件下方定义、供 TX 路径兜底调用 */
int esp_hosted_netdev_apply_ip(FAR const uint8_t *ip, FAR const uint8_t *nm,
                               FAR const uint8_t *gw);

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
  bool                   mac_learned; /* d_mac holds the station's own MAC */
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

static int hosted_netdev_txpoll(FAR struct net_driver_s *dev);

static void hosted_netdev_txavail(FAR struct net_driver_s *dev)
{
  FAR struct hosted_netdev_s *priv = (FAR struct hosted_netdev_s *)dev;

  /* This NuttX netdev model has no d_iftransmit(): the network stack calls
   * d_txavail() when it has queued a packet in d_buf/d_len, and the driver
   * sends it there.
   */

  {
    static int tx_trace;

    if (tx_trace < 20)
      {
        tx_trace++;
        EH_TRACE(
               "%s: txavail len=%u txfn=%s channel=%s bifup=%d\n",
               NETDEV_TAG, (unsigned)dev->d_len,
               priv->txfn ? "yes" : "no",
               priv->channel ? "yes" : "no", (int)priv->bifup);
      }
  }

  if (priv->txfn == NULL || priv->channel == NULL)
    {
      return;
    }

  /* Ask the stack for whatever it has queued; it fills d_buf/d_len and calls
   * hosted_netdev_txpoll() for each frame.
   *
   * d_txavail alone is only a notification in this NuttX: the transmit path is
   * devif_poll().  Measured with a notification-only driver: d_txavail ran with
   * d_len == 0 (nothing was ever staged for us), no frame ever reached the
   * companion, and the DHCP client's broadcast blocked inside sendto()
   * (appendix BY.26).
   */

  /* devif_poll() walks the stack's send queues and expects the network lock to
   * be held, the way the in-tree drivers call it.
   */

  net_lock();
  devif_poll(dev, hosted_netdev_txpoll);
  net_unlock();
}

/****************************************************************************
 * Name: hosted_netdev_txpoll
 *
 * Description:
 *   One frame the stack wants sent.  Called by devif_poll() with the frame
 *   already in d_buf/d_len; returning zero lets the poll continue while
 *   returning non-zero ends it.
 *
 ****************************************************************************/

static int hosted_netdev_txpoll(FAR struct net_driver_s *dev)
{
  FAR struct hosted_netdev_s *priv = (FAR struct hosted_netdev_s *)dev;
  static int poll_trace;

  if (poll_trace < 20)
    {
      poll_trace++;
      EH_TRACE("%s: txpoll len=%u\n", NETDEV_TAG,
             (unsigned)dev->d_len);
    }

  if (dev->d_len == 0 || priv->txfn == NULL || priv->channel == NULL)
    {
      syslog(LOG_ERR, "[TX-CHK] skip: d_len=%u txfn=%p chan=%p\n",
             (unsigned)dev->d_len, (FAR void *)priv->txfn,
             (FAR void *)priv->channel);
      return 1;
    }

  /* 2026-11-05 止血：源 MAC 全零的帧绝对不能发出去。
   *
   * 真机实测（TX 帧头日志）：
   *   `TX 304 B type=0800 dst=ff:ff:ff:ff:ff:ff src=00:00:00:00:00:00`
   * 全零源地址不是有效站点，AP 的网桥无从投递回包 —— 表现为 DHCP 收不到
   * OFFER、ARP 无应答、ping 全丢。这里兜底成占位地址并显式报错，
   * 避免再出现"静默发坏帧"这种最难查的故障形态。
   */

  if (dev->d_mac.ether.ether_addr_octet[0] == 0 &&
      dev->d_mac.ether.ether_addr_octet[1] == 0 &&
      dev->d_mac.ether.ether_addr_octet[2] == 0 &&
      dev->d_mac.ether.ether_addr_octet[3] == 0 &&
      dev->d_mac.ether.ether_addr_octet[4] == 0 &&
      dev->d_mac.ether.ether_addr_octet[5] == 0)
    {
      dev->d_mac.ether.ether_addr_octet[0] = 0x02;
      dev->d_mac.ether.ether_addr_octet[1] = 0x00;
      dev->d_mac.ether.ether_addr_octet[2] = 0x00;
      dev->d_mac.ether.ether_addr_octet[3] = 0x00;
      dev->d_mac.ether.ether_addr_octet[4] = 0x00;
      dev->d_mac.ether.ether_addr_octet[5] = 0x01;

      syslog(LOG_ERR, "%s: FATAL: d_mac was all zero at TX; forced placeholder "
             "02:00:00:00:00:01\n", NETDEV_TAG);
    }

  /* 2026-11-05：任何方式拿到地址后都必须有默认路由。
   *
   * DHCP 路径由 apply_ip() 装好了路由+DNS；但用 `wapi ip` / `wapi mask` 设
   * **静态地址**时那条路不会跑 —— 于是同网段通（实测 ping 网关 9/10 回包、
   * DNS 解析成功），出网段却 `sendto failed: 101`（ENETUNREACH）。
   * 这里兜底：接口已有地址但没有网关时，按 x.x.x.1 推导出网关，
   * 交给同一个 apply_ip() 统一处理（地址/掩码/网关/DNS/路由）。
   */

  /* 2026-11-05：发送前自愈 —— 若还没拿到真实 STA MAC，先试一次再发。
   * 真机踩过：注册时取 MAC 太早会失败，之后若没有连接动作就永远保持全零，
   * 于是 ARP/DHCP 全部被 AP 丢弃（`sendto failed: 101`）。
   */

  if (!g_hosted_netdev.mac_learned)
    {
      uint8_t hm[6];

      if (eh_host_sys_get_mac(WIFI_IF_STA, hm) == ESP_OK)
        {
          memcpy(dev->d_mac.ether.ether_addr_octet, hm,
                 sizeof(dev->d_mac.ether.ether_addr_octet));
          g_hosted_netdev.mac_learned = true;

          EH_INFO( "%s: station MAC learnt before TX "
                 "%02x:%02x:%02x:%02x:%02x:%02x\n", NETDEV_TAG,
                 hm[0], hm[1], hm[2], hm[3], hm[4], hm[5]);
        }
    }

  if (!g_static_fixup_done && dev->d_ipaddr != 0 && dev->d_netmask != 0 &&
      dev->d_draddr == 0)
    {
      uint8_t  ipb[4];
      uint8_t  nmb[4];
      uint8_t  gwb[4];
      uint32_t gw  = htonl((ntohl(dev->d_ipaddr) & ntohl(dev->d_netmask)) | 1u);

      g_static_fixup_done = true;

      /* 按**内存字节**取，不能用 >>24 位移：
       * NuttX 的 d_ipaddr / d_netmask 是网络字节序，小端机上位移会把整个地址
       * 反序（实测 192.168.1.200 被打成 200.1.168.192，网关 192.168.1.1 变成
       * 0.1.168.193，于是立刻 ENETUNREACH）。apply_ip() 收的正是这种字节数组。
       * `(ip & mask) | 1` 在网络字节序下恰好把最后一字节置 1 = x.x.x.1 ✓
       */

      memcpy(ipb, &dev->d_ipaddr, sizeof(ipb));
      memcpy(nmb, &dev->d_netmask, sizeof(nmb));
      memcpy(gwb, &gw, sizeof(gwb));

      EH_INFO( "%s: static-IP fallback: gateway %u.%u.%u.%u "
             "(route + DNS installed)\n", NETDEV_TAG,
             gwb[0], gwb[1], gwb[2], gwb[3]);

      (void)esp_hosted_netdev_apply_ip(ipb, nmb, gwb);
    }

  if (dev->d_len >= 14)
    {
      FAR uint8_t *p = (FAR uint8_t *)dev->d_buf;

      /* 2026-11-05：DHCP 报文取证（仅 DHCP 帧）。
       *
       * BOOTP 布局：xid[4..8] flags[10..12] ciaddr[12..16] yiaddr[16..20]
       * chaddr[28..34]。若 chaddr 与以太源 MAC 不一致，DHCP 服务器通常直接忽略
       * 该请求 —— 与"DISCOVER 已发出但无 OFFER"的现象完全吻合。
       */

      if (p[12] == 0x08 && p[13] == 0x00 && dev->d_len > 14 + 20 + 8 + 34)
        {
          FAR uint8_t *ip4 = p + 14;
          uint8_t      ihl = (uint8_t)((ip4[0] & 0x0f) * 4);

          if (ip4[9] == 17 && (uint32_t)dev->d_len > 14u + ihl + 8u + 34u)
            {
              FAR uint8_t *udp = ip4 + ihl;

              if (udp[2] == 0x00 && udp[3] == 0x43)          /* dport 67 = DHCP server */
                {
                  /* 整个块只做取证打印：INFO 关掉时连 `b` 的推导都不该存在
                   * （否则 -Wunused-variable，且白白多一段解析）。 */

#ifdef CONFIG_ESPRESSIF_HOSTED_LOG_INFO
                  FAR uint8_t *b = udp + 8;
#endif

                  EH_INFO(
                         "%s: DHCP pkt: xid=%02x%02x%02x%02x flags=%02x%02x "
                         "ciaddr=%u.%u.%u.%u yiaddr=%u.%u.%u.%u "
                         "chaddr=%02x:%02x:%02x:%02x:%02x:%02x "
                         "ethsrc=%02x:%02x:%02x:%02x:%02x:%02x\n",
                         NETDEV_TAG,
                         b[4], b[5], b[6], b[7], b[10], b[11],
                         b[12], b[13], b[14], b[15],
                         b[16], b[17], b[18], b[19],
                         b[28], b[29], b[30], b[31], b[32], b[33],
                         p[6], p[7], p[8], p[9], p[10], p[11]);
                }
            }
        }

      /* 2026-11-05：ARP 帧取证（只打 0x0806）。
       * 真机：DHCP(广播) 能通，ARP(广播) 2 秒无应答 -> arp_send 以 ENETUNREACH 终止。
       * 这里把请求帧逐字段摊开：eth | arp{htype,ptype,hlen,plen,op,sha,spa,tha,tpa}
       */

      if (p[12] == 0x08 && p[13] == 0x06 && dev->d_len >= 42)
        {
          syslog(LOG_ERR,
                 "[ARP-CHK] eth dst=%02x:%02x:%02x:%02x:%02x:%02x src=%02x:%02x:%02x:%02x:%02x:%02x | "
                 "htype=%04x ptype=%04x hlen=%02x plen=%02x op=%04x | "
                 "sha=%02x:%02x:%02x:%02x:%02x:%02x spa=%u.%u.%u.%u | "
                 "tha=%02x:%02x:%02x:%02x:%02x:%02x tpa=%u.%u.%u.%u\n",
                 p[0], p[1], p[2], p[3], p[4], p[5],
                 p[6], p[7], p[8], p[9], p[10], p[11],
                 (unsigned)((p[14] << 8) | p[15]),
                 (unsigned)((p[16] << 8) | p[17]),
                 p[18], p[19],
                 (unsigned)((p[20] << 8) | p[21]),
                 p[22], p[23], p[24], p[25], p[26], p[27],
                 p[28], p[29], p[30], p[31],
                 p[32], p[33], p[34], p[35], p[36], p[37],
                 p[38], p[39], p[40], p[41]);
        }

      /* 取证：把 src/dst MAC 也打出来。
       *
       * 纯 RPC 模式下，主机帧的源 MAC 就是空口上的发送地址 —— 若它与从机的
       * STA MAC 不一致，AP 会丢弃全部帧（症状正是"发得出去、收不回来"）。
       */

      EH_TRACE(
             "%s: TX %u B type=%04x dst=%02x:%02x:%02x:%02x:%02x:%02x "
             "src=%02x:%02x:%02x:%02x:%02x:%02x\n", NETDEV_TAG,
             (unsigned)dev->d_len,
             (((unsigned)p[12] << 8) | (unsigned)p[13]),
             p[0], p[1], p[2], p[3], p[4], p[5],
             p[6], p[7], p[8], p[9], p[10], p[11]);
    }

  {
    esp_err_t trc = priv->txfn(priv->channel->api_chan, dev->d_buf, dev->d_len);

    if (trc != ESP_OK)
      {
        syslog(LOG_ERR, "[TX-CHK] txfn FAILED rc=%d len=%u\n",
               (int)trc, (unsigned)dev->d_len);
      }
  }

  if (0)
    {
      NETDEV_TXERRORS(dev);
      dev->d_len = 0;
      return 1;
    }

  NETDEV_TXPACKETS(dev);
  dev->d_len = 0;
  return 0;
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
        EH_NETDEV_TRACE("%s: rx frame len=%u:%s\n", NETDEV_TAG,
               (unsigned)len, line);
      }
  }

  if (priv->bifup && buf != NULL && len > 0 && len <= HOSTED_NETDEV_BUFSIZE &&
      len >= NET_LL_HDRLEN(&priv->dev))
    {
      FAR struct eth_hdr_s *eth;

      /* 取证：RX 侧到底有没有帧进来。
       * MAC 是从收到的第一帧里学来的，学不到就说明 RX 没通 ——
       * 而 AP 会丢弃"未知站点"发来的所有帧，表现为 ARP 请求有去无回。
       */

      {
        /* rp 只给 EH_TRACE 用；trace 关掉时连它一起消失（否则 unused）。 */

#if defined(CONFIG_ESPRESSIF_HOSTED_LOG_VERBOSE)
        FAR uint8_t *rp = (FAR uint8_t *)buf;
#endif

        EH_TRACE("%s: RX %u B type=%04x\n", NETDEV_TAG, (unsigned)len,
               (((unsigned)rp[12] << 8) | (unsigned)rp[13]));
      }

      /* Flush a pending transmit first: d_buf/d_len are shared between the two
       * directions in this netdev model.
       *
       * The stack stages an outgoing frame in d_buf/d_len and then asks the
       * driver to send it (d_txavail).  If a received frame lands in between,
       * copying it into d_buf destroys the staged frame -- and the driver then
       * sees an empty packet when it is finally asked to transmit, which is
       * exactly what the trace showed: "txavail len=0" and a broadcast send
       * that never completed (appendix BY.26).  Sending the pending frame here
       * keeps the single buffer coherent.
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

      /* Adopt the station's own MAC from the first frame addressed to it.
       *
       * The radio is in the companion, so the address that is on the air is the
       * companion's; the port starts with a locally administered placeholder
       * because no RPC getter exposes it (the StaConnected event carries only
       * the AP's BSSID).  A frame the AP sends to this station carries that MAC
       * as its destination, so it can be learnt here -- and it has to be, or the
       * AP drops everything this interface transmits as coming from an unknown
       * station (appendix BY.27).
       */

      if (!priv->mac_learned && (eth->dest[0] & 0x01) == 0)
        {
          memcpy(priv->dev.d_mac.ether.ether_addr_octet, eth->dest,
                 sizeof(priv->dev.d_mac.ether.ether_addr_octet));
          priv->mac_learned = true;

          EH_INFO( "%s: station MAC learnt %02x:%02x:%02x:%02x:%02x:%02x\n",
                 NETDEV_TAG, eth->dest[0], eth->dest[1], eth->dest[2],
                 eth->dest[3], eth->dest[4], eth->dest[5]);
        }


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
          /* Not IPv4/ARP: say so, with the bytes.
           *
           * The companion's network-up message -- the one that carries the DHCP
           * address -- goes out as a *data channel* message (its log tags it
           * event_id=0) and is not an Ethernet frame, so it lands here.  Printing
           * it is how its shape gets identified instead of silently dropped
           * (appendix BY.27).
           */

          static int other_trace;

          if (other_trace < 8)
            {
              char line[3 * 16 + 1];
              unsigned k;
              unsigned n = len < 16 ? (unsigned)len : 16;

              other_trace++;

              for (k = 0; k < n; k++)
                {
                  snprintf(&line[k * 3], 4, " %02x",
                           ((FAR const unsigned char *)priv->dev.d_buf)[k]);
                }

              line[n * 3] = '\0';
              EH_TRACE("%s: non-IP frame len=%u type=%04x:%s\n",
                     NETDEV_TAG, (unsigned)len, (unsigned)eth->type, line);
            }

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

/* devif_poll() is how a driver asks the network stack to hand it anything it
 * has queued.  Its declaration lives in net/devif/devif.h, which is internal to
 * the net stack and not on this target's include path (the arch build sees the
 * public headers only), so it is declared here with the callback type spelled
 * out -- devif_poll_callback_t is the same function-pointer type.
 */

int devif_poll(FAR struct net_driver_s *dev,
               CODE int (*callback)(FAR struct net_driver_s *dev));

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

  EH_TRACE("%s: ioctl cmd=0x%04x (SIOCSIWSCAN=0x%04x)\n",
           NETDEV_TAG, (unsigned)cmd, (unsigned)SIOCSIWSCAN);

  switch (cmd)
    {
      case SIOCSIWSCAN:
        {
          /* Blocking scan: the caller expects the table to be ready when this
           * returns, and esp-hosted's RPC carries the result back with it.
           */

          uint16_t count = 0;
          wifi_scan_config_t scan_cfg;
          esp_err_t mret;

          /* 扫描前 best-effort 把从机拉回 STA 模式。
           *
           * 实测（真机 2026-11-02 开机日志）：`RPC wifi_set_mode(STA) -> -1`，
           * 而从机不在 STA 模式时 esp_wifi_scan_start() 不给结果（BY.18 记录过同一现象：
           * "scan 前再发一次 set_mode(STA) 并回读"）。这一步失败也不致命，继续扫。
           */

          mret = eh_host_wifi_set_mode(WIFI_MODE_STA);
          EH_INFO_OR_WARN(mret == ESP_OK,
                          "%s: pre-scan set_mode(STA) -> %d\n", NETDEV_TAG,
                          mret);

          g_dbg_wifi_scan_calls++;
          g_dbg_wifi_setmode_rc = (int)mret;
          g_dbg_wifi_mode_rc    = (int)eh_host_wifi_get_mode((wifi_mode_t *)&g_dbg_wifi_mode);

          /* 显式给扫描参数，不用 NULL（"IDF 默认"）。
           *
           * 真机实测（2026-11-02）：cfg=NULL 时扫描 RPC 能正常返回（rc=0、~11s），
           * 但 CP 回报 0 个 AP，而同一位置网线能拿到 192.168.1.x —— 怀疑从机侧把
           * "无配置"当成了空信道表。这里按 wifi_scan_channel_bitmap_t 的语义给
           * 2.4G 全信道：bit0=0（按位图扫）、bit1..bit14 = 信道 1..14。
           */

          memset(&scan_cfg, 0, sizeof(scan_cfg));
          scan_cfg.channel                  = 0;      /* 0 = 按位图扫 */
          scan_cfg.show_hidden              = true;
          scan_cfg.scan_type                = WIFI_SCAN_TYPE_ACTIVE;
          scan_cfg.scan_time.active.min     = 120;
          scan_cfg.scan_time.active.max     = 300;
          scan_cfg.channel_bitmap.ghz_2_channels = 0x7ffe;  /* 信道 1..14 */
          scan_cfg.channel_bitmap.ghz_5_channels = 0;

          ret = eh_host_wifi_scan_start(&scan_cfg, true);
          g_dbg_wifi_scan_rc = (int)ret;
          if (ret != ESP_OK)
            {
              syslog(LOG_ERR, "%s: scan failed: %d (retrying with defaults)\n",
                     NETDEV_TAG, ret);
              ret = eh_host_wifi_scan_start(NULL, true);
              if (ret != ESP_OK)
                {
                  syslog(LOG_ERR, "%s: scan failed: %d\n", NETDEV_TAG, ret);
                  return -EIO;
                }
            }

          ret = eh_host_wifi_scan_get_ap_num(&count);
          g_dbg_wifi_apnum_rc = (int)ret;
          g_dbg_wifi_apnum    = (int)count;
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

          EH_INFO( "%s: scan done, %d AP(s)\n", NETDEV_TAG, g_scan_count);
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
              ev.len = (uint16_t)need;

              if (used + need > room)
                {
                  break;
                }

              memcpy(out + used, &ev, need);
              used += need;

              /* The SSID, which is what a join matches on.
               *
               * WAPI 约定：essid.pointer 是**本缓冲区内的偏移**，名字字节跟在事件
               * 结构后面。原来这里直接塞内核地址，应用侧按偏移规则解析 → 拿到空串。
               */

              {
                size_t         alen = strnlen((FAR const char *)ap->ssid, 32) + 1;
                size_t         elen = IW_EV_LEN(essid);

                memset(&ev, 0, sizeof(ev));
                ev.cmd = SIOCGIWESSID;
                ev.u.essid.pointer = (FAR void *)(uintptr_t)(used + elen);
                ev.u.essid.length  = alen;
                ev.u.essid.flags   = 1;

                /* len 必须**含**内联的名字字节。
                 *
                 * 无线 API 的事件流是一个按 iw_event.len 串起来的链
                 * （wapi 在 wireless.c:316 就是 `stream->current +=
                 * iwe_stream->len;`）。ESSID 事件的约定是把名字直接跟在事件
                 * 结构后面、并让 len 覆盖全部（Linux wext 也是
                 * `iwe.len = IW_EV_LEN(essid) + len`）。
                 *
                 * 原来这里写的是 `ev.len = elen`（只含头），名字却在后面多占
                 * alen 字节 —— 解析器下一次前进就落进名字里，整条流从此错位，
                 * 于是 `wapi scan` 明明拿到 593 字节事件流、却解析出 0 个 AP、
                 * 什么都不打印。取证日志：`SIOCGIWSCAN room=4096 used=593
                 * aps=9 -> OK` 之后 wapi 静默返回。
                 */

                ev.len = (uint16_t)(elen + alen);

                if (used + elen + alen > room)
                  {
                    break;
                  }

                memcpy(out + used, &ev, elen);
                memcpy(out + used + elen, ap->ssid, alen);
                used += elen + alen;
              }

              /* 信号强度：应用要显示 dBm，缺了它整条记录没意义。 */

              memset(&ev, 0, sizeof(ev));
              ev.cmd = IWEVQUAL;
              ev.u.qual.level   = (uint8_t)(int8_t)ap->rssi;
              ev.u.qual.qual    = 0;
              ev.u.qual.noise   = 0;
              ev.u.qual.updated = IW_QUAL_LEVEL_UPDATED | IW_QUAL_DBM;
              need = IW_EV_LEN(qual);
              ev.len = (uint16_t)need;

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
              ev.len = (uint16_t)need;

              if (used + need > room)
                {
                  break;
                }

              memcpy(out + used, &ev, need);
              used += need;
            }

          iwr->u.data.length = used;

          /* 取证：wapi 的取结果路径此前是"静默失败"的（WAPI_ERROR 被编成空），
           * 这里把 room / used / 缓存条数 / 返回码打出来，一眼能看出是
           * "没人来取"、"缓冲区太小" 还是 "缓存本来就空"。
           */

          EH_INFO( "%s: SIOCGIWSCAN room=%u used=%u aps=%d -> %s\n",
                 NETDEV_TAG, (unsigned)room, (unsigned)used, (int)g_scan_count,
                 (i < g_scan_count) ? "E2BIG" : "OK");

          /* 装不下时必须回 -E2BIG，不能回 OK。
           *
           * 无线 API 的约定是：SIOCGIWSCAN 的结果放不下就返回 E2BIG，调用方据此
           * 知道"结果已就绪、只是缓冲区太小"，然后换大缓冲区重取。
           *
           * wapi 就是这么用的 —— 它的 wapi_scan_stat() 先拿一个 1 字节缓冲
           * （u.data.length = 0）试一次，期望拿到 E2BIG。原来这里无论装没装下都
           * 只 break 然后 return OK（length = 0），wapi 便判定"扫描完成、零个 AP"，
           * 于是 `wapi scan` 明明驱动里缓存了 15 个 AP，却什么都不打印。
           *
           * 判据：循环因装不下而 break 时 i < g_scan_count；正常跑完则相等。
           */

          if (i < g_scan_count)
            {
              return -E2BIG;
            }

          return OK;
        }

      /* 2026-11-05：必须接受 SIOCSIWAUTH / SIOCGIWAUTH。
       *
       * wapi 的 `wapi psk` 在设口令之前会先发 IW_AUTH_WPA_VERSION 和
       * IW_AUTH_CIPHER_PAIRWISE（都是 SIOCSIWAUTH）。原来这里没有这一条 ->
       * 返回 -ENOTTY -> wapi 判定认证参数设置失败，**直接跳过**后续的
       * SIOCSIWENCODEEXT -> 口令永远进不了驱动。
       * 真机现象完全吻合：`joining ... psk_len=0`，且日志里连 `encode cmd=` 都不出现
       * （而 DeskMate 不走这步，所以它的 encode 成功）。
       */

      case SIOCSIWAUTH:
      case SIOCGIWAUTH:
        {
          EH_INFO( "%s: auth param accepted (cmd=0x%04x)\n",
                 NETDEV_TAG, (unsigned)cmd);
          return OK;
        }

      case SIOCSIWENCODE:
      case SIOCSIWENCODEEXT:
        {
          /* 密码：先在 ENCODE 里收下，等 ESSID 到齐再一次性 set_config+connect。
           *
           * 之前这个 netdev 根本没实现这两条 ioctl，所以"选网 + 输密码"里的密码
           * 无处可去（应用侧发 SIOCSIWENCODEEXT 只会拿到 ENOSYS）。
           */

          FAR const uint8_t *key = (FAR const uint8_t *)iwr->u.encoding.pointer;
          size_t             klen = iwr->u.encoding.length;

          /* 取证：`wapi psk` 走的是 SIOCSIWENCODEEXT，而真机上 ESSID 那侧看到的
           * 一直是 psk_len=0。这一行能区分两种可能：
           *   - ioctl 根本没到驱动（不会有这行日志）；
           *   - 到了但 pointer/length 是空的（ptr=(nil) 或 len=0）。
           */

          EH_INFO( "%s: encode cmd=0x%04x ptr=%p len=%u\n",
                 NETDEV_TAG, (unsigned)cmd, (FAR void *)key, (unsigned)klen);

          if (cmd == SIOCSIWENCODEEXT && key != NULL &&
              klen >= sizeof(struct iw_encode_ext))
            {
              FAR const struct iw_encode_ext *ext =
                (FAR const struct iw_encode_ext *)key;

              key  = (FAR const uint8_t *)ext + offsetof(struct iw_encode_ext, key);
              klen = ext->key_len;
            }

          if (key == NULL || klen == 0)
            {
              /* 2026-11-05：空 key 不再清掉已存的口令。
               *
               * wapi 的 `wapi psk` 会先/后再发一次不带 key 的 ENCODE（只设标志位），
               * 原来这里把 g_sta_psk 清空 -> 紧接着 ESSID 用 psk_len=0 去连 WPA2 AP
               * -> 认证必然失败。真机现象正是 `joining ... psk_len=0 authmode=2`。 */
              return OK;
              g_dbg_wifi_enc_rc = OK;
              return OK;
            }

          if (klen > sizeof(g_sta_psk) - 1)
            {
              klen = sizeof(g_sta_psk) - 1;
            }

          memcpy(g_sta_psk, key, klen);
          g_sta_psk[klen] = '\0';

          g_dbg_wifi_psk_len = (int)klen;
          g_dbg_wifi_enc_rc  = OK;
          EH_INFO( "%s: passphrase stored (%u bytes)\n", NETDEV_TAG,
                 (unsigned)klen);
          return OK;
        }

      case SIOCSIWAP:
        {
          /* AP（热点）能力实验：**默认不编译**。
           *
           * 实测（2026-11-03）：这台板子的从机固件是 STA 版，
           * `set_mode(WIFI_MODE_AP)` 返回 0 但 `set_config(WIFI_IF_AP)` 返回 -1，
           * 而且随后连"恢复 STA 模式"也失败——从机进入坏状态，之后所有 RPC 都超时，
           * 应用连 `dm_net_refresh()` 都过不去（要重启从机才能恢复）。
           * 所以这个实验只在显式打开 CONFIG_ESPRESSIF_HOSTED_AP_PROBE 时才做。
           */

#ifdef CONFIG_ESPRESSIF_HOSTED_AP_PROBE
          wifi_config_t ap;

          g_dbg_ap_setmode_rc = (int)eh_host_wifi_set_mode(WIFI_MODE_AP);

          memset(&ap, 0, sizeof(ap));
          strlcpy((FAR char *)ap.ap.ssid, "DeskMate-AP", sizeof(ap.ap.ssid));
          strlcpy((FAR char *)ap.ap.password, "deskmate123",
                  sizeof(ap.ap.password));
          ap.ap.ssid_len       = 11;
          ap.ap.channel        = 6;
          ap.ap.max_connection = 4;
          ap.ap.authmode       = WIFI_AUTH_WPA2_PSK;

          g_dbg_ap_setcfg_rc = (int)eh_host_wifi_set_config(WIFI_IF_AP, &ap);

          g_dbg_ap_restore_rc = (int)eh_host_wifi_set_mode(WIFI_MODE_STA);

          EH_INFO( "%s: AP probe mode=%d cfg=%d restore=%d\n",
                 NETDEV_TAG, g_dbg_ap_setmode_rc, g_dbg_ap_setcfg_rc,
                 g_dbg_ap_restore_rc);
          return OK;
#else
          /* 不做实验：直接把"不支持"记进诊断，返回 ENOTSUP。 */

          g_dbg_ap_setmode_rc = DM_DBG_UNSET;
          g_dbg_ap_setcfg_rc  = -ENOTSUP;
          g_dbg_ap_restore_rc = DM_DBG_UNSET;
          return -ENOTSUP;
#endif
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
              /* 空 ESSID = "断开"。
               *
               * 真机需要它：从机固件自带的应用会反复连一个不存在的 SSID，IDF 于是拒绝
               * 一切扫描（`STA is connecting, scan are not allowed!`），主机侧唯一的
               * 取消手段就是这条 WifiDisconnect RPC。原来这里直接 -EINVAL，等于让
               * 应用无法把从机从"连接中"拉出来，scan 永远 0 个 AP。
               */

              ret = eh_host_wifi_disconnect();
              EH_INFO_OR_ERR(ret == ESP_OK,
                             "%s: disconnect -> %d\n", NETDEV_TAG, ret);
              return ret == ESP_OK ? OK : -EIO;
            }

          memset(&cfg, 0, sizeof(cfg));
          memcpy(cfg.sta.ssid, ssid, iwr->u.essid.length);

          if (g_sta_psk[0] != '\0')
            {
              strlcpy((FAR char *)cfg.sta.password, g_sta_psk,
                      sizeof(cfg.sta.password));
            }

          /* 2026-11-05：显式声明加密门槛与 PMF。
           *
           * 从机日志实测：`connect FAILED after 6 tries — reason 210`
           * （NO_AP_FOUND_W_COMPATIBLE_SECURITY）。主机侧确实下发了 ssid 与
           * password（eh_host_wifi_set_config 的 STA 分支会序列化 password），
           * 但这颗 cfg 是 memset 出来的，`threshold.authmode` 恒为 0（OPEN），
           * 而扫描结果里目标 AP 是 auth=3（WPA2-PSK）。把门槛显式写成
           * "WPA 及以上都接受"，并声明 PMF 可选，去掉这一层歧义。
           */

          cfg.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
          cfg.sta.pmf_cfg.capable    = true;
          cfg.sta.pmf_cfg.required   = false;

          ret = eh_host_wifi_set_config(WIFI_IF_STA, &cfg);
          g_dbg_wifi_setcfg_rc = (int)ret;
          if (ret != ESP_OK)
            {
              syslog(LOG_ERR, "%s: set_config failed: %d\n", NETDEV_TAG, ret);
              return -EIO;
            }

          ret = eh_host_wifi_connect();
          g_dbg_wifi_connect_rc = (int)ret;
          if (ret != ESP_OK)
            {
              syslog(LOG_ERR, "%s: connect failed: %d\n", NETDEV_TAG, ret);
              return -EIO;
            }

          EH_INFO(
                 "%s: joining \"%.*s\" psk_len=%u authmode=%d\n", NETDEV_TAG,
                 (int)iwr->u.essid.length, (FAR const char *)ssid,
                 (unsigned)strnlen(g_sta_psk, sizeof(g_sta_psk)),
                 (int)cfg.sta.threshold.authmode);

          /* 2026-11-05：注册时若没取到 STA MAC，这里再补一次。
           *
           * 走到这一步说明 RPC 一定可用（关联请求本身就是经 RPC 发出的）。
           * 源 MAC 不对的话，AP 会丢弃我们所有的帧 —— 这正是"发得出去、
           * 收不回来"的根因，所以必须在 DHCP/ARP 之前拿到真值。
           */

          if (!g_hosted_netdev.mac_learned)
            {
              uint8_t lm[6];

              if (eh_host_sys_get_mac(WIFI_IF_STA, lm) == ESP_OK)
                {
                  memcpy(dev->d_mac.ether.ether_addr_octet, lm,
                         sizeof(dev->d_mac.ether.ether_addr_octet));
                  g_hosted_netdev.mac_learned = true;

                  EH_INFO(
                         "%s: station MAC from companion (late) "
                         "%02x:%02x:%02x:%02x:%02x:%02x\n", NETDEV_TAG,
                         lm[0], lm[1], lm[2], lm[3], lm[4], lm[5]);
                }
            }

          /* 2026-11-05：纯 RPC 从机不跑 DHCP —— 现在 IP 栈归主机。
           *
           * 换成 `examples/wifi/sta/cp` 固件后，从机只做射频收发，不再有
           * `DhcpDnsStatus` 事件；而板上的 `netinit` 只在开机时跑一次 DHCP，
           * 那时还没关联，必然失败（app 里那句 `DHCP on wlan0 failed` 同理）。
           * 所以在这里、关联请求发出后，由主机自己拉一次地址。
           *
           * 关联需要几百毫秒（真机实测 500 ms），先等 2 s 再请求；
           * 拿到的地址/掩码/网关交给已有的 apply_ip()，它顺手装 DNS 和默认路由。
           */

          {
            struct dhcpc_state ds;
            int                attempt;

            /* 关联本身只要几百毫秒，但 DHCP 需要 AP 侧 4-way 完成后再来，
             * 所以这里重试 3 次、每次间隔 2 s，并把 errno 打出来区分
             * "open 失败 / 请求超时 / 收到 NAK"。
             */

            for (attempt = 1; attempt <= 3; attempt++)
              {
                FAR void *dhcpc;

                /* 2026-11-05：等关联真正完成（轮询），而不是盲等 2 秒。
                 *
                 * DHCP 的 DISCOVER 只有在关联完成后才会被 AP 应答；固定延时既可能
                 * 太短（关联还没完成 -> 无 OFFER），也可能白等。
                 * get_ap_info 成功即视为已关联，同时留下确凿日志。
                 */

                {
                  wifi_ap_record_t ap;
                  int              w;

                  for (w = 0; w < 40; w++)
                    {
                      memset(&ap, 0, sizeof(ap));
                      if (eh_host_wifi_sta_get_ap_info(&ap) == ESP_OK)
                        {
                          EH_INFO(
                                 "%s: ASSOCIATED \"%.32s\" rssi=%d (%d00 ms)\n",
                                 NETDEV_TAG, (const char *)ap.ssid,
                                 (int)ap.rssi, w);
                          break;
                        }

                      usleep(100000);
                    }

                  if (w >= 40)
                    {
                      syslog(LOG_WARNING,
                             "%s: NOT associated after 4s; DHCP will likely fail\n",
                             NETDEV_TAG);
                    }
                }

                /* 2026-11-05：必须把真实 STA MAC 传给 dhcpc！
               *
               * dhcpc_open(ifname, macaddr, maclen) 的第二个参数是**原始 6 字节
               * MAC**，它会被原样填进 BOOTP 的 chaddr。原来这里传 NULL/0 ->
               * chaddr = 00:00:00:00:00:00 -> DHCP 服务器无法识别客户端 ->
               * 永远收不到 OFFER（真机取证：`DHCP pkt: chaddr=00:00:...:00
               * ethsrc=10:bd:a3:89:70:d0`）。
               */

              dhcpc = dhcpc_open("wlan0", dev->d_mac.ether.ether_addr_octet, 6);
                if (dhcpc == NULL)
                  {
                    syslog(LOG_ERR, "%s: dhcpc_open failed (errno=%d)\n",
                           NETDEV_TAG, errno);
                    continue;
                  }

                if (dhcpc_request(dhcpc, &ds) == OK)
                  {
                    EH_INFO(
                           "%s: DHCP got %u.%u.%u.%u nm %u.%u.%u.%u gw %u.%u.%u.%u "
                           "(attempt %d)\n", NETDEV_TAG,
                           (unsigned)(ntohl(ds.ipaddr.s_addr) >> 24) & 0xff,
                           (unsigned)(ntohl(ds.ipaddr.s_addr) >> 16) & 0xff,
                           (unsigned)(ntohl(ds.ipaddr.s_addr) >> 8) & 0xff,
                           (unsigned)ntohl(ds.ipaddr.s_addr) & 0xff,
                           (unsigned)(ntohl(ds.netmask.s_addr) >> 24) & 0xff,
                           (unsigned)(ntohl(ds.netmask.s_addr) >> 16) & 0xff,
                           (unsigned)(ntohl(ds.netmask.s_addr) >> 8) & 0xff,
                           (unsigned)ntohl(ds.netmask.s_addr) & 0xff,
                           (unsigned)(ntohl(ds.default_router.s_addr) >> 24) & 0xff,
                           (unsigned)(ntohl(ds.default_router.s_addr) >> 16) & 0xff,
                           (unsigned)(ntohl(ds.default_router.s_addr) >> 8) & 0xff,
                           (unsigned)ntohl(ds.default_router.s_addr) & 0xff,
                           attempt);

                    (void)esp_hosted_netdev_apply_ip(
                           (FAR const uint8_t *)&ds.ipaddr.s_addr,
                           (FAR const uint8_t *)&ds.netmask.s_addr,
                           (FAR const uint8_t *)&ds.default_router.s_addr);

                    dhcpc_close(dhcpc);
                    break;
                  }

                syslog(LOG_ERR, "%s: DHCP attempt %d failed (errno=%d)\n",
                       NETDEV_TAG, attempt, errno);
                dhcpc_close(dhcpc);
              }
          }
          return OK;
        }

      case SIOCGIWAP:
        {
          /* 已关联 AP 的 BSSID。
           *
           * 应用靠它判断"真的连上了没有"——SIOCGIWESSID 回读的是主机刚写进去的
           * 配置值，未关联时也非空，会造成假阳性（实测踩过）。
           */

          wifi_ap_record_t ap;

          memset(&ap, 0, sizeof(ap));

          if (eh_host_wifi_sta_get_ap_info(&ap) != ESP_OK)
            {
              return -ENODATA;
            }

          if (iwr->u.ap_addr.sa_data == NULL)
            {
              return -EINVAL;
            }

          memcpy(iwr->u.ap_addr.sa_data, ap.bssid, 6);
          iwr->u.ap_addr.sa_family = AF_INET;

          g_dbg_wifi_rssi = (int)ap.rssi;
          return OK;
        }

      case SIOCGIWSTATS:
        {
          /* 信号强度（dBm）。UI 上要显示 dBm，标准读法就是这条。 */

          wifi_ap_record_t ap;

          memset(&ap, 0, sizeof(ap));

          if (eh_host_wifi_sta_get_ap_info(&ap) != ESP_OK)
            {
              return -ENODATA;
            }

          iwr->u.qual.level   = (uint8_t)(int8_t)ap.rssi;
          iwr->u.qual.qual    = 0;
          iwr->u.qual.noise   = 0;
          iwr->u.qual.updated = IW_QUAL_LEVEL_UPDATED | IW_QUAL_DBM;

          g_dbg_wifi_rssi = (int)ap.rssi;
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
  EH_INFO( "%s: wlan0 up\n", NETDEV_TAG);
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
 * Name: esp_hosted_netdev_carrier
 *
 * Description:
 *   Tell NuttX the link is up (or down).
 *
 *   IFF_UP alone is not enough for the stack to pick wlan0 as the outgoing
 *   interface: netdev_default() -- what a broadcast send falls back to --
 *   requires IFF_UP *and* IFF_RUNNING, and IFF_RUNNING is what netdev_carrier_on()
 *   sets.  Measured without it: a broadcast bound to wlan0 fails with
 *   EHOSTUNREACH (errno 113) and the DHCP client can never send its DISCOVER
 *   (appendix BY.26).
 *
 ****************************************************************************/

void esp_hosted_netdev_carrier(bool up)
{
  FAR struct hosted_netdev_s *priv = &g_hosted_netdev;

  if (up)
    {
      netdev_carrier_on(&priv->dev);
    }
  else
    {
      netdev_carrier_off(&priv->dev);
    }

  EH_INFO( "%s: carrier %s\n", NETDEV_TAG, up ? "on" : "off");
}

/****************************************************************************
 * Name: esp_hosted_netdev_mac_learned
 *
 * Description:
 *   True once the station's own MAC has been captured from a received frame.
 *
 ****************************************************************************/

bool esp_hosted_netdev_mac_learned(void)
{
  return g_hosted_netdev.mac_learned;
}

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

  /* 2026-11-05：接口一 up 就必须带 IFF_RUNNING。
   *
   * DHCP 客户端要在**还没有 IP** 的时候发广播（DISCOVER/REQUEST）。实测这时
   * 如果只有 IFF_UP，协议栈的路由查找不认这个设备，DHCP 直接
   * `DHCP attempt N failed (errno=113 = EHOSTUNREACH)`。
   * （原来只在 apply_ip() 里设 IFF_RUNNING —— 那已经是拿到地址之后了，
   *   对 DHCP 来说太晚；network-split 固件由从机做 DHCP，所以一直没暴露。）
   */

  priv->dev.d_flags |= IFF_UP | IFF_RUNNING;
  priv->dev.d_flags &= ~IFF_NOARP;

  EH_INFO( "%s: wlan0 up (IFF_UP|IFF_RUNNING, ARP on)\n", NETDEV_TAG);
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


/* 2026-11-05：地址应用必须离开"无线 ioctl 的调用栈"。
 *
 * 真机证据：
 *   - shell 里手工 `wapi ip`/`wapi mask`（不持 netdev 锁）  -> ping 立刻通
 *   - apply_ip() 里的同样 ioctl（在 hosted_netdev_ioctl 内部、netdev_lock 已持有）
 *     -> 字段正确、rc=0，但 sendto 一律 ENETUNREACH
 * 原因是 SIOCSIFADDR 内部要发 RTM_NEWADDR 通知，而该通知在持锁状态下无法完成，
 * 协议栈始终不知道这个地址。：
 * 因此 apply_ip() 只登记待应用地址，真正的 ioctl 交给 lpwork 线程执行。
 */

static volatile bool g_apply_in_worker;
static uint32_t      g_pend_ip;
static uint32_t      g_pend_nm;
static uint32_t      g_pend_gw;
static struct work_s g_apply_work;

static void hosted_apply_worker(FAR void *arg)
{
  uint8_t ip[4];
  uint8_t nm[4];
  uint8_t gw[4];

  UNUSED(arg);

  memcpy(ip, &g_pend_ip, 4);
  memcpy(nm, &g_pend_nm, 4);
  memcpy(gw, &g_pend_gw, 4);

  g_apply_in_worker = true;
  (void)esp_hosted_netdev_apply_ip(ip, nm, gw);
  g_apply_in_worker = false;
}

/* 2026-11-05：地址下发改由**独立用户任务**执行。
 *
 * 真机对照证明：同一组 ioctl，由 shell 任务发出立刻生效；由驱动/lpwork 线程
 * 发出则 sendto 一律 ENETUNREACH（此前已排除参数、短路、时序、锁、IFF_FLAGS）。
 * 这里创建一个短命任务（与 NSH 命令同属用户任务上下文）执行同样的动作。
 */

static int hosted_apply_task(int argc, FAR char *argv[])
{
  uint8_t ip[4];
  uint8_t nm[4];
  uint8_t gw[4];

  memcpy(ip, &g_pend_ip, 4);
  memcpy(nm, &g_pend_nm, 4);
  memcpy(gw, &g_pend_gw, 4);

  g_apply_in_worker = true;
  (void)esp_hosted_netdev_apply_ip(ip, nm, gw);
  g_apply_in_worker = false;

  return 0;
}

int esp_hosted_netdev_apply_ip(FAR const uint8_t *ip, FAR const uint8_t *nm,
                               FAR const uint8_t *gw)
{
  /* 2026-11-05：全零地址 = "对端还没就绪"（从机在未关联时持续上报
   * DhcpDnsStatus link=0/ip=0.0.0.0）。原来照单全收 -> 把用户刚设好的
   * 静态地址/默认路由清零 -> ping 立刻 sendto failed: 101。这里直接忽略。
   */

  /* 2026-11-05：如果当前在无线 ioctl 的调用栈里（持有 netdev 锁），
   * 就只登记待应用地址并投递给 lpwork；真正的 ioctl 在 lpwork 线程执行，
   * 那里不持锁，RTM_NEWADDR 通知才能完成。 */

  if (!g_apply_in_worker)
    {
      memcpy((void *)&g_pend_ip, ip, 4);
      memcpy((void *)&g_pend_nm, nm, 4);
      memcpy((void *)&g_pend_gw, gw, 4);

      EH_INFO( "%s: deferring address application to lpwork\n",
             NETDEV_TAG);

      (void)task_create("wlan-apply", 100, 3072, hosted_apply_task, NULL);
      return OK;
    }

  if (ip == NULL || (ip[0] | ip[1] | ip[2] | ip[3]) == 0)
    {
      return OK;
    }

  FAR struct hosted_netdev_s *priv = &g_hosted_netdev;

  if (ip == NULL)
    {
      return -EINVAL;
    }

  /* 2026-11-05（最终根因）：三个地址字段必须是**网络字节序**。
   *
   * 原来用 (ip[0]<<24)|(ip[1]<<16)|… 组合成 0xc0a80109，但在小端机上内存里变成
   * 09 a8 01 c0 —— ARP 请求里的 spa/tpa 因此全部反序。真机取证：
   *   [ARP-CHK] sha=10:bd:a3:89:70:d0 spa=9.1.168.192 tpa=1.1.168.192
   * 即"我要找 1.1.168.192"，该地址不存在，AP 不回 -> arp_send 以 ENETUNREACH
   * 终止 -> ping 全部 sendto failed: 101。
   * ip/nm/gw 三个数组本身就是网络序，直接 memcpy 即可。
   */

  if (ip != NULL)
    {
      memcpy(&priv->dev.d_ipaddr, ip, 4);
    }
  else
    {
      priv->dev.d_ipaddr = 0;
    }

  if (nm != NULL)
    {
      memcpy(&priv->dev.d_netmask, nm, 4);
    }
  else
    {
      priv->dev.d_netmask = 0xffffff00;
    }

  if (gw != NULL)
    {
      memcpy(&priv->dev.d_draddr, gw, 4);
    }
  else
    {
      priv->dev.d_draddr = 0;
    }

  /* 2026-11-05：地址必须**由 SIOCSIFADDR 真正下发**，否则协议栈收不到通知。
   *
   * NuttX 的 SIOCSIFADDR 在"新值与当前 d_ipaddr 相同"时直接 break —— 既不设置
   * 也不发 RTM_NEWADDR 通知。原来本函数先把 d_ipaddr/d_netmask 直接写好再发
   * ioctl -> ioctl 变成空操作 -> 协议栈从未得知该地址 -> DHCP 明明拿到 IP、
   * 默认路由也装了，sendto 却一律 ENETUNREACH。
   * 而手工在 shell 里 `wapi ip`/`wapi mask`（无短路、真正下发）之后 ping 立刻通，
   * 与此完全吻合。这里先清零字段，再把真实值交给 ioctl。
   */

  {
    struct in_addr ip_v;
    struct in_addr nm_v;
    int            ss = socket(AF_INET, SOCK_DGRAM, 0);

    ip_v.s_addr = priv->dev.d_ipaddr;
    nm_v.s_addr = priv->dev.d_netmask;

    priv->dev.d_ipaddr  = 0;
    priv->dev.d_netmask = 0;

    if (ss >= 0)
      {
        struct ifreq ifr_v;

        memset(&ifr_v, 0, sizeof(ifr_v));
        strlcpy(ifr_v.ifr_name, "wlan0", IFNAMSIZ);

        int r1;
        int e1;
        int r2;
        int e2;

        ((FAR struct sockaddr_in *)&ifr_v.ifr_addr)->sin_family = AF_INET;
        ((FAR struct sockaddr_in *)&ifr_v.ifr_addr)->sin_addr   = ip_v;
        r1 = ioctl(ss, SIOCSIFADDR, (unsigned long)(uintptr_t)&ifr_v);
        e1 = errno;

        ((FAR struct sockaddr_in *)&ifr_v.ifr_addr)->sin_addr   = nm_v;
        r2 = ioctl(ss, SIOCSIFNETMASK, (unsigned long)(uintptr_t)&ifr_v);
        e2 = errno;

        /* ioctl 必须照发（它们才是真正在配置接口的动作），回值只喂给下面那条
         * 取证日志；INFO 关掉时它们就没有读者了，显式吃掉以免 -Wunused-but-set。 */

        (void)r1;
        (void)e1;
        (void)r2;
        (void)e2;

        /* 2026-11-05：显式走一次 SIOCSIFFLAGS(IFF_UP)。
         *
         * 真机对照：手工 `wapi ip`/`wapi mask` 之后 ping 立刻通；而 apply_ip()
         * 里同样的地址 ioctl（rc=0、字段一致）之后 ping 一律 ENETUNREACH。
         * 差别在于 netdev_ifup() 只是直接把 d_flags |= IFF_UP，并没有走
         * SIOCSIFFLAGS 的完整"上设备"流程（含 ARP/轮询初始化）。
         */

        ifr_v.ifr_flags = IFF_UP | IFF_BROADCAST | IFF_RUNNING;
        (void)ioctl(ss, SIOCSIFFLAGS, (unsigned long)(uintptr_t)&ifr_v);

        EH_INFO(
               "%s: addr ioctl: SIOCSIFADDR rc=%d e=%d  SIOCSIFNETMASK rc=%d e=%d"
               "  -> d_ipaddr=%08x d_netmask=%08x d_draddr=%08x d_flags=%04x\n",
               NETDEV_TAG, r1, e1, r2, e2,
               (unsigned)priv->dev.d_ipaddr, (unsigned)priv->dev.d_netmask,
               (unsigned)priv->dev.d_draddr, (unsigned)priv->dev.d_flags);

        close(ss);
      }
  }

  /* 2026-11-05：通过接口 ioctl 正式配置地址。
   *
   * 只写 net_driver_s 里的 d_ipaddr/d_netmask/d_draddr 时，协议栈"目的地址是否
   * 可达"的判断拿不到这个接口 —— 真机实测连**同网段**的网关都
   * `sendto failed: 101`（ENETUNREACH），说明问题不在缺默认路由，而在接口本身
   * 没被 IPv4 层当作可用出口。这里改走标准路径 SIOCSIFADDR / SIOCSIFNETMASK。
   */

  {
    int s = socket(AF_INET, SOCK_DGRAM, 0);

    if (s >= 0)
      {
        struct ifreq ifr;

        memset(&ifr, 0, sizeof(ifr));
        strlcpy(ifr.ifr_name, "wlan0", IFNAMSIZ);

        ((FAR struct sockaddr_in *)&ifr.ifr_addr)->sin_family = AF_INET;
        ((FAR struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr =
          priv->dev.d_ipaddr;
        (void)ioctl(s, SIOCSIFADDR, (unsigned long)(uintptr_t)&ifr);

        ((FAR struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr =
          priv->dev.d_netmask;
        (void)ioctl(s, SIOCSIFNETMASK, (unsigned long)(uintptr_t)&ifr);

        close(s);
      }
  }

  netdev_ifup(&priv->dev);

  /* 2026-11-05：接口 up 之后**再下发一次**地址/掩码。
   *
   * 真机实测：第一次 SIOCSIFADDR 发生在接口尚未完全 up 时，协议栈没有真正
   * 接纳该地址 -> "DHCP 拿到 IP、默认路由也装了，但 sendto 一律 ENETUNREACH"；
   * 随后用 `wapi ip`/`wapi mask` 再下一次同样的地址，立刻就能 ping 通
   * （`56 bytes from 192.168.1.1 time=730 ms`）。把这次"二次下发"固化进来，
   * DHCP 之后无需任何手工操作。
   */

  {
    int s2 = socket(AF_INET, SOCK_DGRAM, 0);

    if (s2 >= 0)
      {
        struct ifreq ifr2;

        memset(&ifr2, 0, sizeof(ifr2));
        strlcpy(ifr2.ifr_name, "wlan0", IFNAMSIZ);

        ((FAR struct sockaddr_in *)&ifr2.ifr_addr)->sin_family = AF_INET;
        ((FAR struct sockaddr_in *)&ifr2.ifr_addr)->sin_addr.s_addr =
          priv->dev.d_ipaddr;
        (void)ioctl(s2, SIOCSIFADDR, (unsigned long)(uintptr_t)&ifr2);

        ((FAR struct sockaddr_in *)&ifr2.ifr_addr)->sin_addr.s_addr =
          priv->dev.d_netmask;
        (void)ioctl(s2, SIOCSIFNETMASK, (unsigned long)(uintptr_t)&ifr2);

        close(s2);
      }
  }


  /* 2026-11-05：C6 已改回纯 RPC 的 examples/wifi/sta/cp 固件 —— 这时 wlan0 是
   * 一个**真正的以太网 over WiFi 接口**（主机的以太帧由从机直接发上空中），
   * ARP 必须正常走，所以这里**不再**置 IFF_NOARP（那是 network-split 固件
   * 专用的补丁：CP 自己跑 IP 栈、不桥接二层）。
   */

  priv->dev.d_flags |= IFF_UP | IFF_RUNNING;
  priv->dev.d_flags &= ~IFF_NOARP;
  EH_INFO( "%s: wlan0 flags=%04x (UP|RUNNING, ARP on)\n", NETDEV_TAG,
         (unsigned)priv->dev.d_flags);

  EH_INFO(
         "%s: wlan0 address %u.%u.%u.%u mask %u.%u.%u.%u gw %u.%u.%u.%u "
         "(from the DhcpDnsStatus event)\n",
         NETDEV_TAG, ip[0], ip[1], ip[2], ip[3],
         priv->dev.d_netmask >> 24 & 0xff, (priv->dev.d_netmask >> 16) & 0xff,
         (priv->dev.d_netmask >> 8) & 0xff, priv->dev.d_netmask & 0xff,
         priv->dev.d_draddr >> 24 & 0xff, (priv->dev.d_draddr >> 16) & 0xff,
         (priv->dev.d_draddr >> 8) & 0xff, priv->dev.d_draddr & 0xff);

  /* 2026-11-05：把 DNS 指向对端 DHCP 给出的网关。
   *
   * 原来 DNS 是开机时按 CONFIG 写死的 192.168.1.1，而这块板接入的网段是
   * 192.168.0.x —— 于是 `ping www.baidu.com` 一定卡在域名解析。
   * 家用/办公 AP 绝大多数同时就是 DNS，用网关注册最稳。
   */

  if (gw != NULL && priv->dev.d_draddr != 0)
    {
      struct sockaddr_in dns;

      memset(&dns, 0, sizeof(dns));
      dns.sin_family      = AF_INET;
      dns.sin_port        = htons(53);
      dns.sin_addr.s_addr = priv->dev.d_draddr;

      if (dns_add_nameserver((FAR const struct sockaddr *)&dns,
                             sizeof(dns)) == OK)
        {
          EH_INFO( "%s: DNS nameserver -> %u.%u.%u.%u\n",
                 NETDEV_TAG, gw[0], gw[1], gw[2], gw[3]);
        }
    }

  /* 2026-11-05：装默认路由。
   *
   * 只把 ip/netmask/gateway 写进 net_driver_s 是不够的 —— NuttX 的发包要查
   * 路由表，没有 default route 时 `ping` 会直接 `sendto failed: 101`
   * （ENETUNREACH），DNS 也解析不了。真机实测就是这个：
   *   `PING 223.5.5.5 ... ERROR: sendto failed at seqno 0: 101`
   *   `ping www.baidu.com` -> `ping_gethostip() failed`
   * 这里用 DHCP 事件里的网关补一条 RTF_GATEWAY 的默认路由。
   */

  if (gw != NULL && priv->dev.d_draddr != 0)
    {
      int s = socket(AF_INET, SOCK_DGRAM, 0);

      if (s >= 0)
        {
          struct rtentry rt;

          memset(&rt, 0, sizeof(rt));
          ((FAR struct sockaddr_in *)&rt.rt_dst)->sin_family     = AF_INET;
          ((FAR struct sockaddr_in *)&rt.rt_genmask)->sin_family = AF_INET;
          ((FAR struct sockaddr_in *)&rt.rt_gateway)->sin_family = AF_INET;
          ((FAR struct sockaddr_in *)&rt.rt_gateway)->sin_addr.s_addr =
            priv->dev.d_draddr;
          rt.rt_dev   = (FAR char *)"wlan0";
          rt.rt_flags = RTF_UP | RTF_GATEWAY;

          if (ioctl(s, SIOCADDRT, (unsigned long)(uintptr_t)&rt) == 0)
            {
              EH_INFO( "%s: default route via %u.%u.%u.%u\n",
                     NETDEV_TAG, gw[0], gw[1], gw[2], gw[3]);
            }

          close(s);
        }
    }

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

  /* 2026-11-05：从从机取真实 STA MAC（彻底去掉占位/硬编码）。
   *
   * 空口上用的是**从机的 STA MAC**。主机若拿别的 MAC 当源地址发帧，AP 的网桥
   * 无法把回包投递给一个"不属于该端口的站点"，表现就是我们已经反复看到的
   * "帧发得出去（TX 304/310/42 B）、但没有任何回包（DHCP 无 OFFER、ARP 无应答）"。
   *
   * 原来这里是本地管理占位地址 02:00:00:00:00:01（并靠 RX 学习修正），后来
   * 临时硬编码过 10:bd:a3:89:70:d0。现在改成走官方同源 RPC：
   *   eh_host_sys_get_mac(WIFI_IF_STA, mac)  ->  RPC_ID__Req_GetMACAddress
   * 这正是 ESP-IDF 主机侧的做法（netif 的 MAC 来自 esp_wifi_get_mac()）。
   */

  {
    uint8_t m[6];
    int     attempt;
    bool    got = false;

    /* 重试：本函数在 "transport ready" 之后立刻调用，此时 RPC-ext-v2 的特性
     * 初始化可能还没走完 —— 实测第一次必失败（`eh_host_sys_get_mac failed`），
     * 而紧接着的 `RPC wifi_init -> 0` 却成功，说明只差一点点时间。
     */

    for (attempt = 1; attempt <= 6 && !got; attempt++)
      {
        if (eh_host_sys_get_mac(WIFI_IF_STA, m) == ESP_OK)
          {
            memcpy(priv->dev.d_mac.ether.ether_addr_octet, m,
                   sizeof(priv->dev.d_mac.ether.ether_addr_octet));
            priv->mac_learned = true;
            got = true;

            EH_INFO(
                   "%s: station MAC from companion %02x:%02x:%02x:%02x:%02x:%02x"
                   " (attempt %d)\n", NETDEV_TAG,
                   m[0], m[1], m[2], m[3], m[4], m[5], attempt);
          }
        else
          {
            usleep(300000);
          }
      }

    if (!got)
      {
        /* 失败也必须回填占位值：全零 MAC 的帧是无效站点，AP 必然丢弃。
         * （真实 MAC 会在 TX 路径上持续重试获取，见 hosted_netdev_txpoll。） */

        priv->dev.d_mac.ether.ether_addr_octet[0] = 0x02;
        priv->dev.d_mac.ether.ether_addr_octet[1] = 0x00;
        priv->dev.d_mac.ether.ether_addr_octet[2] = 0x00;
        priv->dev.d_mac.ether.ether_addr_octet[3] = 0x00;
        priv->dev.d_mac.ether.ether_addr_octet[4] = 0x00;
        priv->dev.d_mac.ether.ether_addr_octet[5] = 0x01;

        syslog(LOG_WARNING,
               "%s: eh_host_sys_get_mac failed after 6 tries; placeholder "
               "02:00:00:00:00:01 in use, will retry before TX\n",
               NETDEV_TAG);
      }
  }

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

  EH_INFO( "%s: %s registered (STA channel bound)\n",
         NETDEV_TAG, priv->dev.d_ifname);
  /* 2026-11-05：延迟 1 秒再下发一次地址。
   *
   * 真机线索：手工执行 `wapi ip`/`wapi mask` 后，ping 的**第 1 个包仍然
   * ENETUNREACH、第 2 个包就通**（~1 秒后）。说明协议栈接纳地址/路由存在约
   * 1 秒的延迟，而 DHCP 之后紧接着 ping 必然全失败。
   * 这里等 1 秒再下发一次 ioctl，等价于"手工再敲一遍 wapi ip"，让路由立即生效。
   */

  {
    int s3;

    usleep(1000000);

    s3 = socket(AF_INET, SOCK_DGRAM, 0);
    if (s3 >= 0)
      {
        struct ifreq ifr3;

        memset(&ifr3, 0, sizeof(ifr3));
        strlcpy(ifr3.ifr_name, "wlan0", IFNAMSIZ);

        ((FAR struct sockaddr_in *)&ifr3.ifr_addr)->sin_family = AF_INET;
        ((FAR struct sockaddr_in *)&ifr3.ifr_addr)->sin_addr.s_addr =
          priv->dev.d_ipaddr;
        (void)ioctl(s3, SIOCSIFADDR, (unsigned long)(uintptr_t)&ifr3);

        ((FAR struct sockaddr_in *)&ifr3.ifr_addr)->sin_addr.s_addr =
          priv->dev.d_netmask;
        (void)ioctl(s3, SIOCSIFNETMASK, (unsigned long)(uintptr_t)&ifr3);

        close(s3);
      }
  }


  return OK;
}

#endif /* CONFIG_ESPRESSIF_HOSTED_OFFICIAL */
