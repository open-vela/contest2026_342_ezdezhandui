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

/* 请求后台线程刷新（UI 线程调用，立即返回）。 */

void dm_net_refresh_request(void);

/* Last snapshot; NULL before the first dm_net_refresh(). */

const struct dm_net_status_s *dm_net_status(void);

/* Connect state of the wireless interface: SSID and signal, empty when not
 * associated.  Returns 0 when a wireless interface exists, -ENODEV otherwise.
 */

int dm_net_wifi_link(char *ssid, size_t ssid_len, int *rssi);

/* 是否真的关联上了（读已关联 AP 的 BSSID，而不是回读配置的 SSID）。 */

bool dm_net_wifi_associated(void);

/* Trigger a scan and return the access points found.  Returns the number of
 * APs (>= 0) or a negated errno when scanning is unavailable.
 */

int dm_net_wifi_scan(struct dm_wifi_ap_s *aps, int max_aps);

/* 上一次扫描里"驱动侧"的真实错误（0 = 驱动链路正常）：
 * 非 0 表示触发/取结果被驱动拒绝或 RPC 超时（例如 esp_hosted 的扫描 RPC 无响应），
 * 此时返回 0 个 AP 不代表"附近没有 AP"，界面与断言要按"驱动不可用"处理。
 */

int dm_net_wifi_last_error(void);

/* 异步扫描：扫描要走 CP（几秒），不能占着 UI 线程。
 *   start()  → 起一个 detached 线程干活，立刻返回；
 *   status() → 0=空闲/未开始，1=进行中，2=完成，-1=失败；
 *   results()→ 把最近一次结果拷出来（调用方持有 aps 数组）。
 */

int dm_net_wifi_scan_start(void);
int dm_net_wifi_scan_status(int *count, int *err);
int dm_net_wifi_scan_results(struct dm_wifi_ap_s *aps, int max_aps);

/* 一条命令回答"WiFi 有没有问题"：把接口/驱动能力/扫描/AP 列表打到 stdout。 */

int dm_net_wifi_probe(void);

/* 热点能力最小实验：让 netdev 走一次 set_mode(AP)+set_config(AP) 再恢复；
 * 结果由 netdev 记进 g_dbg_ap_*（JTAG 可读）。 */

int dm_net_ap_probe(void);

/* 等关联成功（轮询 SIOCGIWESSID），最多 timeout_ms；成功时给出 SSID/RSSI。 */

int dm_net_wifi_wait_link(char *ssid, size_t ssid_len, int *rssi,
                          int timeout_ms);

/* ── 类 PC 的接口配置（网口自动/手动 IP）───────────────────────────────── */

int dm_net_if_get_config(const char *ifname, char *ip, size_t ip_len,
                         char *mask, size_t mask_len, char *gw, size_t gw_len);
int dm_net_if_set_dhcp(const char *ifname);

/* 异步版：DHCP 要几秒，UI 线程不能等；status 返回 0=空闲 1=进行中 2=成功 -1=失败 */

int dm_net_if_set_dhcp_async(const char *ifname);
int dm_net_if_dhcp_status(int *err);
int dm_net_if_set_static(const char *ifname, const char *ip, const char *mask,
                         const char *gw);
int dm_net_if_set_up(const char *ifname, bool up);

/* 点分十进制校验（UI 层用，避免把错地址写进去导致失联）。 */

bool dm_net_ipv4_valid(const char *text);

/* ── 热点 ──────────────────────────────────────────────────────────────
 *
 * 真机实测（2026-11-03）：从机固件是 STA 版，set_mode(AP)=0 但
 * set_config(WIFI_IF_AP)=-1，且恢复 STA 也失败（还会卡死从机 RPC）。
 * 所以这里如实报告"不支持"，UI 显示原因而不是假装能开。
 */

bool dm_net_ap_supported(void);
int  dm_net_ap_start(const char *ssid, const char *psk, int channel);
int  dm_net_ap_stop(void);

/* 连接诊断结果（非 static，串口坏掉时可用 JTAG 读） */

extern int  g_app_wifi_connect_rc;
extern int  g_app_wifi_link_ok;
extern int  g_app_wifi_link_rssi;
extern int  g_app_wifi_dhcp_rc;
extern char g_app_wifi_ip[24];

/* Associate with an AP.  psk may be NULL/empty for an open network.
 * Returns 0 on success.
 */

int dm_net_wifi_connect(const char *ssid, const char *psk);

/* Disassociate. */

int dm_net_wifi_disconnect(void);

/* One-line summary of the interface list, for the console evidence. */

int dm_net_describe(char *buf, size_t len);

#endif /* __APPS_EXAMPLES_DESKMATE_HAL_DM_NET_H */
