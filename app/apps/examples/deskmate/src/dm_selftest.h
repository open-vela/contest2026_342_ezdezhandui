/****************************************************************************
 * apps/examples/deskmate/src/dm_selftest.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Headless self-test: the evidence generator.
 *
 * The scenario is a small state machine stepped from the UI loop (so it runs
 * on the LVGL thread and needs no locking):
 *
 *   1. display backend ready
 *   2. LVGL major version is the one the app was written for
 *   3. the widget tree got built
 *   4. an injected tap is delivered to the button and updates the model
 *   5. a snapshot is written and its size matches the frame geometry
 *   6. the snapshot is a drawn picture, not a blank or a noise field
 *
 * Every check prints one "[dm-check] <name> PASS/FAIL" line and the run ends
 * with "DM-SELFTEST RESULT: PASS (n/n)" or FAIL, which is what the host
 * scripts grep for.
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_DESKMATE_DM_SELFTEST_H
#define __APPS_EXAMPLES_DESKMATE_DM_SELFTEST_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Start the scenario.  snap_path may be NULL for the default location. */

void dm_selftest_begin(const char *snap_path);

/* 把自检输出再写一份到文件（板上串口常被驱动日志淹没，报告文件才是可靠证据）。 */

void dm_selftest_set_report(const char *path);

/* Advance the scenario by one UI loop iteration. */

void dm_selftest_step(void);

/* True while the scenario still needs loop iterations. */

bool dm_selftest_active(void);

/* Print the final PASS/FAIL line.  Call once the scenario finished; calling it
 * twice is harmless.
 */

void dm_selftest_finish(void);

/* ── tour mode ────────────────────────────────────────────────────────────
 * Not a test: a scripted walk through every page that snapshots each of them
 * (and a short burst of camera frames) so a run can be turned into
 * screenshots or an animation on the host.  Same loop, no assertions.
 */

void dm_tour_begin(const char *snap_path);
void dm_tour_step(void);
bool dm_tour_active(void);
void dm_tour_finish(void);

/* Result of a finished run: 0 = all checks passed.  Only meaningful once
 * dm_selftest_active() returned false.
 */

int dm_selftest_result(void);

#endif /* __APPS_EXAMPLES_DESKMATE_DM_SELFTEST_H */
