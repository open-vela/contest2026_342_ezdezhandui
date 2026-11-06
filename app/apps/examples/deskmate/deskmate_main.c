/****************************************************************************
 * apps/examples/deskmate/deskmate_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * DeskMate -- the openvela control-panel application layer.
 *
 * NSH built-in:
 *
 *   nsh> deskmate                     run the UI
 *   nsh> deskmate --selftest          headless scenario + snapshot + report
 *   nsh> deskmate --exit-after 30     stop after 30 seconds
 *   nsh> deskmate --snap /tmp/x.bin   snapshot destination
 *
 * The run is single threaded on purpose: LVGL and the widget tree are only
 * ever touched from the loop below, so there is no cross-thread invalidation
 * to get wrong.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/sched.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/stat.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#if defined(CONFIG_BOARDCTL) && !defined(CONFIG_NSH_ARCHINIT)
#  include <sys/boardctl.h>
#endif

#if defined(CONFIG_FS_HOSTFS)
#  include <sys/mount.h>
#endif

#include <lvgl/lvgl.h>

#include "agent/dm_agent.h"
#include "dm_config.h"
#include "dm_log.h"
#include "dm_selftest.h"
#include "hal/dm_camera.h"
#include "hal/dm_display.h"
#include "hal/dm_net.h"
#include "hal/dm_touch.h"
#include "ui/dm_page.h"
#include "ui/dm_ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Longest sleep between loop iterations: keeps exit conditions and the
 * self-test responsive even when LVGL has nothing to do.
 *
 * 2026-11-06: 40 -> 16。
 *
 * 这个上限不是"省电旋钮"，它直接决定帧周期的下限：面板提交只发生在
 * lv_timer_handler() 里（本工程没有 LVGL render thread），而 usleep 被 10ms
 * 系统 tick 向上取整（CONFIG_USEC_PER_TICK=10000），所以 40ms 的睡眠把主循环
 * 钉在 ~67ms = 15fps（真机 JTAG 实测：提交数 ≡ 循环数，>100ms 帧间隔 58 次/30s）。
 * 16ms 与 dm_display.c 里显式设的 LVGL 刷新周期一致，经 tick 量化后每轮睡 ~20ms。
 *
 * 注意：这只是"别再自己拖后腿"；实际帧率仍受每轮真实绘制耗时约束。
 */

#define DM_MAX_SLEEP_MS 16

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 用法文本：与 README 的「三、命令行参数」一节一一对应。
 * 带 [P4] 的选项只有 ESP32-P4 目标才编进去（模拟器上会报 unknown argument），
 * 带 [PPA] 的需要 CONFIG_ESPRESSIF_PPA。
 */

static const char *g_usage =
  "Usage: " DM_PROGNAME " [options]\n"
  "\n"
  "默认启动（不带参数）：摄像头 + 内置 ai_agent(ws) + wlan0 自动拉起。\n"
  "  取证模式 --selftest/--tour/--wifi-*/--ap-probe/--wifi-connect 不套用这些默认。\n"
  "\n"
  "Run modes:\n"
  "  (no option)         run the UI until killed\n"
  "  --selftest          headless scenario: asserts, snapshots, report, exit\n"
  "  --tour              walk every page and snapshot it (demo, no assertions)\n"
  "  --soak <sec>        cycle pages every <sec> s, log metrics to\n"
  "                      /tmp/deskmate_soak.txt every 30 s (long run)\n"
  "  --exit-after <sec>  leave the UI loop after <sec> seconds\n"
  "  --page <id|index>   start on a page: home|camera|chat|net|settings|0..4\n"
  "\n"
  "Evidence options:\n"
  "  --snap <path>       first snapshot path (default " DM_SNAP_HOST_DIR
                              "/dm_selftest.bin);\n"
  "                      every page is written next to it as dm_<page>.bin\n"
  "                      (a .txt sidecar records width/height/bpp/stride)\n"
  "  --report <path>     also write the [dm-check] report lines to a file\n"
  "  --hostfs <dir>      mount this host directory at /host (simulator only)\n"
  "\n"
  "启动默认值开关（默认全开）：\n"
  "  --no-camera         do not open the camera at startup\n"
  "  --no-agent-spawn    do not start the built-in ai_agent at startup\n"
  "  --no-wlan           do not bring wlan0 up / DHCP at startup\n"
  "\n"
  "Agent bridge:\n"
  "  --agent <mode>      mock | ws | off (default from Kconfig)\n"
  "  --agent-endpoint <host[:port]>  ws backend (default 127.0.0.1:28789)\n"
  "  --agent-spawn       start the built-in ai_agent in an in-process task\n"
  "                      [needs ai_agent in the image + NET_LOOPBACK]\n"
  "  --say <text>        send one message through the bridge and print the\n"
  "                      reply (end-to-end probe: bridge -> backend -> UI)\n"
  "\n"
  "Camera:\n"
  "  --camera            open " DM_CAMERA_PATH " (CSI 1280x720 RAW8 -> RGB565)\n"
  "                      CSI and DSI are not exclusive: the board bring-up runs\n"
  "                      the ISP bypass before MIPI-DSI starts.  Off by default\n"
  "                      only to save power and PSRAM bandwidth.\n"
  "  --cam-fps <n>       throttle the preview to <n> fps (0 = unlimited)\n"
  "  --freeze-preview    stop re-pointing the canvas at new frames (diagnostic)\n"
  "\n"
  "Network:\n"
  "  --wifi-scan         scan wlan0 through the app's WAPI path, print the APs\n"
  "  --wifi-probe        print a WiFi diagnostic report and exit\n"
  "  --wifi-connect <ssid> [<psk>]  associate, then DHCP on wlan0\n"
  "  --ap-probe          exercise the AP-mode ioctl once and exit\n"
  "\n"
  "Display / touch diagnostics:\n"
  "  --disp-reinit       re-run the panel/DSI bring-up once      [P4]\n"
  "  --disp-stall <sec>  freeze the UI loop for <sec> s (watchdog test)\n"
  "  --touch-fake x y    synthesize a hardware press through the touch snapshot\n"
  "  --touch-wr x y      write a real press into the GT911 registers  [P4]\n"
  "  --wifi-start        try the hosted RPC wifi_start (bounded in time) [P4]\n"
  "  --ppa-test          run the PPA (2D accelerator) fill selftest   [PPA]\n"
  "  --help, -h          this text\n";

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static unsigned int dm_now_ms(void)
{
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
      return 0;
    }

  return (unsigned int)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* Mount the host directory the simulator uses to hand snapshots out, when one
 * is configured.  Failure is reported but never fatal: a run without hostfs
 * still produces console evidence.
 */

/* 本次运行是否成功挂上 hostfs（挂上才用 /host 目录放快照）。 */

static bool g_hostfs_mounted;

static void dm_mount_hostfs(const char *path)
{
#if defined(CONFIG_FS_HOSTFS)
  char options[160];

  if (path == NULL || path[0] == '\0')
    {
      return;
    }

  (void)mkdir("/host", 0777);

  snprintf(options, sizeof(options), "fs=%s", path);

  if (mount(NULL, "/host", "hostfs", 0, options) < 0)
    {
      DM_WARN("hostfs mount of %s failed: %d", path, errno);
      return;
    }

  (void)mkdir(DM_SNAP_HOST_DIR, 0777);
  g_hostfs_mounted = true;
  DM_LOG("hostfs: %s mounted at /host", path);
#else
  DM_LOG("hostfs: not enabled in this configuration (%s)",
         path != NULL ? path : "");
#endif
}

/* Simulator convenience: hand the wifi_sim driver a BSS table so the wireless
 * page has access points to list on a host without a radio.  The file format
 * is the one nttx/drivers/net/wifi_sim.c parses:
 *
 *     bssid,freq,signal,security,ssid,password
 */

static void dm_write_wifi_bss(void)
{
  static const char bss[] =
    "02:00:00:00:01:00,2412,-42,[WPA2-PSK],DeskMate-Lab,lab-pass-123\n"
    "02:00:00:00:02:00,2437,-58,[WPA2-PSK],openvela-dev,dev-pass-456\n"
    "02:00:00:00:03:00,2462,-71,[ESS],Guest-Open,\n";

  FILE *fp;

  if (DM_WIFI_BSS[0] == '\0')
    {
      return;
    }

  /* Make sure the containing directory exists: on the simulator the table
   * lives under /host (hostfs), which the guest root filesystem cannot
   * provide by itself.
   */

  {
    char dir[96];
    char *slash;

    strlcpy(dir, DM_WIFI_BSS, sizeof(dir));
    slash = strrchr(dir, '/');

    if (slash != NULL && slash != dir)
      {
        *slash = '\0';

        for (char *q = dir + 1; *q != '\0'; q++)
          {
            if (*q == '/')
              {
                *q = '\0';
                (void)mkdir(dir, 0777);
                *q = '/';
              }
          }

        (void)mkdir(dir, 0777);
      }
  }

  fp = fopen(DM_WIFI_BSS, "w");
  if (fp == NULL)
    {
      DM_WARN("wifi: cannot write the BSS table %s (%d)", DM_WIFI_BSS, errno);
      return;
    }

  (void)fwrite(bss, 1, sizeof(bss) - 1, fp);
  fclose(fp);

  DM_LOG("wifi: BSS table written to %s", DM_WIFI_BSS);
}


/* ---------------------------------------------------------------------------
 * 默认启动：把无线口自己拉起来
 *
 * 板上 wlan0 由 ESP-Hosted 注册，但"注册"不等于"可用"：接口默认是 DOWN，
 * 也没有地址。以前用户得先 `wapi essid wlan0 <ssid>` + `ifup wlan0`，再回
 * deskmate —— 对一块"上电即用"的中控屏来说，这个前置流程本身就是缺陷。
 *
 * 现在默认做两步（都能被 --no-wlan 整体关掉）：
 *   1) ifup：把无线口启用（驱动的 STATION 状态机开始工作）
 *   2) DHCP：异步取地址（几秒，不能占 UI 线程）
 *
 * 关联由 DM_WLAN_SSID 决定：
 *   · 配了 SSID → 先关联再 DHCP（无人值守部署时用 Kconfig 写死即可）
 *   · SSID 为空 → 不主动关联，直接 DHCP（从机固件常常自己已经连上了，
 *                  应用再关联一次只会把它踢下线）
 *
 * 所有失败路径都只是告警：没有无线网卡的镜像（NuttX sim 走 TAP 以太网、
 * 有的板级配置没开 ESP-Hosted）照常启动 UI，网络页显示降级状态。
 * ------------------------------------------------------------------------- */

static void dm_wlan_autostart(const char *ssid, const char *psk)
{
  struct dm_wifi_ap_s aps[DM_NET_MAX_APS];
  const struct dm_net_status_s *net;
  const char *ifname = NULL;
  int rc;
  int i;

  /* 先刷新一次，确保接口表是最新的（wlan0 可能刚被驱动注册）。 */

  (void)dm_net_refresh();

  net = dm_net_status();

  for (i = 0; net != NULL && i < net->count; i++)
    {
      if (net->ifs[i].wireless)
        {
          ifname = net->ifs[i].name;
          break;
        }
    }

  if (ifname == NULL)
    {
      /* 没有无线口不算错误：模拟器只有 TAP 以太网，有的板级配置也没开
       * ESP-Hosted。写一行日志，UI 继续按降级状态走。
       */

      DM_LOG("wlan: no wireless interface, autostart skipped");
      return;
    }

  rc = dm_net_if_set_up(ifname, true);
  DM_LOG("wlan: ifup(%s) rc=%d", ifname, rc);

  if (ssid != NULL && ssid[0] != '\0')
    {
      char linked[DM_NET_SSID_LEN];
      int  rssi = 0;

      /* 已经关联上就别再踢一次：重复 SIOCSIWESSID 会让从机重新握手，
       * 已经拿到的地址也会掉。
       */

      if (dm_net_wifi_link(linked, sizeof(linked), &rssi) == 0)
        {
          DM_LOG("wlan: already associated with '%s' (%d dBm)", linked, rssi);
        }
      else if (dm_net_wifi_connect(ssid, psk) != 0)
        {
          DM_WARN("wlan: associate '%s' failed (errno=%d)", ssid, errno);
        }
      else if (dm_net_wifi_wait_link(linked, sizeof(linked), &rssi, 20000) != 0)
        {
          DM_WARN("wlan: associate '%s' timed out after 20s", ssid);
        }
      else
        {
          DM_LOG("wlan: associated with '%s' (%d dBm)", linked, rssi);
        }
    }
  else
    {
      /* SSID 没配：扫一次只为留证据（网络页本来也要显示 AP 列表）。
       * 扫描要走 CP、可能几秒，失败不致命。
       */

      int found = dm_net_wifi_scan(aps, DM_NET_MAX_APS);

      DM_LOG("wlan: scan before dhcp found=%d err=%d", found,
             found < 0 ? dm_net_wifi_last_error() : 0);
    }

  /* DHCP 异步：UI 线程不能等几秒的 DHCP。结果由网络页轮询
   * dm_net_if_dhcp_status() 显现，启动过程不被阻塞。
   */

  rc = dm_net_if_set_dhcp_async(ifname);
  DM_LOG("wlan: dhcp on %s requested rc=%d", ifname, rc);
}


static void dm_eth_autostart(void)
{
  const struct dm_net_status_s *net;
  const char *ifname = NULL;
  int rc;
  int i;

  /* 先把有线口拉起来并请求 DHCP。
   *
   * 为什么这条比 wlan 更重要：本板的 wlan0 默认 SSID 是空的（真机要连的 AP
   * 由部署时填），而 eth0 是 ESP32-C6 伴生芯片透传的以太网 —— 插上就通。
   * 而 ai_agent 调用大模型必须要有出网能力：实测没地址时
   * mbedtls_net_connect() 返回 -0x52（MBEDTLS_ERR_NET_UNKNOWN_HOST），
   * 也就是 DNS 解析失败，然后 LLM 失败、对话回落到兜底文案。
   *
   * 现象上很容易误判成"deskmate 连不上 agent" —— 其实 ws 早就 online 了，
   * 断的是 agent 出网那一段。
   */

  (void)dm_net_refresh();

  net = dm_net_status();

  for (i = 0; net != NULL && i < net->count; i++)
    {
      const char *n = net->ifs[i].name;

      if (net->ifs[i].wireless || n == NULL)
        {
          continue;
        }

      /* lo 不是出网口（有地址也不代表能连外网）。 */

      if (strcmp(n, "lo") == 0)
        {
          continue;
        }

      ifname = n;
      break;
    }

  if (ifname == NULL)
    {
      DM_LOG("eth: no wired interface, autostart skipped");
      return;
    }

  rc = dm_net_if_set_up(ifname, true);
  DM_LOG("eth: ifup(%s) rc=%d", ifname, rc);

  rc = dm_net_if_set_dhcp_async(ifname);
  DM_LOG("eth: dhcp on %s requested rc=%d", ifname, rc);
}

/* ---------------------------------------------------------------------------
 * --wifi-start 的辅助：hosted 的 RPC wifi_start 在本板 C6 固件上**可能永不返回**
 * （移植层因此故意跳过它，附录 BY.17）。为了既能验证又不会拖死系统，这里用
 * **独立线程**去调，主线程只等一个有上限的时间。
 * ------------------------------------------------------------------------- */

volatile int g_dbg_wifi_start_rc = -999;    /* -999=还没返回 */

#ifdef CONFIG_ARCH_CHIP_ESP32P4
static void *dm_wifi_start_thread(void *arg)
{
  extern int eh_host_wifi_start(void);

  (void)arg;
  g_dbg_wifi_start_rc = eh_host_wifi_start();
  return NULL;
}
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  const char *snap_path = NULL;
  const char *hostfs_path = DM_HOSTFS_PATH;
  const char *agent_mode = DM_AGENT_MODE;
  const char *agent_endpoint = NULL;
  bool agent_spawn = false;
  const char *say = NULL;
  const char *start_page = NULL;
  const char *report_path = NULL;
  bool wifi_probe = false;
  bool ap_probe = false;
  const char *join_ssid = NULL;
  const char *join_psk = NULL;
  bool camera_on = false;
  bool no_camera = false;
  bool no_agent_spawn = false;
  bool no_wlan = false;
  bool evidence_mode = false;
  bool selftest = false;
  bool tour = false;
  int  soak_sec = 0;
  unsigned int exit_after_ms = (unsigned int)DM_EXIT_AFTER * 1000u;
  unsigned int start_ms;
  int result = 0;
  int i;
  bool disp_reinit_once = false;
  int  disp_stall_sec = 0;

  for (i = 1; i < argc; i++)
    {
      if (strcmp(argv[i], "--selftest") == 0)
        {
          selftest = true;
        }
      else if (strcmp(argv[i], "--disp-reinit") == 0)
        {
          disp_reinit_once = true;   /* 显示起来之后再执行，见下面 */
        }
      else if (strcmp(argv[i], "--touch-fake") == 0 && i + 2 < argc)
        {
          extern void dm_touch_fake_hw(int x, int y);

          dm_touch_fake_hw(atoi(argv[i + 1]), atoi(argv[i + 2]));
          i += 2;
        }
#ifdef CONFIG_ARCH_CHIP_ESP32P4
      else if (strcmp(argv[i], "--wifi-start") == 0)
        {
          pthread_t tid;
          struct dm_wifi_ap_s aps[16];
          int waited = 0;

          if (pthread_create(&tid, NULL, dm_wifi_start_thread, NULL) != 0)
            {
              printf("DM-WIFI-START cannot spawn thread\n");
              return 1;
            }

          while (g_dbg_wifi_start_rc == -999 && waited < 50)
            {
              usleep(100 * 1000);
              waited++;
            }

          if (g_dbg_wifi_start_rc == -999)
            {
              printf("DM-WIFI-START blocked (RPC did not return in 5s) "
                     "=> the companion firmware is the 'already started' kind\n");
              return 2;
            }

          printf("DM-WIFI-START rc=%d\n", g_dbg_wifi_start_rc);

          {
            int n = dm_net_wifi_scan(aps, (int)(sizeof(aps) / sizeof(aps[0])));

            printf("DM-WIFI-START scan=%d err=%d\n", n,
                   n < 0 ? dm_net_wifi_last_error() : 0);
            for (int k = 0; n > 0 && k < n; k++)
              {
                printf("DM-WIFI-START ap%d ssid=%s rssi=%d\n", k, aps[k].ssid,
                       aps[k].rssi);
              }
          }

          return 0;
        }
#endif
      else if (strcmp(argv[i], "--wifi-scan") == 0)
        {
          /* wlan0 扫描测试：直接用应用自己的 WAPI 通道（hosted netdev 实现了
           * SIOCGIWSCAN），不依赖镜像里有没有 wapi 命令行工具。
           */

          struct dm_wifi_ap_s aps[16];
          int n = dm_net_wifi_scan(aps, (int)(sizeof(aps) / sizeof(aps[0])));

          if (n < 0)
            {
              printf("DM-SCAN err=%d\n", dm_net_wifi_last_error());
              return 1;
            }

          printf("DM-SCAN count=%d\n", n);
          for (int k = 0; k < n; k++)
            {
              printf("DM-SCAN ap%d ssid=%s rssi=%d freq=%d secure=%d\n",
                     k, aps[k].ssid, aps[k].rssi, aps[k].freq,
                     (int)aps[k].secure);
            }

          return 0;
        }
      else if (strcmp(argv[i], "--touch-wr") == 0 && i + 2 < argc)
        {
#ifdef CONFIG_ARCH_CHIP_ESP32P4
          extern int board_touch_fake_press(int x, int y);

          DM_LOG("touch: GT911 fake press write rc=%d",
                 board_touch_fake_press(atoi(argv[i + 1]), atoi(argv[i + 2])));
          i += 2;
#endif
        }
      else if (strcmp(argv[i], "--disp-stall") == 0 && i + 1 < argc)
        {
          disp_stall_sec = atoi(argv[++i]);   /* 人为把 UI 循环冻住，测看门狗 */
        }
#ifdef CONFIG_ESPRESSIF_PPA
      else if (strcmp(argv[i], "--ppa-test") == 0)
        {
          /* PPA 是 ESP32-P4 的 arch 侧 helper，模拟器上没有 */

          extern int esp_ppa_selftest(void);

          int rc = esp_ppa_selftest();

          DM_LOG("PPA selftest rc=%d", rc);
          return rc == 0 ? 0 : 1;
        }
#endif
      else if (strcmp(argv[i], "--freeze-preview") == 0)
        {
          extern int g_freeze_preview;

          g_freeze_preview = 1;
        }
      else if (strcmp(argv[i], "--soak") == 0 && i + 1 < argc)
        {
          soak_sec = atoi(argv[++i]);
        }
      else if (strcmp(argv[i], "--tour") == 0)
        {
          tour = true;
        }
      else if (strcmp(argv[i], "--snap") == 0 && i + 1 < argc)
        {
          snap_path = argv[++i];
        }
      else if (strcmp(argv[i], "--hostfs") == 0 && i + 1 < argc)
        {
          hostfs_path = argv[++i];
        }
      else if (strcmp(argv[i], "--agent") == 0 && i + 1 < argc)
        {
          agent_mode = argv[++i];
        }
      else if (strcmp(argv[i], "--agent-endpoint") == 0 && i + 1 < argc)
        {
          agent_endpoint = argv[++i];
        }
      else if (strcmp(argv[i], "--agent-spawn") == 0)
        {
          agent_spawn = true;
        }
      else if (strcmp(argv[i], "--say") == 0 && i + 1 < argc)
        {
          say = argv[++i];
        }
      else if (strcmp(argv[i], "--cam-fps") == 0 && i + 1 < argc)
        {
          dm_page_camera_set_fps(atoi(argv[++i]));
        }
      else if (strcmp(argv[i], "--camera") == 0)
        {
          camera_on = true;
        }
      else if (strcmp(argv[i], "--no-camera") == 0)
        {
          no_camera = true;
        }
      else if (strcmp(argv[i], "--no-agent-spawn") == 0)
        {
          no_agent_spawn = true;
        }
      else if (strcmp(argv[i], "--no-wlan") == 0)
        {
          no_wlan = true;
        }
      else if (strcmp(argv[i], "--ap-probe") == 0)
        {
          ap_probe = true;
        }
      else if (strcmp(argv[i], "--wifi-connect") == 0 && i + 1 < argc)
        {
          join_ssid = argv[++i];

          if (i + 1 < argc && argv[i + 1][0] != '-')
            {
              join_psk = argv[++i];
            }
        }
      else if (strcmp(argv[i], "--wifi-probe") == 0)
        {
          wifi_probe = true;
        }
      else if (strcmp(argv[i], "--report") == 0 && i + 1 < argc)
        {
          report_path = argv[++i];
        }
      else if (strcmp(argv[i], "--page") == 0 && i + 1 < argc)
        {
          start_page = argv[++i];
        }
      else if (strcmp(argv[i], "--exit-after") == 0 && i + 1 < argc)
        {
          exit_after_ms = (unsigned int)atoi(argv[++i]) * 1000u;
        }
      else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
          printf("%s", g_usage);
          return 0;
        }
      else
        {
          DM_WARN("ignoring unknown argument '%s'", argv[i]);
        }
    }

  /* ── 默认启动策略 ───────────────────────────────────────────────────────
   *
   * 不带参数直接跑 deskmate 时，用户期待的是"一块能用的中控屏"，不是一块
   * 需要先敲三条命令配置的空白屏。所以交互式启动默认把三件事拉起来：
   *
   *   · 摄像头     → 相机页一进去就有画面（--no-camera 关）
   *   · ai_agent   → 同进程起内置 agent 并 ws 连上，顶栏"已连服务"（--no-agent-spawn 关）
   *   · wlan0      → ifup + DHCP（可选关联），网络页直接有地址（--no-wlan 关）
   *
   * **取证与诊断模式不走这些默认**：--selftest / --tour 要可重复、可对比，
   * 不能被后台线程、Agent 的 1.5 秒监听等待、以及 DHCP 抢地址搅乱；
   * --wifi-* / --ap-probe 是"测一次就退出"的探针，更是必须保持纯净。
   * ---------------------------------------------------------------------- */

  evidence_mode = selftest || tour || wifi_probe || ap_probe ||
                  (join_ssid != NULL);

  /* --no-* 是**否决票**：即使显式给了 --camera / --agent-spawn 也以关为准，
   * 否则"关掉"这个语义在组合参数下会静默失效。
   */

  if (no_camera)
    {
      camera_on = false;
    }

  if (no_agent_spawn)
    {
      agent_spawn = false;
    }

  {
    if (!evidence_mode)
      {
        if (!no_camera && DM_CAMERA_AUTOSTART)
          {
            camera_on = true;
          }

        if (!no_agent_spawn && DM_AGENT_AUTOSPAWN)
          {
            agent_spawn = true;
          }
      }

    DM_LOG("startup: camera=%d agent_spawn=%d wlan=%d (evidence_mode=%d)",
           (int)camera_on, (int)agent_spawn,
           (int)(!evidence_mode && !no_wlan && DM_WLAN_AUTOSTART),
           (int)evidence_mode);
  }

  DM_LOG("%s %s starting (selftest=%d)", DM_APP_NAME, DM_APP_VERSION,
         (int)selftest);

  dm_mount_hostfs(hostfs_path);
  dm_write_wifi_bss();

  /* 无线自启放在显示/UI 之前：DHCP 是异步的，越早发起越可能"界面一出来就
   * 已经有地址"。取证模式不碰（见上面的默认启动策略）。
   */

  if (!evidence_mode && !no_wlan && DM_WLAN_AUTOSTART)
    {
      /* 有线优先：eth0 不用等关联，插上就有网；wlan0 还要先关联 AP。 */

      dm_eth_autostart();
      dm_wlan_autostart(DM_WLAN_SSID, DM_WLAN_PSK);
    }

#if defined(CONFIG_BOARDCTL) && !defined(CONFIG_NSH_ARCHINIT)
  boardctl(BOARDIOC_INIT, 0);
#endif

  /* 一个进程生命周期里只允许跑一轮。
   *
   * `lv_is_initialized()` 只能在"上一轮没有 lv_deinit()"时挡住重入（例如在
   * lv_init 之后、display 创建失败那条提前返回路径上）；正常退出会调
   * lv_deinit()，于是这个判据在第二轮恒为 false，挡不住。
   *
   * 而第二轮**确实会崩**：`lv_deinit()` 释放了 LVGL 的全部对象，但各页面模块
   * （dm_page_home/net/chat/camera/settings）和 dm_ui.c 里的 `static lv_obj_t *`
   * 是我们自己的变量、不会跟着清零。第二轮 `dm_ui_create()` 走到建页面时就会
   * 在已释放的对象上做 `lv_*` 调用，症状是 `lv_obj_get_screen` 里
   * `Load access fault`，MTVAL 是个明显不是指针的小整数（实测 0x7d / 0x3d）。
   * 已经给 dm_ui.c 做过一轮置 NULL，但页面模块的 static 指针同样需要处理，
   * 而那涉及 5 个文件、收益只在于"同一次开机里连跑两次"。
   *
   * 因此这里用进程内的静态标志明确拒绝第二次，把"崩"变成"一句话说清楚"。
   * 需要再跑一次时：复位板子，或在另一个 shell 里 `nsh` 退出后重进。
   */

  static bool s_ran_once;

  if (s_ran_once || lv_is_initialized())
    {
      DM_ERR("deskmate 已在本进程里跑过一轮；LVGL 的静态对象指针无法安全复用。"
             " 请复位后重跑（或换一个 shell 会话）。");
      return 1;
    }

  s_ran_once = true;

  lv_init();

  if (dm_display_create() != 0)
    {
      DM_ERR("display bring-up failed");
      lv_deinit();
      return 1;
    }

  if (dm_touch_create() != 0)
    {
      DM_ERR("touch bring-up failed");
      dm_camera_destroy();
      dm_display_destroy();
      dm_touch_shutdown();
      lv_deinit();
      return 1;
    }

  /* 端点必须先于 init 生效（init 里就会发起第一次 ws 连接）。
   *
   * 默认是 127.0.0.1:28789 —— 但本板 **CONFIG_NET_LOOPBACK 未开**，
   * 127.0.0.1 会被路由到默认网卡，永远连不到本机的 ai_agent。
   * 所以给一个显式覆盖入口：--agent-endpoint <ip[:port]>。
   */

  /* --agent-spawn：把内置 ai_agent 直接起成**同进程任务**。
   *
   * 为什么不用 `ai_agent &`：本配置 CONFIG_NSH_BUILTIN_AS_COMMAND=y，
   * `&` 后台那条路在 nsh_parse.c 里被显式判死（nsh_fileapp/nsh_builtin
   * 都没编进来），会直接报 "cannot run ... in the background in this build"。
   * 而且开了 CONFIG_NSH_BUILTIN_AS_COMMAND=n 会让 builtin 派发把整机卡死
   * （见 defconfig 里 2026-09-15 的记录）。所以就在自家进程里 task_create。
   */

  /* --agent-spawn 隐含 ws 模式（mock 时根本不会去连）。 */

  if (agent_spawn && strcmp(agent_mode, "ws") != 0)
    {
      agent_mode = "ws";
    }

  if (agent_spawn)
    {
      /* ai_agent 的入口只有在镜像里编进了 ai_agent 应用时才存在。
       *
       * sim / goldfish 两个配置都**没有** CONFIG_EXAMPLES_AI_AGENT_VELA：
       * 不加这个守卫时，deskmate_main.o 会引用一个不存在的符号，整条链接
       * 直接失败（`undefined reference to ai_agent_main`），连"跑一次自检"
       * 都做不到。加守卫后：没有 ai_agent 的镜像上 --agent-spawn 退化成
       * 一条明确的告警，UI 照常起来。
       */

#if defined(CONFIG_EXAMPLES_AI_AGENT_VELA)
      extern int ai_agent_main(int argc, char *argv[]);
      static char *agent_argv[] = { "ai_agent", "--no-cli", NULL };
      int rc = task_create("ai_agent", 100, 24576, ai_agent_main, agent_argv);

      /* 24KB 栈：ai_agent 自己的 PROGNAME 栈是 16KB，但它还会继续起
       * 线程，这里给足余量；失败只影响"能不能用真 agent"，UI 照常。
       */

      if (rc < 0)
        {
          DM_WARN("agent: task_create(ai_agent) failed: %d", errno);
        }
      else
        {
          DM_LOG("agent: built-in ai_agent started as task %d", rc);
          usleep(1500 * 1000);   /* 等它把 28789 监听上 */
        }
#else
      DM_WARN("agent: --agent-spawn needs CONFIG_EXAMPLES_AI_AGENT_VELA=y "
              "(ai_agent not in this image); staying in %s mode", agent_mode);
#endif
    }

  if (agent_endpoint != NULL)
    {
      char host[64];
      int port = 28789;
      const char *colon = strchr(agent_endpoint, ':');

      if (colon != NULL)
        {
          size_t n = (size_t)(colon - agent_endpoint);

          if (n >= sizeof(host))
            {
              n = sizeof(host) - 1;
            }

          memcpy(host, agent_endpoint, n);
          host[n] = '\0';
          port = atoi(colon + 1);
        }
      else
        {
          strlcpy(host, agent_endpoint, sizeof(host));
        }

      dm_agent_set_endpoint(host, port);
    }
  else if (agent_spawn)
    {
      /* spawn 出来的 agent 就监听在 INADDR_ANY:28789，走 lo 最稳。 */

      dm_agent_set_endpoint("127.0.0.1", 28789);
    }

  (void)dm_agent_init(agent_mode);

  printf("[dm] net: refresh begin\n");
  fflush(stdout);
  (void)dm_net_refresh();
  printf("[dm] net: refresh done\n");
  fflush(stdout);

  if (ap_probe)
    {
      int rc = dm_net_ap_probe();

      printf("DM-AP-PROBE rc=%d (see g_dbg_ap_* in RAM)\n", rc);
      fflush(stdout);
      dm_agent_destroy();
      dm_camera_destroy();
      dm_display_destroy();
      dm_touch_shutdown();
      lv_deinit();
      return rc == 0 ? 0 : 1;
    }

  if (join_ssid != NULL)
    {
      char linked[DM_NET_SSID_LEN];
      int  rssi = 0;

      g_app_wifi_connect_rc = dm_net_wifi_connect(join_ssid, join_psk);
      printf("DM-WIFI-CONNECT ssid='%s' rc=%d\n", join_ssid,
             g_app_wifi_connect_rc);
      fflush(stdout);

      if (dm_net_wifi_wait_link(linked, sizeof(linked), &rssi, 20000) == 0)
        {
          char ip[20] = "";

          printf("DM-WIFI-LINK ok ssid='%s' rssi=%d\n", linked, rssi);

          /* 关联成功后直接在 wlan0 上取地址（DHCP），这样"连上并拿到 IP"
           * 是一条命令就能验收的事。
           */

          if (dm_net_if_set_dhcp("wlan0") == 0)
            {
              (void)dm_net_if_get_config("wlan0", ip, sizeof(ip), NULL, 0,
                                         NULL, 0);
              printf("DM-WIFI-IP %s\n", ip[0] != '\0' ? ip : "0.0.0.0");
            }
          else
            {
              printf("DM-WIFI-IP dhcp-failed\n");
            }
        }
      else
        {
          printf("DM-WIFI-LINK timeout\n");
        }

      fflush(stdout);
      dm_agent_destroy();
      dm_camera_destroy();
      dm_display_destroy();
      dm_touch_shutdown();
      lv_deinit();
      return g_app_wifi_link_ok ? 0 : 1;
    }

  if (wifi_probe)
    {
      /* 诊断模式：只回答"WiFi 有没有问题"，不起 UI。
       * 板上串口仍会有驱动/中断日志插进来，所以支持 --report 把报告写文件。
       */

      if (report_path != NULL && report_path[0] != '\0')
        {
          if (freopen(report_path, "w", stdout) == NULL)
            {
              DM_WARN("wifi-probe: cannot open report '%s', using stdout",
                      report_path);
            }
        }

      (void)dm_net_wifi_probe();
      printf("DM-WIFI-PROBE DONE\n");
      fflush(stdout);
      dm_agent_destroy();
      dm_camera_destroy();
      dm_display_destroy();
      dm_touch_shutdown();
      lv_deinit();
      return 0;
    }
  /* 摄像头：交互式启动时默认打开（见上面「默认启动策略」），
   * 只有 --no-camera 或 CONFIG_EXAMPLES_DESKMATE_CAMERA_AUTOSTART=n 才关。
   *
   * CSI 与 DSI **不互斥**。2026-11-06 真机实测（本板）：连续预览期间 DSI 的
   * 传输完成中断计数从 0x52a8 涨到 0x5416（6 秒 +366），同时相机发布帧数
   * 0x144b -> 0x14a3（+88 帧）；两者同时增长 ⇒ 面板在被刷新、相机在出帧。
   * 曾经"一开摄像头面板变纯色"的根因是 ISP bypass 在**相机首次打开时**才
   * bring-up，而那时 DSI 已经在出图；板级 bringup 已把它提到 DSI 启动之前
   * （esp32p4_camera.c 里的 esp_csi_isp_prepare）。
   */

  dm_camera_set_disabled(!camera_on);

  if (camera_on)
    {
      (void)dm_camera_create(DM_CAMERA_PREVIEW_W, DM_CAMERA_PREVIEW_H);
    }

  if (disp_stall_sec > 0)
    {
      /* 模拟"面板被打停"：UI 线程睡 N 秒，提交计数会停 ⇒ 看门狗应当报出并自救 */

      DM_ERR("display: 人为停摆 %d 秒（看门狗测试）", disp_stall_sec);
      sleep((unsigned)disp_stall_sec + 5);   /* 先跑 5 秒让看门狗建立基线 */
      sleep((unsigned)disp_stall_sec);
    }

  if (disp_reinit_once)
    {
#ifdef CONFIG_ARCH_CHIP_ESP32P4
      extern int esp32p4_display_reinit(void);

      DM_LOG("display reinit rc=%d", esp32p4_display_reinit());
#endif
    }

  if (dm_ui_create() != 0)
    {
      DM_ERR("UI build failed");
      dm_camera_destroy();
      dm_agent_destroy();
      dm_display_destroy();
      dm_touch_shutdown();
      lv_deinit();
      return 1;
    }

  if (start_page != NULL)
    {
      (void)dm_ui_goto_id(start_page);
    }

  /* --say：端到端探针。
   *
   * 走的是**和界面完全一样**的路径（dm_agent_send），所以它能把
   * "deskmate 组帧 -> ws_server 解帧 -> agent_loop -> 回帧 -> dm_agent_poll"
   * 整条链一次验证掉，不需要手指去点屏幕。
   */

  if (say != NULL)
    {
      char reply[512];
      int waited;

      /* 必须先等握手完成。实测 --say 会在 ws 连上之前就发出去（探针在
       * boot+1.7s 跑，ws 在 boot+8s 才 connected），那时 dm_agent_send 会
       * 走"无后端 -> 本地离线应答"，看起来像"连不上 agent"，其实是抢跑。
       */

      for (waited = 0; waited < 40; waited++)
        {
          const char *st = dm_agent_state();

          if (st != NULL && strcmp(st, "online") == 0)
            {
              break;
            }

          if (st != NULL && (strcmp(st, "mock") == 0 ||
                             strcmp(st, "off") == 0))
            {
              break;   /* 这两种模式下不会有 ws，别干等 */
            }

          /* 探针在主线程里等，而重连逻辑挂在 dm_agent_poll() 上 —— 等的时候
           * 没有 poll，也就没人去连。不主动踢一脚的话这一圈纯属空转，
           * 会一路等到 20s 超时（实测）。
           */

          dm_agent_retry_now();
          usleep(500000);
        }

      printf("DM-PROBE state=%s after %dms\n", dm_agent_state(), waited * 500);
      DM_LOG("probe: sending \"%s\" through the agent bridge", say);
      printf("DM-PROBE send rc=%d\n", dm_agent_send(say));

      for (waited = 0; waited < 40; waited++)
        {
          if (dm_agent_poll(reply, sizeof(reply)) > 0)
            {
              printf("DM-PROBE recv: %s\n", reply);
              break;
            }

          usleep(500000);
        }

      if (waited >= 40)
        {
          printf("DM-PROBE recv: TIMEOUT (state=%s err=%d sent=%u recv=%u)\n",
                 dm_agent_state(), dm_agent_last_error(),
                 dm_agent_sent_count(), dm_agent_recv_count());
        }

      fflush(stdout);
    }

  {
    const struct dm_display_metrics_s *metrics = dm_display_metrics();

    printf("DM-READY display=%s %dx%d bpp=%d lvgl=%s agent=%s camera=%s "
           "page=%s\n",
           metrics != NULL ? metrics->backend : "none",
           metrics != NULL ? metrics->width : 0,
           metrics != NULL ? metrics->height : 0,
           metrics != NULL ? metrics->bpp : 0, lv_version_info(),
           dm_agent_state(), dm_camera_backend(),
           dm_ui_current_id() != NULL ? dm_ui_current_id() : "?");
    fflush(stdout);
  }

  if (selftest || tour)
    {
      /* 快照目录：模拟器经 hostfs 落宿主；真机没有 hostfs，落 /tmp（tmpfs）。
       * --snap 显式给了就以它为准。
       */

      char default_snap[96];

      if (snap_path == NULL)
        {
          snprintf(default_snap, sizeof(default_snap), "%s/%s",
                   g_hostfs_mounted ? "/host/deskmate" : "/tmp/deskmate",
                   selftest ? "dm_selftest.bin" : "dm_tour.bin");

          (void)mkdir(g_hostfs_mounted ? "/host/deskmate" : "/tmp/deskmate",
                      0777);
          snap_path = default_snap;
        }

      if (selftest)
        {
          if (report_path != NULL)
            {
              dm_selftest_set_report(report_path);
            }

          dm_selftest_begin(snap_path);
        }
      else
        {
          dm_tour_begin(snap_path);
        }

      /* Give LVGL a couple of frames to draw the freshly built tree before
       * the scenario starts poking at it.
       */

      for (i = 0; i < 4; i++)
        {
          lv_timer_handler();
          dm_ui_tick();
          usleep(5000);
        }
    }

  start_ms = dm_now_ms();

  for (; ; )
    {
      static uint32_t hb_ms;
      static uint32_t hb_loops;
      uint32_t idle;

      hb_loops++;
      if (hb_ms == 0 || dm_now_ms() - hb_ms >= 5000u)
        {
          hb_ms = dm_now_ms();
          DM_LOG("heartbeat: loops=%u cam_page_calls=%u",
                 hb_loops, (unsigned)dm_page_camera_calls());
          hb_loops = 0;

          /* 触摸采样链路的分段统计。四个数字必须一起看：
           *   ticks 涨、xfers 不涨 => 采样被调度饿死（LPWORK 拿不到 CPU）
           *   xfers 涨、hist 高     => 总线/控制器慢
           *   两者都涨、events 少   => 两次采样之间丢了动作
           *   lock_waits 涨         => 和 open()/close() 抢锁
           */

          {
            struct dm_touch_drv_stats_s ts;

            if (dm_touch_driver_stats(&ts) == 0)
              {
                /* dm_log_rate() 而不是 DM_LOG()：后者的 key 是 __func__，
                 * 上面那行 heartbeat 也是 main()，同一秒内的第二行会被限速
                 * 丢掉 —— 实测就是这个现象（心跳后面跟着"+1 条同源日志被限速"，
                 * 而触摸统计一行都没出来）。
                 */

                dm_log_rate("touchstat",
                            "touch: ticks=%u xfers=%u err=%u events=%u "
                            "lock=%u/%ums xfer_max=%uus hist=%u/%u/%u/%u/%u/%u",
                            (unsigned)ts.ticks, (unsigned)ts.xfers,
                            (unsigned)ts.xfer_err, (unsigned)ts.events,
                            (unsigned)ts.lock_waits, (unsigned)ts.lock_ms_max,
                            (unsigned)ts.xfer_us_max,
                            (unsigned)ts.hist[0], (unsigned)ts.hist[1],
                            (unsigned)ts.hist[2], (unsigned)ts.hist[3],
                            (unsigned)ts.hist[4], (unsigned)ts.hist[5]);
              }
          }
        }

      {
        unsigned int busy0 = dm_now_ms();
        unsigned int t1;

        idle = lv_timer_handler();
        t1 = dm_now_ms();
        dm_ui_note_timer(t1 - busy0);

        dm_ui_tick();
        dm_ui_note_tick(dm_now_ms() - t1);
        dm_ui_note_busy(dm_now_ms() - busy0);
      }

      unsigned int elapsed = dm_now_ms() - start_ms;

      /* --soak：定时切页 + 定期把关键指标追加到文件。
       * 长跑验证用：屏幕必须一直有画面（display commits 持续增长）、
       * 相机不能卡死（published frames 持续增长）、UI 不能被拖出长停顿。
       */

      if (soak_sec > 0)
        {
          static uint32_t page_ms;
          static uint32_t log_ms;
          static int      page_idx;

          if (page_ms == 0 || dm_now_ms() - page_ms >= (uint32_t)soak_sec * 1000u)
            {
              page_ms = dm_now_ms();
              page_idx = (page_idx + 1) % dm_ui_page_count();
              (void)dm_ui_goto(page_idx);
            }

          if (log_ms == 0 || dm_now_ms() - log_ms >= 30000u)
            {
              FILE *fp;

              log_ms = dm_now_ms();

              fp = fopen("/tmp/deskmate_soak.txt", "a");
              if (fp != NULL)
                {
                  fprintf(fp,
                          "t=%ums page=%d commits=%u cam_frames=%u loops=%u "
                          "max_busy=%ums\n",
                          (unsigned)dm_now_ms(), page_idx,
                          (unsigned)dm_display_commits(),
                          (unsigned)dm_page_camera_frames(),
                          (unsigned)dm_ui_loop_count(),
                          (unsigned)dm_ui_max_busy_ms());
                  fclose(fp);
                }
            }
        }

      if (dm_selftest_active())
        {
          dm_selftest_step();
        }

      if (dm_tour_active())
        {
          dm_tour_step();
        }

      if (selftest && !dm_selftest_active())
        {
          dm_selftest_finish();
          result = dm_selftest_result() == 0 ? 0 : 1;
          break;
        }

      if (tour && !dm_tour_active())
        {
          dm_tour_finish();
          break;
        }

      if (exit_after_ms > 0 && elapsed >= exit_after_ms)
        {
          DM_LOG("exit-after reached (%u ms)", exit_after_ms);
          break;
        }

      if (idle == 0)
        {
          idle = 1;
        }
      else if (idle > DM_MAX_SLEEP_MS)
        {
          idle = DM_MAX_SLEEP_MS;
        }

      usleep(idle * 1000);
    }

  dm_camera_destroy();
  dm_agent_destroy();
  dm_display_destroy();
      dm_touch_shutdown();
  lv_deinit();

  printf("DM-EXIT %s result=%d\n", selftest ? "selftest" : "run", result);
  fflush(stdout);

  return result;
}
