/****************************************************************************
 * apps/examples/deskmate/src/hal/dm_net.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Network half of the DeskMate HAL.
 *
 * Two things are exposed, because the product UI needs both:
 *
 *   ethernet / generic interfaces : name, up/running, IPv4 address.  Read from
 *       getifaddrs() + SIOCGIFFLAGS, so it covers the ESP32-P4 EMAC (eth0),
 *       the simulator TAP device and any other NuttX netdev.
 *
 *   WiFi                          : the WAPI ioctl surface (SIOCGIWESSID,
 *       SIOCGIWSCAN, SIOCSIWESSID, SIOCSIWENCODEEXT).  This is what the
 *       on-board ESP-Hosted path exposes and what the simulator's wifi_sim
 *       pseudo device exposes, so the same UI code drives both.
 *
 * Everything is best effort: a board without WiFi reports "no wireless
 * interface" instead of failing, and the UI degrades to the wired status.
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_DESKMATE_HAL_DM_NET_H
#define __APPS_EXAMPLES_DESKMATE_HAL_DM_NET_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define DM_NET_MAX_IFS   4
#define DM_NET_MAX_IFSZ  8
#define DM_NET_MAX_APS   8
#define DM_NET_SSID_LEN  33

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct dm_net_if_s
{
  char name[DM_NET_MAX_IFSZ];
  bool up;
  bool running;
  bool wireless;
  char ip[20];
  char mask[20];
};

struct dm_wifi_ap_s
{
  char ssid[DM_NET_SSID_LEN];
  char bssid[18];
  int  rssi;                    /* dBm (negative), 0 when unknown */
  int  freq;                    /* MHz, 0 when unknown */
  bool secure;
};

struct dm_net_status_s
{
  int  count;
  struct dm_net_if_s ifs[DM_NET_MAX_IFS];
  char primary[DM_NET_MAX_IFSZ];  /* Interface that carries the address */
  bool online;                    /* Some interface is up with an address */
  bool wireless_present;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Re-read the interface list.  Returns 0 on success. */

int dm_net_refresh(void);

/* Last snapshot; NULL before the first dm_net_refresh(). */

const struct dm_net_status_s *dm_net_status(void);

/* Connect state of the wireless interface: SSID and signal, empty when not
 * associated.  Returns 0 when a wireless interface exists, -ENODEV otherwise.
 */

int dm_net_wifi_link(char *ssid, size_t ssid_len, int *rssi);

/* Trigger a scan and return the access points found.  Returns the number of
 * APs (>= 0) or a negated errno when scanning is unavailable.
 */

int dm_net_wifi_scan(struct dm_wifi_ap_s *aps, int max_aps);

/* Associate with an AP.  psk may be NULL/empty for an open network.
 * Returns 0 on success.
 */

int dm_net_wifi_connect(const char *ssid, const char *psk);

/* Disassociate. */

int dm_net_wifi_disconnect(void);

/* One-line summary of the interface list, for the console evidence. */

int dm_net_describe(char *buf, size_t len);

#endif /* __APPS_EXAMPLES_DESKMATE_HAL_DM_NET_H */
