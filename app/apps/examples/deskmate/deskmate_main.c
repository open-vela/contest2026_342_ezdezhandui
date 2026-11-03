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

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
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
#include "ui/dm_ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Longest sleep between loop iterations: keeps exit conditions and the
 * self-test responsive even when LVGL has nothing to do.
 */

#define DM_MAX_SLEEP_MS 40

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char *g_usage =
  "Usage: " DM_PROGNAME " [options]\n"
  "  --selftest          run the headless scenario, print the report and exit\n"
  "  --freeze-preview    stop re-pointing the canvas at new frames (diagnostic)\n"
  "  --soak <sec>        cycle pages every <sec> seconds and log metrics (long run)\n"
  "  --tour              walk every page and snapshot it (demo, no assertions)\n"
  "  --snap <path>       snapshot destination (default " DM_SNAP_HOST_DIR "/dm_selftest.bin)\n"
  "  --hostfs <dir>      mount this host directory at /host (simulator)\n"
  "  --agent <mode>      mock | ws | off\n"
  "  --report <path>     also write the self-test report to a file\n"
  "  --wifi-probe        print a WiFi diagnostic report and exit\n"
  "  --camera            open /dev/video0 (CSI shares the MIPI PHY with DSI;\n"
  "                      on this board opening it kills the display, so it is\n"
  "                      OFF by default until the platform conflict is fixed)\n"
  "  --page <id|index>   start on a page\n"
  "  --exit-after <sec>  leave the UI loop after <sec> seconds\n"
  "  --help              this text\n";

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

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  const char *snap_path = NULL;
  const char *hostfs_path = DM_HOSTFS_PATH;
  const char *agent_mode = DM_AGENT_MODE;
  const char *start_page = NULL;
  const char *report_path = NULL;
  bool wifi_probe = false;
  bool ap_probe = false;
  const char *join_ssid = NULL;
  const char *join_psk = NULL;
  bool camera_on = false;
  bool selftest = false;
  bool tour = false;
  int  soak_sec = 0;
  unsigned int exit_after_ms = (unsigned int)DM_EXIT_AFTER * 1000u;
  unsigned int start_ms;
  int result = 0;
  int i;

  for (i = 1; i < argc; i++)
    {
      if (strcmp(argv[i], "--selftest") == 0)
        {
          selftest = true;
        }
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
      else if (strcmp(argv[i], "--cam-fps") == 0 && i + 1 < argc)
        {
          dm_page_camera_set_fps(atoi(argv[++i]));
        }
      else if (strcmp(argv[i], "--camera") == 0)
        {
          camera_on = true;
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

  DM_LOG("%s %s starting (selftest=%d)", DM_APP_NAME, DM_APP_VERSION,
         (int)selftest);

  dm_mount_hostfs(hostfs_path);
  dm_write_wifi_bss();

#if defined(CONFIG_BOARDCTL) && !defined(CONFIG_NSH_ARCHINIT)
  boardctl(BOARDIOC_INIT, 0);
#endif

  if (lv_is_initialized())
    {
      DM_ERR("LVGL is already initialised; refusing to run twice");
      return 1;
    }

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
      lv_deinit();
      return 1;
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
      lv_deinit();
      return 0;
    }
  /* 摄像头默认关闭：CSI 与 DSI 共享 MIPI PHY，本板实测一开摄像头 DSI 就失去
   * 同步（面板变纯色）。平台侧解决之前，默认不碰它；--camera 可显式打开。
   */

  dm_camera_set_disabled(!camera_on);

  if (camera_on)
    {
      (void)dm_camera_create(DM_CAMERA_PREVIEW_W, DM_CAMERA_PREVIEW_H);
    }

  if (dm_ui_create() != 0)
    {
      DM_ERR("UI build failed");
      dm_camera_destroy();
      dm_agent_destroy();
      dm_display_destroy();
      lv_deinit();
      return 1;
    }

  if (start_page != NULL)
    {
      (void)dm_ui_goto_id(start_page);
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
        }

      {
        unsigned int busy0 = dm_now_ms();

        idle = lv_timer_handler();
        dm_ui_tick();
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
  lv_deinit();

  printf("DM-EXIT %s result=%d\n", selftest ? "selftest" : "run", result);
  fflush(stdout);

  return result;
}
