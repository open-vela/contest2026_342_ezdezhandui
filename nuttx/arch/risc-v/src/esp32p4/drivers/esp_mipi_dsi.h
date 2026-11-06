/****************************************************************************
 * arch/risc-v/src/common/espressif/esp_mipi_dsi.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#ifndef __ARCH_RISCV_SRC_COMMON_ESPRESSIF_ESP_MIPI_DSI_H
#define __ARCH_RISCV_SRC_COMMON_ESPRESSIF_ESP_MIPI_DSI_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <nuttx/video/mipi_dsi.h>

#ifdef CONFIG_ESPRESSIF_MIPI_DSI

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* SoC bus parameters. Board/panel code may override Kconfig defaults. */

struct esp_mipi_dsi_bus_config_s
{
  uint8_t  num_data_lanes;      /* 1..2; 0 = use CONFIG_ESPRESSIF_MIPI_DSI_LANES */
  uint32_t lane_bit_rate_mbps;  /* 0 = use CONFIG_ESPRESSIF_MIPI_DSI_LANE_BITRATE_MBPS */
};

/* DPI / video-mode timing for esp_mipi_dsi_configure_dpi().
 * Filled by board or panel drivers — the arch host has no panel defaults.
 */

struct esp_mipi_dsi_dpi_config_s
{
  uint16_t h_res;               /* Active width (pixels) */
  uint16_t v_res;               /* Active height (lines) */
  uint16_t hsync_pulse_width;
  uint16_t hsync_back_porch;
  uint16_t hsync_front_porch;
  uint16_t vsync_pulse_width;
  uint16_t vsync_back_porch;
  uint16_t vsync_front_porch;
  uint32_t dpi_clock_freq_mhz;  /* Expected DPI pixel clock (MHz) */
  uint8_t  virtual_channel;     /* DPI virtual channel (0..3) */
  uint8_t  format;              /* MIPI_DSI_FMT_RGB565 / RGB888 / ... */
};

/* DSI health counters, gathered in one call so a watchdog or a self-test does
 * not have to race four separate getters across a live refresh.
 */

struct esp_mipi_dsi_stats_s
{
  uint32_t dma_isr;             /* TFR_DONE interrupts == panel refreshes */
  uint32_t dma_restart;         /* times the channel had to be re-armed */
  uint32_t dma_unknown_isr;     /* ISR entries that were not a refresh */
  uint32_t bridge_underrun;     /* bridge FIFO starved (sticky, polled) */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifndef __ASSEMBLY__

#ifdef __cplusplus
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

/****************************************************************************
 * Name: esp_mipi_dsi_initialize
 *
 * Description:
 *   Enable clocks/resets for the MIPI-DSI host, PHY and bridge; configure
 *   the PHY PLL for the selected lane bitrate; leave the controller in
 *   command mode for LP DCS/generic transfers; and register the host via
 *   mipi_dsi_host_register().
 *
 *   Does not power VDD_MIPI_DPHY — board code must enable the MIPI PHY
 *   supply (e.g. on-chip LDO) before calling this.
 *
 * Input Parameters:
 *   cfg - Optional bus config (lanes / bitrate). NULL uses Kconfig defaults.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int esp_mipi_dsi_initialize(
      FAR const struct esp_mipi_dsi_bus_config_s *cfg);

/****************************************************************************
 * Name: esp_mipi_dsi_host_get
 *
 * Description:
 *   Return the registered Espressif MIPI-DSI host, or NULL if not
 *   initialized.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   Pointer to the registered mipi_dsi_host on success; NULL if
 *   esp_mipi_dsi_initialize() has not completed successfully.
 *
 ****************************************************************************/

FAR struct mipi_dsi_host *esp_mipi_dsi_host_get(void);

/****************************************************************************
 * Name: esp_mipi_dsi_configure_dpi
 *
 * Description:
 *   Configure DPI clock divider and host/bridge video timing for later
 *   HS video mode. Does not start streaming and does not bind a
 *   framebuffer.
 *
 * Input Parameters:
 *   cfg - DPI timing / format (must not be NULL; board/panel supplies this)
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int esp_mipi_dsi_configure_dpi(
      FAR const struct esp_mipi_dsi_dpi_config_s *cfg);

/****************************************************************************
 * Name: esp_mipi_dsi_bind_framebuffer
 *
 * Description:
 *   Bind a RGB framebuffer in memory to the DSI bridge via DW-GDMA so that
 *   esp_mipi_dsi_video_start() can stream pixels continuously.  Call after
 *   esp_mipi_dsi_configure_dpi().  fb must remain valid while video runs;
 *   after CPU writes, callers should call
 *   esp_mipi_dsi_flush_framebuffer() on the dirty region before expecting
 *   the panel to show new pixels.
 *
 * Input Parameters:
 *   fb      - Framebuffer base (DMA-capable, typically PSRAM)
 *   fb_size - Size in bytes (h_res * v_res * bpp / 8)
 *   h_res   - Active width in pixels
 *   v_res   - Active height in lines
 *   bpp     - Bits per pixel (16 for RGB565, 24 for RGB888)
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int esp_mipi_dsi_bind_framebuffer(FAR void *fb, size_t fb_size,
                                  uint16_t h_res, uint16_t v_res,
                                  uint8_t bpp);

/****************************************************************************
 * Name: esp_mipi_dsi_flush_framebuffer
 *
 * Description:
 *   Cache write-back (C2M) so DW-GDMA sees CPU stores to the framebuffer.
 *   Call after CPU writes to a bound FB (full or partial region).
 *
 * Input Parameters:
 *   addr - Start of the dirty region (typically within the bound FB)
 *   len  - Length in bytes
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int esp_mipi_dsi_flush_framebuffer(FAR void *addr, size_t len);

/****************************************************************************
 * Name: esp_mipi_dsi_video_start
 *
 * Description:
 *   Switch the host to video mode and enable bridge DPI output.  If a
 *   framebuffer was bound, also starts DW-GDMA streaming.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int esp_mipi_dsi_video_start(void);

/****************************************************************************
 * Name: esp_mipi_dsi_video_stop
 *
 * Description:
 *   Disable bridge DPI output and return the host to command mode for
 *   further LP transfers.  Stops DW-GDMA if running.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int esp_mipi_dsi_video_stop(void);

/****************************************************************************
 * Name: esp_mipi_dsi_dma_isr_count
 *
 * Description:
 *   Number of DW-GDMA transfer-complete interrupts seen for the DSI channel.
 *   This increments once per completed framebuffer transfer, i.e. once per
 *   panel refresh, and is the only signal that proves the panel is still
 *   being fed.  The framebuffer contents cannot prove it: LVGL keeps drawing
 *   into PSRAM even when the DSI channel has stopped.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   Monotonically increasing counter (wraps at 2^32).
 *
 ****************************************************************************/

uint32_t esp_mipi_dsi_dma_isr_count(void);

/****************************************************************************
 * Name: esp_mipi_dsi_dma_restart_count
 *
 * Description:
 *   Number of times the DSI DMA path had to be re-armed by the driver.  A
 *   steadily growing value means the channel keeps dying and being rescued.
 *
 ****************************************************************************/

uint32_t esp_mipi_dsi_dma_restart_count(void);

/****************************************************************************
 * Name: esp_mipi_dsi_dma_unknown_isr_count
 *
 * Description:
 *   Number of entries into the DSI DMA interrupt handler that were not a
 *   normal transfer-complete.  The DW-GDMA interrupt is shared with the CSI
 *   channel at the interrupt-matrix level, so this separates "the DSI channel
 *   is spamming" from "another channel owns this interrupt".
 *
 ****************************************************************************/

uint32_t esp_mipi_dsi_dma_unknown_isr_count(void);

/****************************************************************************
 * Name: esp_mipi_dsi_get_stats
 *
 * Description:
 *   Snapshot every DSI health counter in one call.
 *
 * Input Parameters:
 *   stats - Destination; NULL is ignored.
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void esp_mipi_dsi_get_stats(FAR struct esp_mipi_dsi_stats_s *stats);

/****************************************************************************
 * Name: esp_mipi_dsi_bridge_underrun_count
 *
 * Description:
 *   Read, clear and accumulate the MIPI-DSI bridge FIFO underrun flag.
 *
 *   The flag means the DPI output asked for pixels that DW-GDMA had not
 *   delivered yet -- i.e. the panel was actually starved.  It is sticky, and
 *   the driver does not service it from the DMA ISR (that handler owns the
 *   DW-GDMA channel, not the bridge), so callers are expected to poll this at
 *   a modest rate.  The display watchdog's 500 ms tick is the intended caller.
 *
 *   Note: this is *not* the same as a cache-coherency stall or a stopped DMA
 *   channel; those show up in dma_isr / dma_restart instead.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   Monotonically increasing underrun count (wraps at 2^32).
 *
 ****************************************************************************/

uint32_t esp_mipi_dsi_bridge_underrun_count(void);

/****************************************************************************
 * Name: esp_mipi_dsi_video_dma_is_active
 *
 * Description:
 *   Report whether DW-GDMA is currently streaming framebuffer pixels to the
 *   DSI bridge.
 *
 *   The CSI driver calls this while initialising, because the two peripherals
 *   share one DW-GDMA controller: tearing the controller down under a running
 *   scan-out is what leaves the panel frozen.  Before this existed the CSI
 *   side had to infer the state from the GDMA bus clock, which is a heuristic
 *   and not the same question.
 *
 * Input Parameters:
 *   None
 *
 * Returned Value:
 *   true when video mode is up and the DMA channel is armed.
 *
 ****************************************************************************/

bool esp_mipi_dsi_video_dma_is_active(void);

/****************************************************************************
 * Name: esp_mipi_dsi_queue_frame_buffer
 *
 * Description:
 *   Ask for `fb` to be scanned out starting at the next frame boundary.
 *
 *   Required for double buffering: the source address is only changed inside
 *   the DMA ISR, so the panel never scans a half-written page.  The most
 *   recent request wins if several arrive before a boundary.
 *
 * Input Parameters:
 *   fb  - Page to display; must lie inside the bound framebuffer
 *   len - Its length in bytes
 *
 * Returned Value:
 *   Zero (OK); -EINVAL if the page is outside the bound FB; -ESHUTDOWN if
 *   video mode is not running.
 *
 ****************************************************************************/

int esp_mipi_dsi_queue_frame_buffer(FAR const void *fb, size_t len);

/****************************************************************************
 * Name: esp_mipi_dsi_set_frame_done_callback
 *
 * Description:
 *   Register a per-frame callback, invoked from ISR context after the frame
 *   boundary hand-off.  The board layer uses it to call fb_notify_vsync().
 *
 * Input Parameters:
 *   cb  - Callback, or NULL to unregister
 *   arg - Opaque argument
 *
 * Returned Value:
 *   Zero (OK).
 *
 ****************************************************************************/

int esp_mipi_dsi_set_frame_done_callback(void (*cb)(FAR void *),
                                         FAR void *arg);

/****************************************************************************
 * Name: esp_mipi_dsi_frame_seq
 *
 * Description:
 *   Number of frames completed since boot.  Differs from
 *   esp_mipi_dsi_dma_isr_count() in that it only counts *completed* frames,
 *   which is what a vsync consumer cares about.
 *
 ****************************************************************************/

uint32_t esp_mipi_dsi_frame_seq(void);

/****************************************************************************
 * Name: esp_mipi_dsi_set_fb_pages
 *
 * Description:
 *   Declare how many pages the bound framebuffer region holds.
 *
 *   esp_mipi_dsi_bind_framebuffer()'s size argument is the bytes-per-frame
 *   (one page) because it also programmes the DMA transfer length.  When the
 *   board allocates N pages for double buffering, it must say so here so that
 *   esp_mipi_dsi_queue_frame_buffer() accepts any of the pages.
 *
 * Input Parameters:
 *   pages - Number of pages (0 is treated as 1)
 *
 * Returned Value:
 *   None
 *
 ****************************************************************************/

void esp_mipi_dsi_set_fb_pages(uint8_t pages);

#undef EXTERN
#ifdef __cplusplus
}
#endif

#endif /* __ASSEMBLY__ */
#endif /* CONFIG_ESPRESSIF_MIPI_DSI */
#endif /* __ARCH_RISCV_SRC_COMMON_ESPRESSIF_ESP_MIPI_DSI_H */
