/****************************************************************************
 * apps/examples/deskmate/src/hal/dm_display.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Display half of the DeskMate hardware abstraction layer.
 *
 * Two backends, chosen by Kconfig:
 *
 *   fbdev : LVGL drives the board framebuffer through the NuttX glue
 *           (lv_nuttx_fbdev_create) and the touchscreen indev comes from
 *           /dev/input0.  Used on the ESP32-P4 panel and on goldfish.
 *
 *   mem   : LVGL renders into a full-screen buffer owned by the application.
 *           Used headless (simulator without X11, CI) and as the
 *           deterministic snapshot source.
 *
 * Both backends expose the same snapshot API, which writes raw pixels plus a
 * sidecar description that the host tools (tools/test/fb2png.py) consume.
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_DESKMATE_HAL_DM_DISPLAY_H
#define __APPS_EXAMPLES_DESKMATE_HAL_DM_DISPLAY_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct dm_display_metrics_s
{
  int      width;         /* Visible width in pixels */
  int      height;        /* Visible height in pixels */
  int      bpp;           /* Bits per pixel of the pixel store */
  int      stride;        /* Bytes per line */
  bool     memory;        /* true: pixels owned by the app (mem backend) */
  uint8_t *pixels;        /* First visible pixel of the current frame */
  size_t   pixels_size;   /* Bytes from pixels to the end of the frame */
  const char *backend;    /* "fbdev" or "mem" */
};

/* Sampled picture statistics, used both for self-test and for evidence.
 *
 * grad_mean follows the convention of tools/test/fb2png.py: the mean absolute
 * luminance difference between horizontally adjacent pixels.  A drawn UI sits
 * below ~8, a noisy or unsynchronised panel is ~60 or more.
 */

struct dm_display_stats_s
{
  int  distinct;          /* Distinct colours found in the sample */
  int  dominant_pct;      /* Share of the most frequent colour, in percent */
  int  grad_mean;         /* Mean |luminance delta| between adjacent pixels */
  int  sampled;           /* Pixels inspected */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Bring up LVGL for DeskMate: initialise the LVGL core entry (tick, caches)
 * and create the display for the selected backend.  lv_init() must have been
 * called by the caller.  Returns 0 on success.  dm_display_create() must be
 * called before any widget is created.
 */

int dm_display_create(void);

/* Current display metrics; NULL before dm_display_create(). */

const struct dm_display_metrics_s *dm_display_metrics(void);

/* LVGL display object (for lv_display_set_* by the UI layer). */

void *dm_display_lv(void);

/* Write the current frame as raw pixels, plus "<path>.txt" describing it:
 *
 *   width=<w>
 *   height=<h>
 *   bpp=<bpp>
 *   stride=<bytes>
 *   backend=<fbdev|mem>
 *
 * Returns the number of pixel bytes written, or a negated errno on failure.
 */

int dm_display_snapshot(const char *path);

/* Picture statistics over the current frame (subsampled, cheap). */

int dm_display_stats(struct dm_display_stats_s *stats);

/* Cheap content fingerprint of the current frame (sampled FNV-1a).  Two frames
 * with different UI have different hashes, which is how the self-test proves
 * that navigating between pages really changed what is on screen.
 */

uint32_t dm_display_frame_hash(void);

/* Tear the display down again (LVGL objects are deleted by the caller). */

void dm_display_destroy(void);

/* 显示提交计数 / 最近一次提交时间（ms）：display:fed 判据用。 */

uint32_t dm_display_commits(void);
uint32_t dm_display_last_ms(void);

/* **面板**保活计数 —— 这是唯一无法伪造的显示证据。
 *
 * dm_display_commits() 数的是 LVGL 把画面画进 PSRAM 的次数；面板（DSI 链路）
 * 停摆时 LVGL 完全不知道，计数照涨。于是"面板变纯色但统计全绿"这种情况，
 * 原来所有判据都发现不了（真机 2026-11-06 复现）。
 *
 * 这个函数返回 DSI 的 DMA 传输完成中断计数：每个刷新周期 +1，链路一断就冻结。
 * 返回值 0 表示该平台没有这个信息源（此时调用方应回退到 commits）。
 */

uint32_t dm_display_panel_beat(void);

/* 面板「饥饿」计数：DSI 桥的 FIFO underrun 次数。
 *
 * beat 只证明 DMA 通道还在跑；这个数证明**画面有没有断供** —— 相机抢带宽时
 * 它会涨而 beat 可能仍然正常。读这个函数会清硬件中断标志，所以只应由显示
 * 看门狗（500ms）这样的固定节奏调用；返回 0 表示该平台没有这个信息源。
 */

uint32_t dm_display_panel_underrun(void);

#endif /* __APPS_EXAMPLES_DESKMATE_HAL_DM_DISPLAY_H */
