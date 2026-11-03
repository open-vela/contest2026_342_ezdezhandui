/****************************************************************************
 * apps/examples/deskmate/src/hal/dm_net.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Network half of the DeskMate HAL: see dm_net.h.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <time.h>
#include <netutils/netlib.h>
#include <netutils/dhcpc.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined(CONFIG_NET) && defined(CONFIG_LIBC_NETDB)
#  include <ifaddrs.h>
#endif

#include <arpa/inet.h>

#if defined(CONFIG_NET) && defined(CONFIG_NETDEV_WIRELESS_IOCTL)
#  include <nuttx/wireless/wireless.h>
#  define DM_HAVE_WAPI 1
#  define IW_EVENT_HEADER offsetof(struct iw_event, u)
#endif

#include "dm_log.h"
#include "hal/dm_net.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct dm_net_status_s g_status;
static int g_sock = -1;
static int g_wifi_last_errno;

/* 异步扫描状态（工作线程写、UI 线程读，全部走这把锁） */

enum
{
  DM_SCAN_IDLE = 0,
  DM_SCAN_RUNNING,
  DM_SCAN_DONE,
  DM_SCAN_FAIL
};

/* 连接/热点实验的诊断（非 static：JTAG 可读） */

int  g_app_wifi_connect_rc = -999;   /* -999 = 该路径没跑到（哨兵） */
int  g_app_wifi_link_ok;
int  g_app_wifi_link_rssi;
int  g_app_wifi_dhcp_rc = -999;
char g_app_wifi_ip[24];

static pthread_mutex_t g_scan_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_scan_state = DM_SCAN_IDLE;
static int g_scan_count;
static int g_scan_err;
static struct dm_wifi_ap_s g_scan_aps[DM_NET_MAX_APS];
static bool g_refreshed;

/* 网络刷新工作线程：getifaddrs() 与 WAPI ioctl 都可能阻塞好几秒（真机实测网络页
 * 单轮 UI 忙时 3.9 秒），所以绝不能放在 UI 线程里。UI 只发请求、只读快照。
 */



/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int dm_net_socket(void)
{
  if (g_sock < 0)
    {
      g_sock = socket(AF_INET, SOCK_DGRAM, 0);
      if (g_sock < 0)
        {
          DM_ERR("net: socket() failed: %d", errno);
        }
    }

  return g_sock;
}

static struct dm_net_if_s *dm_net_find_if(const char *name)
{
  int i;

  for (i = 0; i < g_status.count; i++)
    {
      if (strcmp(g_status.ifs[i].name, name) == 0)
        {
          return &g_status.ifs[i];
        }
    }

  if (g_status.count >= DM_NET_MAX_IFS)
    {
      return NULL;
    }

  struct dm_net_if_s *entry = &g_status.ifs[g_status.count++];

  memset(entry, 0, sizeof(*entry));
  strlcpy(entry->name, name, sizeof(entry->name));

  return entry;
}

static void dm_net_fill_flags(struct dm_net_if_s *entry)
{
#if defined(CONFIG_NET)
  struct ifreq ifr;

  if (dm_net_socket() < 0)
    {
      return;
    }

  memset(&ifr, 0, sizeof(ifr));
  strlcpy(ifr.ifr_name, entry->name, IFNAMSIZ);

  if (ioctl(g_sock, SIOCGIFFLAGS, (unsigned long)&ifr) == 0)
    {
      entry->up      = (ifr.ifr_flags & IFF_UP) != 0;
      entry->running = (ifr.ifr_flags & IFF_RUNNING) != 0;

      /* A control panel is also the place where a link gets raised: bring the
       * interface up when the operator can see it is down.  Best effort.
       */

      if (!entry->up)
        {
          ifr.ifr_flags |= IFF_UP;

          if (ioctl(g_sock, SIOCSIFFLAGS, (unsigned long)&ifr) == 0)
            {
              entry->up = true;
              DM_LOG("net: %s brought up", entry->name);
            }
        }

      if (!entry->running && entry->wireless)
        {
          entry->running = entry->up;
        }
    }

  /* A wireless interface answers SIOCGIWNAME; that is the portable probe. */

#ifdef DM_HAVE_WAPI
  {
    struct iwreq iwr;

    memset(&iwr, 0, sizeof(iwr));
    strlcpy(iwr.ifr_name, entry->name, IFNAMSIZ);

    if (ioctl(g_sock, SIOCGIWNAME, (unsigned long)&iwr) == 0)
      {
        entry->wireless = true;
      }
    else if (strncmp(entry->name, "wl", 2) == 0)
      {
        /* esp_hosted 的 netdev 只实现了 SIOCGIWSCAN / ESSID / FREQ 这几条，
         * 没实现 SIOCGIWNAME —— 只看这一条会把真无线网卡判成"没有无线"。
         * 名字前缀兜底（wlan0 / wl0），与常见工具的做法一致。
         */

        entry->wireless = true;

        {
          static bool said;

          if (!said)
            {
              said = true;
              DM_LOG("net: %s treated as wireless by name "
                     "(SIOCGIWNAME unsupported, 只提示一次)", entry->name);
            }
        }
      }
  }
#endif
#endif
}

#ifdef DM_HAVE_WAPI
/* Pick a wireless interface to talk to. */

static const char *dm_net_wifi_ifname(void)
{
  int i;

  for (i = 0; i < g_status.count; i++)
    {
      if (g_status.ifs[i].wireless)
        {
          return g_status.ifs[i].name;
        }
    }

  return NULL;
}

/* The SSID of a scan event is stored either right after the iw_point header
 * (the layout the NuttX WAPI drivers use) or at an offset from the start of
 * the result buffer (the original WE-18 layout).  Accept both.
 */

static const char *dm_wifi_event_ssid(const struct iw_event *ev,
                                      const char *base,
                                      size_t total)
{
  uintptr_t ptr = (uintptr_t)ev->u.essid.pointer;
  size_t len = ev->u.essid.length;

  if (len == 0 || len > DM_NET_SSID_LEN)
    {
      return NULL;
    }

  /* 驱动可能给两种形式：WAPI 规范的"缓冲内偏移"，或直接给数据地址
   * （flat build 下两者都能读）。两种都认，避免又变成空 SSID。
   */

  if (ptr + len <= total)
    {
      return base + ptr;
    }

  if (ptr >= 0x40000000u && ptr < 0x60000000u)
    {
      return (const char *)ptr;
    }

  return NULL;
}
#endif

/* Put the radio into infrastructure (station) mode.  Some drivers refuse to
 * scan until the interface is in the right mode - wifi_sim returns -EINVAL
 * for anything but IW_MODE_INFRA - so this is done before scan and connect.
 */

#ifdef DM_HAVE_WAPI
static void dm_net_wifi_ensure_infra(const char *ifname)
{
  struct iwreq iwr;

  if (ifname == NULL || dm_net_socket() < 0)
    {
      return;
    }

  memset(&iwr, 0, sizeof(iwr));
  strlcpy(iwr.ifr_name, ifname, IFNAMSIZ);
  iwr.u.mode = IW_MODE_INFRA;

  if (ioctl(g_sock, SIOCSIWMODE, (unsigned long)&iwr) < 0)
    {
      DM_LOG("wifi: SIOCSIWMODE(infra) failed: %d", errno);
    }
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int dm_net_refresh(void)
{
  static bool in_refresh;

  /* 重入保护：任何调用方都不可能再把 UI 线程刷爆（实测首页曾经每个 UI tick
   * 调一次、30-50Hz，把控制台灌满后应用阻塞在 printf 上，界面冻住）。
   *
   * 频率节流放在唯一调用点 dm_ui_tick() 里（1s 一次）；这里只挡重入，
   * 不依赖 clock()/时间，避免再引入不可达代码路径。
   */

  if (in_refresh)
    {
      return 0;
    }

  in_refresh = true;

#ifdef DM_NET_TRACE
  DM_LOG("net: refresh step=getifaddrs");
#endif
  memset(&g_status, 0, sizeof(g_status));

#if defined(CONFIG_NET) && defined(CONFIG_LIBC_NETDB)
  {
    struct ifaddrs *list = NULL;
    struct ifaddrs *ifa;

    if (getifaddrs(&list) != 0)
      {
        DM_WARN("net: getifaddrs failed: %d", errno);
        in_refresh = false;
        return -errno;
      }

    for (ifa = list; ifa != NULL; ifa = ifa->ifa_next)
      {
        struct dm_net_if_s *entry;
        struct sockaddr_in *sin;

        if (ifa->ifa_name == NULL || ifa->ifa_addr == NULL)
          {
            continue;
          }

        if (ifa->ifa_addr->sa_family != AF_INET)
          {
            continue;
          }

        if (strcmp(ifa->ifa_name, "lo") == 0)
          {
            continue;
          }

        entry = dm_net_find_if(ifa->ifa_name);
        if (entry == NULL)
          {
            continue;
          }

        dm_net_fill_flags(entry);

        sin = (struct sockaddr_in *)ifa->ifa_addr;
        (void)inet_ntop(AF_INET, &sin->sin_addr, entry->ip,
                        sizeof(entry->ip));

        if (ifa->ifa_netmask != NULL)
          {
            sin = (struct sockaddr_in *)ifa->ifa_netmask;
            (void)inet_ntop(AF_INET, &sin->sin_addr, entry->mask,
                            sizeof(entry->mask));
          }
      }

    freeifaddrs(list);
  }
#endif

  /* Interfaces that exist but have no IPv4 address yet still matter (the UI
   * shows "link up, no address"), so fill their flags in too.
   */

#ifdef DM_HAVE_WAPI
  {
    struct dm_net_if_s *entry = dm_net_find_if("wlan0");

    if (entry != NULL)
      {
        dm_net_fill_flags(entry);
      }
  }
#endif

#ifdef DM_NET_TRACE
  DM_LOG("net: refresh step=summarize");
#endif
  for (int i = 0; i < g_status.count; i++)
    {
      if (g_status.ifs[i].wireless)
        {
          g_status.wireless_present = true;
        }

      if (g_status.ifs[i].up && g_status.ifs[i].ip[0] != '\0' &&
          strcmp(g_status.ifs[i].ip, "0.0.0.0") != 0)
        {
          g_status.online = true;

          if (g_status.primary[0] == '\0')
            {
              strlcpy(g_status.primary, g_status.ifs[i].name,
                      sizeof(g_status.primary));
            }
        }
    }

  /* Prefer the wireless interface as the primary one when it is associated:
   * that is what the product narrative promises.
   */

  if (g_status.wireless_present)
    {
      for (int i = 0; i < g_status.count; i++)
        {
          if (g_status.ifs[i].wireless && g_status.ifs[i].up &&
              g_status.ifs[i].ip[0] != '\0' &&
              strcmp(g_status.ifs[i].ip, "0.0.0.0") != 0)
            {
              strlcpy(g_status.primary, g_status.ifs[i].name,
                      sizeof(g_status.primary));
              break;
            }
        }
    }

  g_refreshed = true;
  in_refresh = false;

  return 0;
}


/* UI 侧只发请求：立刻唤醒工作线程刷一次；线本身每 1 秒也会刷。 */

/* 保留接口名，但**同步**执行。
 *
 * 曾经把它做成后台线程（想把 WAPI ioctl 移出 UI 线程），实测反而更糟：
 * worker 会长时间卡在阻塞 ioctl 里，占着重入标志，导致快照永远发布不出来
 * （自检 net:eth-static 的 if 为空）。见文档 §18 的待办：要真正解决网络页
 * 1.7s 停顿，需要给 ioctl 加超时/取消机制，而不是简单挪线程。
 */

void dm_net_refresh_request(void)
{
  (void)dm_net_refresh();
}

const struct dm_net_status_s *dm_net_status(void)
{
  return g_refreshed ? &g_status : NULL;
}

bool dm_net_wifi_associated(void)
{
#ifdef DM_HAVE_WAPI
  const char *ifname = dm_net_wifi_ifname();
  struct iwreq iwr;

  if (ifname == NULL || dm_net_socket() < 0)
    {
      return false;
    }

  memset(&iwr, 0, sizeof(iwr));
  strlcpy(iwr.ifr_name, ifname, IFNAMSIZ);

  /* 驱动在"已关联"时才会给出 BSSID；未关联返回错误。
   * 这比回读 SIOCGIWESSID（那是我们刚写下去的配置值）可靠得多。
   */

  return ioctl(g_sock, SIOCGIWAP, (unsigned long)&iwr) == 0;
#else
  return false;
#endif
}

int dm_net_wifi_link(char *ssid, size_t ssid_len, int *rssi)
{
#ifdef DM_HAVE_WAPI
  const char *ifname = dm_net_wifi_ifname();
  struct iwreq iwr;
  static char buf[DM_NET_SSID_LEN];

  if (rssi != NULL)
    {
      *rssi = 0;
    }

  if (ifname == NULL || dm_net_socket() < 0)
    {
      return -ENODEV;
    }

  /* 只有真的关联上了才算"已连接"（驱动在未关联时不回 BSSID）。 */

  if (!dm_net_wifi_associated())
    {
      return -ENODATA;
    }

  memset(&iwr, 0, sizeof(iwr));
  strlcpy(iwr.ifr_name, ifname, IFNAMSIZ);
  iwr.u.essid.pointer = buf;
  iwr.u.essid.length  = sizeof(buf);

  if (ioctl(g_sock, SIOCGIWESSID, (unsigned long)&iwr) < 0)
    {
      return -errno;
    }

  if (ssid != NULL && ssid_len > 0 && iwr.u.essid.length > 0)
    {
      size_t len = iwr.u.essid.length;

      if (len >= ssid_len)
        {
          len = ssid_len - 1;
        }

      memcpy(ssid, buf, len);
      ssid[len] = '\0';
    }

  /* 信号强度：标准读法是 SIOCGIWSTATS。 */

  if (rssi != NULL)
    {
      memset(&iwr, 0, sizeof(iwr));
      strlcpy(iwr.ifr_name, ifname, IFNAMSIZ);

      if (ioctl(g_sock, SIOCGIWSTATS, (unsigned long)&iwr) == 0)
        {
          *rssi = (int)(int8_t)iwr.u.qual.level;
        }
    }

  return 0;
#else
  (void)ssid;
  (void)ssid_len;
  (void)rssi;
  return -ENODEV;
#endif
}

static size_t dm_wifi_event_len(uint16_t cmd)
{
  /* 驱动可能不填 ev.len：esp_hosted 的 netdev 在打包 iw_event 时只填了 cmd 和
   * 联合体（实测就是这样，导致应用侧第一个事件就判非法、整张表被丢掉 ——
   * 从机明明扫到 30 个 AP，应用却拿到 0 个）。这里按 cmd 反推长度兜底。
   */

  switch (cmd)
    {
      case SIOCGIWAP:
        return IW_EV_LEN(ap_addr);
      case SIOCGIWESSID:
        return IW_EV_LEN(essid);
      case SIOCGIWFREQ:
        return IW_EV_LEN(freq);
      case SIOCGIWENCODE:
        return IW_EV_LEN(data);
      case IWEVQUAL:
        return IW_EV_LEN(qual);
      default:
        return 0;
    }
}

static int dm_net_wifi_scan_once(struct dm_wifi_ap_s *aps, int max_aps)
{
#ifdef DM_HAVE_WAPI
  const char *ifname = dm_net_wifi_ifname();
  static char scan_buf[2048];
  struct iwreq iwr;
  struct iw_event *ev;
  struct dm_wifi_ap_s *cur = NULL;
  int found = 0;
  size_t off = 0;
  size_t total;

  if (ifname == NULL || dm_net_socket() < 0)
    {
      return -ENODEV;
    }

  if (aps == NULL || max_aps <= 0)
    {
      return -EINVAL;
    }

  memset(aps, 0, sizeof(*aps) * max_aps);
  g_wifi_last_errno = 0;

  dm_net_wifi_ensure_infra(ifname);

  /* Ask for a scan, then read what the driver collected.  The first call only
   * kicks the state machine off; a driver that is already scanning answers
   * with the results on the second call.
   */

  memset(&iwr, 0, sizeof(iwr));
  strlcpy(iwr.ifr_name, ifname, IFNAMSIZ);

  if (ioctl(g_sock, SIOCSIWSCAN, (unsigned long)&iwr) < 0)
    {
      /* 有些驱动（如 esp_hosted 的 netdev）只实现"取结果"不实现"触发扫描"，
       * 触发失败不代表没有结果可用 —— 继续去取，真的没有结果再报错。
       */

      DM_LOG("wifi: scan trigger rc=%d, fetching results anyway", errno);
      g_wifi_last_errno = errno;
    }

  /* Collect the results.  A driver that just started scanning answers
   * -EAGAIN for a moment (wifi_sim advances its state machine one call per
   * ioctl), so retry briefly instead of failing the page.
   */

  {
    int attempt;
    int rc = -EAGAIN;

    for (attempt = 0; attempt < 8; attempt++)
      {
        memset(&iwr, 0, sizeof(iwr));
        strlcpy(iwr.ifr_name, ifname, IFNAMSIZ);
        iwr.u.data.pointer = scan_buf;
        iwr.u.data.length  = sizeof(scan_buf);
        iwr.u.data.flags   = 0;

        rc = ioctl(g_sock, SIOCGIWSCAN, (unsigned long)&iwr);
        if (rc == 0)
          {
            break;
          }

        if (errno != EAGAIN && errno != EINPROGRESS)
          {
            break;
          }

        usleep(20000);
      }

    if (rc < 0)
      {
        g_wifi_last_errno = errno;

        if (errno == E2BIG)
          {
            DM_WARN("wifi: scan list larger than %u bytes",
                    (unsigned)sizeof(scan_buf));
          }
        else
          {
            DM_LOG("wifi: scan results unavailable (%d)", errno);
          }

        return -errno;
      }
  }

  total = iwr.u.data.length;

  /* Event lengths are computed by the driver as IW_EV_LEN(field) =
   * offsetof(u) + sizeof(field), which is *smaller* than sizeof(struct
   * iw_event) for the fixed-size fields (freq/qual/param).  Validating
   * against sizeof() truncates the list after the first access point, so only
   * the two header words are required here.
   */

  while (off + IW_EVENT_HEADER <= total)
    {
      size_t elen;

      ev = (struct iw_event *)(scan_buf + off);

      elen = ev->len;
      if (elen < IW_EVENT_HEADER)
        {
          elen = dm_wifi_event_len(ev->cmd);
        }

      if (elen < IW_EVENT_HEADER || off + elen > total)
        {
          break;
        }

      switch (ev->cmd)
        {
          case SIOCGIWAP:
            /* A new AP starts here. */

            if (cur != NULL)
              {
                found++;
              }

            if (found >= max_aps)
              {
                cur = NULL;
                break;
              }

            cur = &aps[found];
            snprintf(cur->bssid, sizeof(cur->bssid),
                     "%02x:%02x:%02x:%02x:%02x:%02x",
                     (uint8_t)ev->u.ap_addr.sa_data[0],
                     (uint8_t)ev->u.ap_addr.sa_data[1],
                     (uint8_t)ev->u.ap_addr.sa_data[2],
                     (uint8_t)ev->u.ap_addr.sa_data[3],
                     (uint8_t)ev->u.ap_addr.sa_data[4],
                     (uint8_t)ev->u.ap_addr.sa_data[5]);
            break;

          case SIOCGIWESSID:
            if (cur != NULL)
              {
                const char *ssid = dm_wifi_event_ssid(ev, scan_buf, total);
                size_t len = ev->u.essid.length;

                if (ssid != NULL)
                  {
                    if (len >= sizeof(cur->ssid))
                      {
                        len = sizeof(cur->ssid) - 1;
                      }

                    memcpy(cur->ssid, ssid, len);
                    cur->ssid[len] = '\0';
                  }
              }
            break;

          case IWEVQUAL:
            if (cur != NULL)
              {
                cur->rssi = (int)(int8_t)ev->u.qual.level;
              }
            break;

          case SIOCGIWFREQ:
            if (cur != NULL)
              {
                /* wifi_sim reports MHz * 10; other drivers report MHz. */

                int m = ev->u.freq.m;

                cur->freq = m > 100000 ? m / 10 : m;
              }
            break;

          case SIOCGIWENCODE:
            if (cur != NULL)
              {
                cur->secure = (ev->u.data.flags & IW_ENCODE_DISABLED) == 0 &&
                              ev->u.data.length > 0;
              }
            break;

          default:
            break;
        }

      off += elen;
    }

  if (cur != NULL)
    {
      found++;
    }

  /* Drop entries we could not name. */

  {
    int out = 0;

    for (int i = 0; i < found && i < max_aps; i++)
      {
        if (aps[i].ssid[0] != '\0')
          {
            if (out != i)
              {
                aps[out] = aps[i];
              }

            out++;
          }
      }

    found = out;
  }

  DM_LOG("wifi: scan found %d AP(s) on %s", found, ifname);
  return found;
#else
  (void)aps;
  (void)max_aps;
  return -ENODEV;
#endif
}

int dm_net_wifi_connect(const char *ssid, const char *psk)
{
#ifdef DM_HAVE_WAPI
  const char *ifname = dm_net_wifi_ifname();
  struct iwreq iwr;
  int ret;

  if (ifname == NULL || ssid == NULL || ssid[0] == '\0')
    {
      return -ENODEV;
    }

  if (dm_net_socket() < 0)
    {
      return -EIO;
    }

  dm_net_wifi_ensure_infra(ifname);

  if (psk != NULL && psk[0] != '\0')
    {
      /* 注意：NuttX 的 struct iw_encode_ext 里 key[] 是**零长数组**，
       * 所以绝不能拿 sizeof(ext.key) 去 clamp（那是 0，会把密码长度清成 0，
       * 密码根本发不出去 —— 实测真机 psk_len=0 就是这么来的）。
       * 这里用一个"结构体 + 尾部缓冲"的整体，长度按 结构体+密钥 计算。
       */

      static struct
      {
        struct iw_encode_ext ext;
        char                 key[128];
      } enc;

      memset(&enc, 0, sizeof(enc));
      enc.ext.alg     = IW_ENCODE_ALG_CCMP;
      enc.ext.key_len = strlen(psk);

      if (enc.ext.key_len > sizeof(enc.key))
        {
          enc.ext.key_len = sizeof(enc.key);
        }

      memcpy(enc.key, psk, enc.ext.key_len);

      memset(&iwr, 0, sizeof(iwr));
      strlcpy(iwr.ifr_name, ifname, IFNAMSIZ);
      iwr.u.encoding.pointer = &enc.ext;
      iwr.u.encoding.length  = sizeof(enc.ext) + enc.ext.key_len;
      iwr.u.encoding.flags   = IW_ENCODE_INDEX;

      ret = ioctl(g_sock, SIOCSIWENCODEEXT, (unsigned long)&iwr);
      if (ret < 0)
        {
          DM_WARN("wifi: setting the passphrase failed (%d)", errno);
        }
    }

  memset(&iwr, 0, sizeof(iwr));
  strlcpy(iwr.ifr_name, ifname, IFNAMSIZ);
  iwr.u.essid.pointer = (void *)ssid;
  iwr.u.essid.length  = strlen(ssid);
  iwr.u.essid.flags   = 1;

  if (ioctl(g_sock, SIOCSIWESSID, (unsigned long)&iwr) < 0)
    {
      DM_ERR("wifi: SIOCSIWESSID(%s) failed: %d", ssid, errno);
      return -errno;
    }

  DM_LOG("wifi: association requested for '%s'", ssid);
  return 0;
#else
  (void)ssid;
  (void)psk;
  return -ENODEV;
#endif
}

/* 扫描重试。
 *
 * 从机（ESP-Hosted CP）上跑着它自己的示例应用，会在开机后**反复尝试连一个不存在的
 * SSID**；IDF 在关联过程中直接拒绝扫描：
 *
 *     W wifi:sta_scan: STA is connecting, scan are not allowed!
 *     E ehcp_wifi_rpc: req_wifi_scan_start failed [esp_wifi_scan_start(...)] = [12294]
 *
 * 这个重试循环是有限次数（约 18s），所以主机侧**间隔重试**就能等到它空闲。
 * 同一现象在 docs/30_实现/连接_WiFi_SDIO.md 的 BY.19 里有完整记录（重试后扫到 25 个 AP）。
 */

#define DM_SCAN_RETRY_MAX   6
#define DM_SCAN_RETRY_WAIT  (6 * 1000 * 1000)   /* 6s，单位 us */

static int dm_net_wifi_scan_impl(struct dm_wifi_ap_s *aps, int max_aps)
{
  int attempt;
  int found = 0;

  for (attempt = 0; attempt < DM_SCAN_RETRY_MAX; attempt++)
    {
      found = dm_net_wifi_scan_once(aps, max_aps);

      if (found > 0)
        {
          if (attempt > 0)
            {
              DM_LOG("wifi: scan ok on retry #%d (%d AP)", attempt, found);
            }

          return found;
        }

      if (found < 0)
        {
          return found;
        }

      /* 0 个 AP：多半是从机还在关联流程里，等一会儿再来。 */

      DM_LOG("wifi: scan attempt #%d found 0 AP, retrying", attempt + 1);
      usleep(DM_SCAN_RETRY_WAIT);
    }

  return 0;
}

static void *dm_wifi_scan_worker(void *arg)
{
  struct dm_wifi_ap_s aps[DM_NET_MAX_APS];
  int found;

  (void)arg;

  memset(aps, 0, sizeof(aps));
  found = dm_net_wifi_scan_impl(aps, DM_NET_MAX_APS);

  pthread_mutex_lock(&g_scan_lock);
  memcpy(g_scan_aps, aps, sizeof(g_scan_aps));
  g_scan_count = found > 0 ? found : 0;
  g_scan_err   = dm_net_wifi_last_error();
  g_scan_state = found >= 0 ? DM_SCAN_DONE : DM_SCAN_FAIL;
  if (found < 0)
    {
      g_scan_err = -found;
    }

  pthread_mutex_unlock(&g_scan_lock);
  DM_LOG("wifi: scan worker done: found=%d err=%d", found, g_scan_err);
  return NULL;
}

int dm_net_wifi_scan_start(void)
{
  pthread_t tid;
  pthread_attr_t attr;
  int ret;

  pthread_mutex_lock(&g_scan_lock);
  if (g_scan_state == DM_SCAN_RUNNING)
    {
      pthread_mutex_unlock(&g_scan_lock);
      return 0;
    }

  g_scan_state = DM_SCAN_RUNNING;
  g_scan_count = 0;
  g_scan_err   = 0;
  pthread_mutex_unlock(&g_scan_lock);

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&attr, 8192);

  ret = pthread_create(&tid, &attr, dm_wifi_scan_worker, NULL);
  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      pthread_mutex_lock(&g_scan_lock);
      g_scan_state = DM_SCAN_FAIL;
      g_scan_err   = ret;
      pthread_mutex_unlock(&g_scan_lock);
      DM_ERR("wifi: cannot start scan worker: %d", ret);
      return -ret;
    }

  return 0;
}

int dm_net_wifi_scan_status(int *count, int *err)
{
  int state;

  pthread_mutex_lock(&g_scan_lock);
  state = g_scan_state;

  if (count != NULL)
    {
      *count = g_scan_count;
    }

  if (err != NULL)
    {
      *err = g_scan_err;
    }

  pthread_mutex_unlock(&g_scan_lock);

  switch (state)
    {
      case DM_SCAN_RUNNING:
        return 1;
      case DM_SCAN_DONE:
        return 2;
      case DM_SCAN_FAIL:
        return -1;
      default:
        return 0;
    }
}

int dm_net_wifi_scan_results(struct dm_wifi_ap_s *aps, int max_aps)
{
  int count;

  if (aps == NULL || max_aps <= 0)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_scan_lock);
  count = g_scan_count < max_aps ? g_scan_count : max_aps;
  memcpy(aps, g_scan_aps, (size_t)count * sizeof(aps[0]));
  pthread_mutex_unlock(&g_scan_lock);

  return count;
}

int dm_net_wifi_probe(void)
{
  const struct dm_net_status_s *net;
  struct dm_wifi_ap_s aps[DM_NET_MAX_APS];
  char ssid[DM_NET_SSID_LEN];
  int rssi = 0;
  int found;
  int i;

  printf("[dm-wifi-probe] begin\n");

  (void)dm_net_refresh();
  net = dm_net_status();

  if (net == NULL)
    {
      printf("[dm-wifi-probe] no network status\n");
      return -1;
    }

  for (i = 0; i < net->count; i++)
    {
      const struct dm_net_if_s *e = &net->ifs[i];

      printf("[dm-wifi-probe] iface=%s up=%d running=%d wireless=%d ip=%s\n",
             e->name, (int)e->up, (int)e->running, (int)e->wireless, e->ip);
    }

  printf("[dm-wifi-probe] wireless_present=%d\n", (int)net->wireless_present);

  if (dm_net_wifi_link(ssid, sizeof(ssid), &rssi) == 0)
    {
      printf("[dm-wifi-probe] link ssid='%s' rssi=%d\n", ssid, rssi);
    }
  else
    {
      printf("[dm-wifi-probe] link: unavailable\n");
    }

  /* 先断开：CP 自己的 NVS 里存了 SSID（实测在反复尝试连 Hello_World_5G），
   * 卡在关联流程里时扫描可能一直返回 0 个 AP。断开后再扫。
   */

  {
    int drc = dm_net_wifi_disconnect();
    printf("[dm-wifi-probe] disconnect rc=%d\n", drc);
    usleep(500000);
  }

  /* 扫描：起线程 → 最多等 20s（CP 扫描通常 3~10s） */

  (void)dm_net_wifi_scan_start();

  for (i = 0; i < 900; i++)          /* 最多 45s */
    {
      if (dm_net_wifi_scan_status(NULL, NULL) != 1)
        {
          break;
        }

      usleep(50000);
    }

  {
    int count = 0;
    int err = 0;
    int st = dm_net_wifi_scan_status(&count, &err);

    printf("[dm-wifi-probe] scan state=%d count=%d driver_errno=%d waited=%dms\n",
           st, count, err, i * 50);
  }

  found = dm_net_wifi_scan_results(aps, DM_NET_MAX_APS);

  if (found <= 0)
    {
      /* 第二次扫描：第一次常常在"刚断开/刚切信道"的瞬间拿到空表 */

      printf("[dm-wifi-probe] retry scan\n");
      (void)dm_net_wifi_scan_start();

      for (i = 0; i < 900; i++)
        {
          if (dm_net_wifi_scan_status(NULL, NULL) != 1)
            {
              break;
            }

          usleep(50000);
        }

      found = dm_net_wifi_scan_results(aps, DM_NET_MAX_APS);
      printf("[dm-wifi-probe] retry found=%d\n", found);
    }

  for (i = 0; i < found; i++)
    {
      printf("[dm-wifi-probe] ap[%d] ssid='%s' rssi=%d freq=%d secure=%d bssid=%s\n",
             i, aps[i].ssid, aps[i].rssi, aps[i].freq, (int)aps[i].secure,
             aps[i].bssid);
    }

  printf("[dm-wifi-probe] end aps=%d\n", found);
  return found;
}

int dm_net_ap_probe(void)
{
#ifdef DM_HAVE_WAPI
  const char *ifname = dm_net_wifi_ifname();
  struct iwreq iwr;

  if (ifname == NULL || dm_net_socket() < 0)
    {
      return -ENODEV;
    }

  memset(&iwr, 0, sizeof(iwr));
  strlcpy(iwr.ifr_name, ifname, IFNAMSIZ);

  if (ioctl(g_sock, SIOCSIWAP, (unsigned long)&iwr) < 0)
    {
      DM_WARN("wifi: AP probe ioctl failed (%d)", errno);
      return -errno;
    }

  return 0;
#else
  return -ENODEV;
#endif
}

bool dm_net_ipv4_valid(const char *text)
{
  int part = 0;
  int digits = 0;
  int value = 0;

  if (text == NULL)
    {
      return false;
    }

  for (const char *p = text; ; p++)
    {
      if (*p >= '0' && *p <= '9')
        {
          value = value * 10 + (*p - '0');
          digits++;

          if (digits > 3 || value > 255)
            {
              return false;
            }
        }
      else if (*p == '.' || *p == '\0')
        {
          if (digits == 0)
            {
              return false;
            }

          part++;
          digits = 0;
          value  = 0;

          if (*p == '\0')
            {
              break;
            }
        }
      else
        {
          return false;
        }
    }

  return part == 4;
}

int dm_net_if_get_config(const char *ifname, char *ip, size_t ip_len,
                         char *mask, size_t mask_len, char *gw, size_t gw_len)
{
  struct in_addr addr;
  struct in_addr maskaddr;
  struct in_addr gwaddr;

  if (ifname == NULL || ifname[0] == '\0')
    {
      return -EINVAL;
    }

  if (ip != NULL && netlib_get_ipv4addr(ifname, &addr) == OK)
    {
      snprintf(ip, ip_len, "%s", inet_ntoa(addr));
    }

  if (mask != NULL && netlib_get_ipv4netmask(ifname, &maskaddr) == OK)
    {
      snprintf(mask, mask_len, "%s", inet_ntoa(maskaddr));
    }

  if (gw != NULL && netlib_get_dripv4addr(ifname, &gwaddr) == OK)
    {
      snprintf(gw, gw_len, "%s", inet_ntoa(gwaddr));
    }

  return 0;
}

int dm_net_if_set_up(const char *ifname, bool up)
{
  if (ifname == NULL)
    {
      return -EINVAL;
    }

  return up ? netlib_ifup(ifname) : netlib_ifdown(ifname);
}

int dm_net_if_set_static(const char *ifname, const char *ip, const char *mask,
                         const char *gw)
{
  struct in_addr addr;

  if (ifname == NULL || !dm_net_ipv4_valid(ip) || !dm_net_ipv4_valid(mask))
    {
      return -EINVAL;
    }

  /* 每一步单独记 errno：模拟器里能几秒内定位，真机上也能靠日志/诊断区分。 */

  {
    int rc = netlib_ifdown(ifname);

    DM_LOG("net: ifdown(%s) rc=%d errno=%d", ifname, rc, errno);
  }

  if (inet_aton(ip, &addr) == 0)
    {
      return -EINVAL;
    }

  if (netlib_set_ipv4addr(ifname, &addr) != OK)
    {
      DM_ERR("net: set_ipv4addr(%s,%s) failed errno=%d", ifname, ip, errno);
      return -errno;
    }

  if (inet_aton(mask, &addr) == 0)
    {
      return -EINVAL;
    }

  if (netlib_set_ipv4netmask(ifname, &addr) != OK)
    {
      DM_ERR("net: set_ipv4netmask(%s,%s) failed errno=%d", ifname, mask, errno);
      return -errno;
    }

  if (gw != NULL && gw[0] != '\0')
    {
      if (!dm_net_ipv4_valid(gw) || inet_aton(gw, &addr) == 0)
        {
          return -EINVAL;
        }

      if (netlib_set_dripv4addr(ifname, &addr) != OK)
        {
          DM_ERR("net: set_dripv4addr(%s,%s) failed errno=%d", ifname, gw, errno);
          return -errno;
        }
    }

  if (netlib_ifup(ifname) != OK)
    {
      DM_ERR("net: ifup(%s) failed errno=%d", ifname, errno);
      return -errno;
    }

  DM_LOG("net: %s static %s/%s gw=%s", ifname, ip, mask,
         gw != NULL ? gw : "-");
  return 0;
}

int dm_net_if_set_dhcp(const char *ifname)
{
#ifdef CONFIG_NETUTILS_DHCPC
  struct dhcpc_state ds;
  struct ether_addr mac;
  void *handle;
  int ret = -EIO;
  size_t maclen;

  if (ifname == NULL)
    {
      return -EINVAL;
    }

  memset(&ds, 0, sizeof(ds));
  memset(&mac, 0, sizeof(mac));
  maclen = sizeof(mac);

  handle = dhcpc_open(ifname, &mac, maclen);
  if (handle == NULL)
    {
      DM_WARN("net: dhcpc_open(%s) failed", ifname);
      return -EIO;
    }

  if (dhcpc_request(handle, &ds) == OK)
    {
      (void)netlib_set_ipv4addr(ifname, &ds.ipaddr);

      if (ds.netmask.s_addr != 0)
        {
          (void)netlib_set_ipv4netmask(ifname, &ds.netmask);
        }

      if (ds.default_router.s_addr != 0)
        {
          (void)netlib_set_dripv4addr(ifname, &ds.default_router);
        }

      if (ds.dnsaddr.s_addr != 0)
        {
          (void)netlib_set_ipv4dnsaddr(&ds.dnsaddr);
        }

      (void)netlib_ifup(ifname);
      DM_LOG("net: %s got %s from DHCP", ifname, inet_ntoa(ds.ipaddr));
      ret = 0;
    }
  else
    {
      DM_WARN("net: DHCP on %s failed", ifname);
    }

  dhcpc_close(handle);
  return ret;
#else
  (void)ifname;
  return -ENOSYS;
#endif
}

static volatile int g_dhcp_state;
static int          g_dhcp_err;

static void *dm_dhcp_worker(void *arg)
{
  const char *ifname = (const char *)arg;
  int rc;

  rc = dm_net_if_set_dhcp(ifname);

  g_dhcp_err   = rc;
  g_dhcp_state = rc == 0 ? 2 : -1;

  DM_LOG("net: dhcp worker on %s rc=%d", ifname, rc);
  return NULL;
}

int dm_net_if_set_dhcp_async(const char *ifname)
{
  static const char *s_name;
  pthread_t tid;
  pthread_attr_t attr;
  int ret;

  if (ifname == NULL)
    {
      return -EINVAL;
    }

  if (g_dhcp_state == 1)
    {
      return 0;
    }

  s_name       = ifname;
  g_dhcp_state = 1;
  g_dhcp_err   = 0;

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&attr, 8192);

  ret = pthread_create(&tid, &attr, dm_dhcp_worker, (void *)s_name);
  pthread_attr_destroy(&attr);

  if (ret != 0)
    {
      g_dhcp_state = -1;
      g_dhcp_err   = ret;
      return -ret;
    }

  return 0;
}

int dm_net_if_dhcp_status(int *err)
{
  if (err != NULL)
    {
      *err = g_dhcp_err;
    }

  return g_dhcp_state;
}

bool dm_net_ap_supported(void)
{
  /* 见 dm_net.h 的说明：从机是 STA 版固件，AP 配置被拒且会卡死从机 RPC。 */

  return false;
}

int dm_net_ap_start(const char *ssid, const char *psk, int channel)
{
  (void)ssid;
  (void)psk;
  (void)channel;
  return -ENOTSUP;
}

int dm_net_ap_stop(void)
{
  return -ENOTSUP;
}

int dm_net_wifi_wait_link(char *ssid, size_t ssid_len, int *rssi,
                          int timeout_ms)
{
  int waited;

  g_app_wifi_link_ok   = 0;
  g_app_wifi_link_rssi = 0;

  for (waited = 0; waited <= timeout_ms; waited += 500)
    {
      char    name[DM_NET_SSID_LEN];
      int     level = 0;

      if (dm_net_wifi_associated() &&
          dm_net_wifi_link(name, sizeof(name), &level) == 0 &&
          name[0] != '\0')
        {
          g_app_wifi_link_ok   = 1;
          g_app_wifi_link_rssi = level;

          if (ssid != NULL && ssid_len > 0)
            {
              strlcpy(ssid, name, ssid_len);
            }

          if (rssi != NULL)
            {
              *rssi = level;
            }

          DM_LOG("wifi: associated with '%s' (%d dBm) after %d ms", name, level,
                 waited);
          return 0;
        }

      usleep(500000);
    }

  return -ETIMEDOUT;
}

int dm_net_wifi_last_error(void)
{
  return g_wifi_last_errno;
}

int dm_net_wifi_scan(struct dm_wifi_ap_s *aps, int max_aps)
{
  int i;

  if (dm_net_wifi_scan_start() != 0)
    {
      return -EIO;
    }

  for (i = 0; i < 900; i++)          /* 最多 45s：重试 6×6s + 单次扫描时间 */
    {
      if (dm_net_wifi_scan_status(NULL, NULL) != 1)
        {
          break;
        }

      usleep(50000);
    }

  if (dm_net_wifi_scan_status(NULL, NULL) == -1)
    {
      return -dm_net_wifi_last_error();
    }

  return dm_net_wifi_scan_results(aps, max_aps);
}

int dm_net_wifi_disconnect(void)
{
#ifdef DM_HAVE_WAPI
  const char *ifname = dm_net_wifi_ifname();
  struct iwreq iwr;

  if (ifname == NULL || dm_net_socket() < 0)
    {
      return -ENODEV;
    }

  memset(&iwr, 0, sizeof(iwr));
  strlcpy(iwr.ifr_name, ifname, IFNAMSIZ);
  iwr.u.essid.pointer = (void *)"";
  iwr.u.essid.length  = 0;
  iwr.u.essid.flags   = 0;

  if (ioctl(g_sock, SIOCSIWESSID, (unsigned long)&iwr) < 0)
    {
      return -errno;
    }

  return 0;
#else
  return -ENODEV;
#endif
}

int dm_net_describe(char *buf, size_t len)
{
  size_t used = 0;
  int i;

  if (buf == NULL || len == 0)
    {
      return -EINVAL;
    }

  buf[0] = '\0';

  if (!g_refreshed)
    {
      snprintf(buf, len, "not refreshed");
      return 0;
    }

  if (g_status.count == 0)
    {
      snprintf(buf, len, "no interfaces");
      return 0;
    }

  for (i = 0; i < g_status.count; i++)
    {
      const struct dm_net_if_s *e = &g_status.ifs[i];
      int n;

      n = snprintf(buf + used, used < len ? len - used : 0, "%s%s %s%s%s",
                   i > 0 ? "; " : "", e->name,
                   e->up ? (e->running ? "UP" : "up") : "down",
                   e->ip[0] != '\0' ? " " : "", e->ip);

      if (n < 0)
        {
          break;
        }

      used += (size_t)n;

      if (used >= len)
        {
          break;
        }
    }

  return 0;
}
