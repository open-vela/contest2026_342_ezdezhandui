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
#endif

#include "dm_log.h"
#include "hal/dm_net.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct dm_net_status_s g_status;
static int g_sock = -1;
static bool g_refreshed;

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

  if (ptr >= (uintptr_t)sizeof(struct iw_point))
    {
      return (const char *)&ev->u.essid + sizeof(struct iw_point);
    }

  if (ptr + len <= total)
    {
      return base + ptr;
    }

  return NULL;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int dm_net_refresh(void)
{
  memset(&g_status, 0, sizeof(g_status));

#if defined(CONFIG_NET) && defined(CONFIG_LIBC_NETDB)
  {
    struct ifaddrs *list = NULL;
    struct ifaddrs *ifa;

    if (getifaddrs(&list) != 0)
      {
        DM_WARN("net: getifaddrs failed: %d", errno);
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
  return 0;
}

const struct dm_net_status_s *dm_net_status(void)
{
  return g_refreshed ? &g_status : NULL;
}

int dm_net_wifi_link(char *ssid, size_t ssid_len, int *rssi)
{
  if (ssid != NULL && ssid_len > 0)
    {
      ssid[0] = '\0';
    }

  if (rssi != NULL)
    {
      *rssi = 0;
    }

#ifdef DM_HAVE_WAPI
  {
    const char *ifname = dm_net_wifi_ifname();
    struct iwreq iwr;
    char buf[DM_NET_SSID_LEN];

    if (ifname == NULL || dm_net_socket() < 0)
      {
        return -ENODEV;
      }

    memset(buf, 0, sizeof(buf));
    memset(&iwr, 0, sizeof(iwr));
    strlcpy(iwr.ifr_name, ifname, IFNAMSIZ);
    iwr.u.essid.pointer = buf;
    iwr.u.essid.length  = sizeof(buf);
    iwr.u.essid.flags   = 1;

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

    return 0;
  }
#else
  return -ENODEV;
#endif
}

int dm_net_wifi_scan(struct dm_wifi_ap_s *aps, int max_aps)
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

  /* Ask for a scan, then read what the driver collected.  The first call only
   * kicks the state machine off; a driver that is already scanning answers
   * with the results on the second call.
   */

  memset(&iwr, 0, sizeof(iwr));
  strlcpy(iwr.ifr_name, ifname, IFNAMSIZ);

  if (ioctl(g_sock, SIOCSIWSCAN, (unsigned long)&iwr) < 0 &&
      errno != EINPROGRESS && errno != EAGAIN)
    {
      DM_LOG("wifi: scan not supported (%d)", errno);
      return -errno;
    }

  memset(&iwr, 0, sizeof(iwr));
  strlcpy(iwr.ifr_name, ifname, IFNAMSIZ);
  iwr.u.data.pointer = scan_buf;
  iwr.u.data.length  = sizeof(scan_buf);
  iwr.u.data.flags   = 0;

  if (ioctl(g_sock, SIOCGIWSCAN, (unsigned long)&iwr) < 0)
    {
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

  total = iwr.u.data.length;

  while (off + sizeof(struct iw_event) <= total)
    {
      ev = (struct iw_event *)(scan_buf + off);

      if (ev->len < sizeof(struct iw_event) || off + ev->len > total)
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

      off += ev->len;
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

  if (psk != NULL && psk[0] != '\0')
    {
      static struct iw_encode_ext ext;

      memset(&ext, 0, sizeof(ext));
      ext.alg     = IW_ENCODE_ALG_CCMP;
      ext.key_len = strlen(psk);

      if (ext.key_len > sizeof(ext.key))
        {
          ext.key_len = sizeof(ext.key);
        }

      memcpy(ext.key, psk, ext.key_len);

      memset(&iwr, 0, sizeof(iwr));
      strlcpy(iwr.ifr_name, ifname, IFNAMSIZ);
      iwr.u.encoding.pointer = &ext;
      iwr.u.encoding.length  = sizeof(ext);
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
