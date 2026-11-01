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
#include "hal/dm_touch.h"
#include "ui/dm_ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Loop iterations granted to LVGL between injecting a tap and checking its
 * effect: the indev is sampled inside lv_timer_handler().
 */

#define DM_SELFTEST_TAP_FRAMES 8

#define DM_SELFTEST_MAX_CHECKS 8

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum dm_selftest_state_e
{
  DM_ST_IDLE = 0,
  DM_ST_TAP_WAIT,
  DM_ST_DONE
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static enum dm_selftest_state_e g_state = DM_ST_IDLE;
static char g_snap_path[128];
static int  g_frames;
static int  g_checks;
static int  g_failures;
static int  g_result = -1;
static int  g_taps_before;

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

void dm_selftest_begin(const char *snap_path)
{
  g_checks    = 0;
  g_failures  = 0;
  g_frames    = 0;
  g_result    = -1;
  g_state     = DM_ST_TAP_WAIT;

  if (snap_path != NULL && snap_path[0] != '\0')
    {
      strlcpy(g_snap_path, snap_path, sizeof(g_snap_path));
    }
  else
    {
      strlcpy(g_snap_path, DM_SNAP_HOST_DIR "/dm_home.bin",
              sizeof(g_snap_path));
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
    char detail[48];
    int objects = dm_ui_object_count();

    snprintf(detail, sizeof(detail), "%d objects", objects);
    dm_check("ui:built", objects >= 3, detail);
  }

  /* Touch: aim at the button and let LVGL consume the sample. */

  {
    int x = 0;
    int y = 0;

    g_taps_before = dm_ui_tap_count();

    if (dm_ui_tap_target(&x, &y) != 0)
      {
        dm_check("touch:target", false, "no tap target");
        g_frames = 0;
      }
    else
      {
        char detail[48];

        snprintf(detail, sizeof(detail), "(%d,%d)", x, y);
        dm_check("touch:target", x > 0 && y > 0, detail);
        dm_touch_tap(x, y);
        g_frames = DM_SELFTEST_TAP_FRAMES;
      }
  }

  DM_LOG("selftest: scenario started, snapshot target %s", g_snap_path);
}

void dm_selftest_step(void)
{
  if (g_state != DM_ST_TAP_WAIT)
    {
      return;
    }

  if (g_frames > 0)
    {
      g_frames--;
      return;
    }

  /* Did the tap reach the widget? */

  {
    int now = dm_ui_tap_count();
    char detail[48];

    snprintf(detail, sizeof(detail), "%d -> %d", g_taps_before, now);
    dm_check("touch:delivered", now > g_taps_before, detail);
  }

  /* Snapshot and picture statistics. */

  {
    const struct dm_display_metrics_s *metrics = dm_display_metrics();
    int written;

    written = dm_display_snapshot(g_snap_path);

    if (metrics == NULL)
      {
        dm_check("snapshot:written", false, "no metrics");
      }
    else
      {
        char detail[96];

        snprintf(detail, sizeof(detail), "%d bytes (expected %u)", written,
                 (unsigned)metrics->pixels_size);
        dm_check("snapshot:written",
                 written > 0 && (size_t)written == metrics->pixels_size,
                 detail);
      }

    {
      struct dm_display_stats_s stats;

      memset(&stats, 0, sizeof(stats));

      if (dm_display_stats(&stats) != 0)
        {
          dm_check("snapshot:picture", false, "stats failed");
        }
      else
        {
          char detail[96];

          snprintf(detail, sizeof(detail),
                   "distinct=%d dominant=%d%% grad=%d n=%d",
                   stats.distinct, stats.dominant_pct, stats.grad_mean,
                   stats.sampled);

          /* A drawn UI has several colours, no single colour covering the
           * whole frame, and smooth (low-gradient) areas; a blank frame has
           * one colour and a noise field has a huge gradient.
           */

          dm_check("snapshot:picture",
                   stats.distinct >= 3 && stats.dominant_pct <= 90 &&
                   stats.grad_mean <= 8, detail);
        }
    }
  }

  g_state = DM_ST_DONE;

  if (g_failures == 0)
    {
      g_result = 0;
      printf("DM-SELFTEST RESULT: PASS (%d/%d)\n", g_checks, g_checks);
    }
  else
    {
      g_result = -1;
      printf("DM-SELFTEST RESULT: FAIL (%d/%d failed)\n", g_failures,
             g_checks);
    }

  fflush(stdout);
}

bool dm_selftest_active(void)
{
  return g_state != DM_ST_DONE && g_state != DM_ST_IDLE;
}

int dm_selftest_result(void)
{
  return g_result;
}
