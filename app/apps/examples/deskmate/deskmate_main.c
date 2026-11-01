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

#include "dm_config.h"
#include "dm_log.h"
#include "dm_selftest.h"
#include "hal/dm_display.h"
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
  "  --snap <path>       snapshot destination (default " DM_SNAP_HOST_DIR "/dm_home.bin)\n"
  "  --hostfs <dir>      mount this host directory at /host (simulator)\n"
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
  DM_LOG("hostfs: %s mounted at /host", path);
#else
  DM_LOG("hostfs: not enabled in this configuration (%s)",
         path != NULL ? path : "");
#endif
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  const char *snap_path = NULL;
  const char *hostfs_path = DM_HOSTFS_PATH;
  bool selftest = false;
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
      else if (strcmp(argv[i], "--snap") == 0 && i + 1 < argc)
        {
          snap_path = argv[++i];
        }
      else if (strcmp(argv[i], "--hostfs") == 0 && i + 1 < argc)
        {
          hostfs_path = argv[++i];
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
      dm_display_destroy();
      lv_deinit();
      return 1;
    }

  if (dm_ui_create() != 0)
    {
      DM_ERR("UI build failed");
      dm_display_destroy();
      lv_deinit();
      return 1;
    }

  {
    const struct dm_display_metrics_s *metrics = dm_display_metrics();

    printf("DM-READY display=%s %dx%d bpp=%d lvgl=%s\n",
           metrics != NULL ? metrics->backend : "none",
           metrics != NULL ? metrics->width : 0,
           metrics != NULL ? metrics->height : 0,
           metrics != NULL ? metrics->bpp : 0, lv_version_info());
    fflush(stdout);
  }

  if (selftest)
    {
      dm_selftest_begin(snap_path);

      /* Give LVGL a couple of frames to draw the freshly built tree before
       * the scenario starts poking at it.
       */

      for (i = 0; i < 4; i++)
        {
          lv_timer_handler();
          usleep(5000);
        }
    }

  start_ms = dm_now_ms();

  for (; ; )
    {
      uint32_t idle = lv_timer_handler();
      unsigned int elapsed = dm_now_ms() - start_ms;

      dm_ui_tick();

      if (dm_selftest_active())
        {
          dm_selftest_step();
        }

      if (selftest && !dm_selftest_active())
        {
          result = dm_selftest_result() == 0 ? 0 : 1;
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

  dm_display_destroy();
  lv_deinit();

  printf("DM-EXIT %s result=%d\n", selftest ? "selftest" : "run", result);
  fflush(stdout);

  return result;
}
