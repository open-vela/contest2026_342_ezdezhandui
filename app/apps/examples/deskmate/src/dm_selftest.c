/****************************************************************************
 * apps/examples/deskmate/src/dm_selftest.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * See dm_selftest.h for the scenario.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <lvgl/lvgl.h>

#include "dm_config.h"
#include "dm_log.h"
#include "dm_selftest.h"
#include "hal/dm_display.h"
#include "hal/dm_net.h"
#include "hal/dm_touch.h"
#include "ui/dm_page.h"
#include "ui/dm_ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Loop iterations granted to LVGL between injecting a tap and checking its
 * effect: the indev is sampled inside lv_timer_handler().
 */

#define DM_SELFTEST_TAP_FRAMES 8
#define DM_SELFTEST_PAGE_FRAMES 6

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum dm_selftest_state_e
{
  DM_ST_IDLE = 0,
  DM_ST_TAP_WAIT,
  DM_ST_GOTO_CAMERA,
  DM_ST_CAMERA_WAIT,
  DM_ST_GOTO_NET,
  DM_ST_NET_WAIT,
  DM_ST_DONE
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static enum dm_selftest_state_e g_state = DM_ST_IDLE;
static char g_snap_path[128];
static char g_snap_dir[112];
static int  g_frames;
static int  g_checks;
static int  g_failures;
static int  g_result = -1;
static int  g_taps_before;
static uint32_t g_hash_home;
static uint32_t g_hash_camera;
static char g_detail[128];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void dm_check(const char *name, bool ok, const char *detail)
{
  g_checks++;

  if (!ok)
    {
      g_failures++;
    }

  printf("[dm-check] %-28s %s %s\n", name, ok ? "PASS" : "FAIL",
         detail != NULL ? detail : "");
  fflush(stdout);
}

static void dm_selftest_snapshot(const char *name)
{
  char path[128];
  int written;

  snprintf(path, sizeof(path), "%s/%s.bin", g_snap_dir, name);

  written = dm_display_snapshot(path);
  snprintf(g_detail, sizeof(g_detail), "%s -> %d bytes", name, written);
  dm_check("snapshot:written", written > 0, g_detail);
}

void dm_selftest_begin(const char *snap_path)
{
  g_checks   = 0;
  g_failures = 0;
  g_frames   = 0;
  g_result   = -1;
  g_state    = DM_ST_TAP_WAIT;

  if (snap_path != NULL && snap_path[0] != '\0')
    {
      strlcpy(g_snap_path, snap_path, sizeof(g_snap_path));
    }
  else
    {
      strlcpy(g_snap_path, DM_SNAP_HOST_DIR "/dm_selftest.bin",
              sizeof(g_snap_path));
    }

  /* Snapshot directory = the directory part of the requested path, so one
   * run writes dm_home.bin / dm_camera.bin / dm_net.bin side by side.
   */

  strlcpy(g_snap_dir, g_snap_path, sizeof(g_snap_dir));
  {
    char *slash = strrchr(g_snap_dir, '/');

    if (slash != NULL && slash != g_snap_dir)
      {
        *slash = '\0';
      }
    else
      {
        strlcpy(g_snap_dir, "/tmp", sizeof(g_snap_dir));
      }
  }

  /* Checks that can be answered straight away. */

  {
    const struct dm_display_metrics_s *metrics = dm_display_metrics();
    char detail[96];

    if (metrics == NULL)
      {
        dm_check("display:ready", false, "no metrics");
      }
    else
      {
        snprintf(detail, sizeof(detail), "%dx%d %dbpp %s", metrics->width,
                 metrics->height, metrics->bpp, metrics->backend);
        dm_check("display:ready",
                 metrics->width > 0 && metrics->height > 0 &&
                 metrics->pixels != NULL && metrics->pixels_size > 0,
                 detail);
      }
  }

  {
    char detail[48];

    snprintf(detail, sizeof(detail), "%s", lv_version_info());
    dm_check("lvgl:version", lv_version_major() == 9, detail);
  }

  {
    char detail[64];

    snprintf(detail, sizeof(detail), "%d objects, %d pages",
             dm_ui_object_count(), dm_ui_page_count());
    dm_check("ui:built",
             dm_ui_object_count() >= 3 && dm_ui_page_count() >= 4, detail);
  }

  {
    char detail[48];

    snprintf(detail, sizeof(detail), "page=%s",
             dm_ui_current_id() != NULL ? dm_ui_current_id() : "?");
    dm_check("ui:home", dm_ui_current() == 0, detail);
  }

  /* Touch: aim at the home button and let LVGL consume the sample. */

  {
    int x = 0;
    int y = 0;

    g_taps_before = dm_page_home_tap_count();

    if (dm_page_home_tap_target(&x, &y) != 0)
      {
        dm_check("touch:target", false, "no tap target");
        g_frames = 0;
      }
    else
      {
        snprintf(g_detail, sizeof(g_detail), "(%d,%d) ->", x, y);
        dm_check("touch:target", x > 0 && y > 0, g_detail);
        dm_touch_tap(x, y);
        g_frames = DM_SELFTEST_TAP_FRAMES;
      }
  }

  DM_LOG("selftest: scenario started, snapshots in %s", g_snap_dir);
}

void dm_selftest_step(void)
{
  switch (g_state)
    {
      case DM_ST_TAP_WAIT:
        if (g_frames > 0)
          {
            g_frames--;
            return;
          }

        {
          int now = dm_page_home_tap_count();

          snprintf(g_detail, sizeof(g_detail), "%d -> %d", g_taps_before, now);
          dm_check("touch:delivered", now > g_taps_before, g_detail);
        }

        /* Home page evidence, then navigate by tapping a real tab button. */

        g_hash_home = dm_display_frame_hash();
        dm_selftest_snapshot("dm_home");

        {
          int x = 0;
          int y = 0;

          if (dm_ui_tab_target("camera", &x, &y) != 0)
            {
              dm_check("ui:tab-target", false, "camera tab not found");
              g_state = DM_ST_DONE;
              return;
            }

          snprintf(g_detail, sizeof(g_detail), "(%d,%d)", x, y);
          dm_check("ui:tab-target", x > 0 && y > 0, g_detail);
          dm_touch_tap(x, y);
        }

        g_frames = DM_SELFTEST_PAGE_FRAMES;
        g_state  = DM_ST_GOTO_CAMERA;
        return;

      case DM_ST_GOTO_CAMERA:
        if (g_frames > 0)
          {
            g_frames--;
            return;
          }

        snprintf(g_detail, sizeof(g_detail), "page=%s",
                 dm_ui_current_id() != NULL ? dm_ui_current_id() : "?");
        dm_check("ui:camera-page",
                 dm_ui_current_id() != NULL &&
                 strcmp(dm_ui_current_id(), "camera") == 0, g_detail);

        g_hash_camera = dm_display_frame_hash();
        snprintf(g_detail, sizeof(g_detail), "home=0x%08x camera=0x%08x",
                 (unsigned)g_hash_home, (unsigned)g_hash_camera);
        dm_check("ui:page-rendered", g_hash_home != g_hash_camera, g_detail);

        dm_selftest_snapshot("dm_camera");

        {
          int x = 0;
          int y = 0;

          if (dm_ui_tab_target("net", &x, &y) != 0)
            {
              g_state = DM_ST_DONE;
              return;
            }

          dm_touch_tap(x, y);
        }

        g_frames = DM_SELFTEST_PAGE_FRAMES;
        g_state  = DM_ST_GOTO_NET;
        return;

      case DM_ST_GOTO_NET:
        if (g_frames > 0)
          {
            g_frames--;
            return;
          }

        {
          char desc[160];
          const struct dm_net_status_s *net;

          (void)dm_net_refresh();
          net = dm_net_status();
          (void)dm_net_describe(desc, sizeof(desc));
          dm_check("ui:net-page",
                   dm_ui_current_id() != NULL &&
                   strcmp(dm_ui_current_id(), "net") == 0, desc);
          dm_check("net:interfaces", net != NULL && net->count > 0, desc);
        }

        dm_selftest_snapshot("dm_net");
        g_state = DM_ST_NET_WAIT;
        g_frames = 2;
        return;

      case DM_ST_NET_WAIT:
        if (g_frames > 0)
          {
            g_frames--;
            return;
          }

        /* Picture statistics of the net page: same criteria as the board. */

        {
          struct dm_display_stats_s stats;

          memset(&stats, 0, sizeof(stats));

          if (dm_display_stats(&stats) != 0)
            {
              dm_check("snapshot:picture", false, "stats failed");
            }
          else
            {
              snprintf(g_detail, sizeof(g_detail),
                       "distinct=%d dominant=%d%% grad=%d n=%d",
                       stats.distinct, stats.dominant_pct, stats.grad_mean,
                       stats.sampled);

              dm_check("snapshot:picture",
                       stats.distinct >= 3 && stats.dominant_pct <= 90 &&
                       stats.grad_mean <= 8, g_detail);
            }
        }

        g_state = DM_ST_DONE;
        return;

      default:
        return;
    }
}

bool dm_selftest_active(void)
{
  return g_state != DM_ST_DONE && g_state != DM_ST_IDLE;
}

int dm_selftest_result(void)
{
  return g_result;
}

/* Called by the main loop once the scenario reports itself finished, so the
 * PASS/FAIL line is printed exactly once.
 */

void dm_selftest_finish(void)
{
  if (g_result >= 0)
    {
      return;
    }

  if (g_failures == 0)
    {
      g_result = 0;
      printf("DM-SELFTEST RESULT: PASS (%d/%d)\n", g_checks, g_checks);
    }
  else
    {
      g_result = -1;
      printf("DM-SELFTEST RESULT: FAIL (%d/%d failed)\n", g_failures, g_checks);
    }

  fflush(stdout);
}
