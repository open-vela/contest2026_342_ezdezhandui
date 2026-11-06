/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_display.c
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

/* /dev/fb0 glue for the ESP32-P4X-C5-Function-EV-Board's EK79007 MIPI-DSI
 * panel.  Mirrors the M5Stack Tab5 bring-up order:
 *
 *   1. Allocate the RGB565 framebuffer (PSRAM: 1024*600*2 = 1.2 MB, far too
 *      large for the internal SRAM heap)
 *   2. esp_mipi_dsi_configure_dpi()
 *   3. EK79007 DCS init (board_ek79007_initialize)
 *   4. esp_mipi_dsi_bind_framebuffer()  -> DW-GDMA streams FB to the bridge
 *   5. sleep-out, then esp_mipi_dsi_video_start()
 *   6. dcs_set_display_on()
 *   7. backlight on
 */

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <debug.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/compiler.h>
#include <nuttx/kmalloc.h>
#include <nuttx/video/fb.h>
#include <nuttx/video/mipi_display.h>
#include <nuttx/video/mipi_dsi.h>

#include <arch/board/board.h>

#include "drivers/esp_mipi_dsi.h"

#include "esp32p4-function-ev-board.h"
#include "esp32p4_lcd_ek79007.h"

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_LCD

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BOARD_FB_BPP       16
#define BOARD_FB_WIDTH     BOARD_MIPI_DSI_H_RES
#define BOARD_FB_HEIGHT    BOARD_MIPI_DSI_V_RES
#define BOARD_FB_STRIDE    (BOARD_FB_WIDTH * (BOARD_FB_BPP / 8))
#define BOARD_FB_PAGE_SIZE (BOARD_FB_STRIDE * BOARD_FB_HEIGHT)

/* 双缓冲页数。
 *
 * LVGL 的 nuttx fbdev 后端判断双缓冲的**唯一**条件是
 *   yres_virtual == yres * 2
 * （lv_nuttx_fbdev.c: `double_buffer = pinfo.yres_virtual == yres * 2`）。
 * 它随后会用 `pinfo.display + 1` 去探测第二页并调用 FBIOPAN_DISPLAY，所以
 * 这里必须是 2，并且 board_getplaneinfo() 必须真的返回第二页的地址。
 *
 * 单缓冲时同一块 1.2 MB 内存既是渲染目标又是 DMA 源：LVGL 一边写、面板一边
 * 扫，撕裂无法消除；而且每次脏区回写都落在同一块上，实测尖峰 80 ms。
 */

#define BOARD_FB_COUNT     2
#define BOARD_FB_SIZE      (BOARD_FB_PAGE_SIZE * BOARD_FB_COUNT)

/* Solid red as the bring-up test colour: proves the DSI link, the panel
 * controller and the backlight all work without needing any app to run.
 */

#define BOARD_FB_TEST_COLOR  0xf800

/* All-Pixels-On / -Off DCS (used by the optional bring-up flash) */

#define BOARD_DCS_ALL_PIXELS_ON   0x23
#define BOARD_DCS_ALL_PIXELS_OFF  0x22
#define BOARD_APO_TEST_MS         1000

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static int board_getvideoinfo(FAR struct fb_vtable_s *vtable,
                              FAR struct fb_videoinfo_s *vinfo);
static int board_getplaneinfo(FAR struct fb_vtable_s *vtable, int planeno,
                              FAR struct fb_planeinfo_s *pinfo);
#ifdef CONFIG_FB_UPDATE
static int board_updatearea(FAR struct fb_vtable_s *vtable,
                            FAR const struct fb_area_s *area);
#endif
static int board_pandisplay(FAR struct fb_vtable_s *vtable,
                            FAR struct fb_planeinfo_s *pinfo);
static void board_frame_done(FAR void *arg);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct fb_vtable_s g_board_vtable =
{
  .getvideoinfo = board_getvideoinfo,
  .getplaneinfo = board_getplaneinfo,
#ifdef CONFIG_FB_UPDATE
  .updatearea   = board_updatearea,
#endif
  .pandisplay   = board_pandisplay,
};

static struct fb_videoinfo_s g_board_video =
{
  .fmt     = FB_FMT_RGB16_565,
  .xres    = BOARD_FB_WIDTH,
  .yres    = BOARD_FB_HEIGHT,
  .nplanes = 1,
};

static struct fb_planeinfo_s g_board_plane =
{
  .fbmem        = NULL,
  .fblen        = BOARD_FB_SIZE,
  .stride       = BOARD_FB_STRIDE,
  .display      = 0,
  .bpp          = BOARD_FB_BPP,
  .xres_virtual = BOARD_FB_WIDTH,
  .yres_virtual = BOARD_FB_HEIGHT * BOARD_FB_COUNT,
  .xoffset      = 0,
  .yoffset      = 0,
};

static FAR uint16_t *g_board_fb;
static bool g_board_fb_ready;
static volatile uint32_t g_board_frame_done;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: board_getvideoinfo
 *
 * Description:
 *   Return the video information.
 *
 * Input Parameters:
 *   vtable - The vtable.
 *   vinfo  - The returned video information.
 *
 * Returned Value:
 *   Zero (OK) on success.
 *
 ****************************************************************************/

static int board_getvideoinfo(FAR struct fb_vtable_s *vtable,
                              FAR struct fb_videoinfo_s *vinfo)
{
  DEBUGASSERT(vtable != NULL && vtable == &g_board_vtable && vinfo != NULL);
  memcpy(vinfo, &g_board_video, sizeof(*vinfo));
  return OK;
}

/****************************************************************************
 * Name: board_getplaneinfo
 *
 * Description:
 *   Return the plane information.
 *
 * Input Parameters:
 *   vtable  - The vtable.
 *   planeno - The plane number.
 *   pinfo   - The returned plane information.
 *
 * Returned Value:
 *   Zero (OK) on success; -EINVAL if the plane number is invalid.
 *
 ****************************************************************************/

static int board_getplaneinfo(FAR struct fb_vtable_s *vtable, int planeno,
                              FAR struct fb_planeinfo_s *pinfo)
{
  DEBUGASSERT(vtable != NULL && vtable == &g_board_vtable && pinfo != NULL);

  if (planeno != 0 || g_board_plane.fbmem == NULL)
    {
      return -EINVAL;
    }

  memcpy(pinfo, &g_board_plane, sizeof(*pinfo));

  /* 所有页都返回**同一个基址和整块 fblen**，这是 LVGL 的"连续布局"约定。
   *
   * lv_nuttx_fbdev.c 的 fbdev_init_mem2() 里：
   *   offset        = phy_mem2 - phy_mem1
   *   is_consecutive = (offset == 0)
   * 只有在 offset == 0 时它才把 fblen 减半、并**自己**用
   * `yres * stride` 作为 mmap 偏移去取第二页。
   *
   * 反过来，如果这里按 display 返回两个不同地址，offset = 1228800，
   * is_consecutive 为 false，于是它拿整块 fblen 去 mmap 一个已经偏移过的
   * 位置 —— 落到 fb_mmap() 的 `offset + length <= fblen` 检查上直接 -EINVAL，
   * 表现为 "LVGL could not attach to /dev/fb0"（实测）。
   *
   * 所以：基址不变、fblen = 整块、yres_virtual = 2 * yres，让上层自己算页。
   */

  return OK;
}

#ifdef CONFIG_FB_UPDATE
/****************************************************************************
 * Name: board_updatearea
 *
 * Description:
 *   Write the dirty region back to memory so that DW-GDMA picks it up.
 *
 * Input Parameters:
 *   vtable - The vtable.
 *   area   - The area to update (NULL means the whole plane).
 *
 * Returned Value:
 *   Zero (OK) on success; -EAGAIN if the framebuffer is not ready.
 *
 ****************************************************************************/

static int board_updatearea(FAR struct fb_vtable_s *vtable,
                            FAR const struct fb_area_s *area)
{
  size_t offset;
  size_t len;
  unsigned int page;

  DEBUGASSERT(vtable != NULL && vtable == &g_board_vtable);

  if (g_board_fb == NULL)
    {
      return -EAGAIN;
    }

  if (area == NULL)
    {
      return esp_mipi_dsi_flush_framebuffer(g_board_fb, BOARD_FB_SIZE);
    }

  /* 越界一律 -EINVAL，不再静默截断 —— 静默截断会让"少刷一块"变成偶发花屏，
   * 是最难查的一类问题。
   */

  /* 坐标是**虚拟帧缓冲**坐标：LVGL 在校验之后会把 dirty_area.y1 加上
   * mem2_yoffset（= yres）再下发，所以第二页的 y 落在 [600, 1200)。
   */

  if (area->y >= BOARD_FB_HEIGHT * BOARD_FB_COUNT ||
      area->x >= BOARD_FB_WIDTH || area->w == 0 || area->h == 0)
    {
      return -EINVAL;
    }

  if ((int)area->y + (int)area->h > (int)(BOARD_FB_HEIGHT * BOARD_FB_COUNT) ||
      (int)area->x + (int)area->w > (int)BOARD_FB_WIDTH)
    {
      return -EINVAL;
    }

  /* 按 y 定位页。
   *
   * 双缓冲下脏区只落在"正在渲染的那一页"，所以基础地址要先按页偏移再加行偏移。
   * 单缓冲时代这里直接用 g_board_fb + y*stride，y 一旦 >= height（第二页）
   * 就会写到整个 framebuffer 的后半段 —— 而 DMA 仍在读第一页，等于白写。
   */

  page = (unsigned int)area->y / BOARD_FB_HEIGHT;

  if (page >= BOARD_FB_COUNT)
    {
      return -EINVAL;
    }

  offset = (size_t)(area->y % BOARD_FB_HEIGHT) * BOARD_FB_STRIDE +
           (size_t)area->x * (BOARD_FB_BPP / 8);
  len = (size_t)area->h * BOARD_FB_STRIDE;

  if (offset + len > BOARD_FB_PAGE_SIZE)
    {
      len = BOARD_FB_PAGE_SIZE - offset;
    }

  return esp_mipi_dsi_flush_framebuffer(
           (FAR uint8_t *)g_board_fb + (size_t)page * BOARD_FB_PAGE_SIZE +
           offset, len);
}

/****************************************************************************
 * Name: board_pandisplay
 *
 * Description:
 *   Show the page named by pinfo->yoffset.
 *
 *   The swap is handed to the DSI driver and happens at the next frame
 *   boundary (esp_mipi_dsi_queue_frame_buffer()), so this function must NOT
 *   touch the hardware source address itself -- doing so would change the
 *   address the bridge is reading mid-frame, i.e. produce tearing.
 *
 * Input Parameters:
 *   vtable - The vtable.
 *   pinfo  - Plane info whose yoffset names the page.
 *
 * Returned Value:
 *   Zero (OK); -EINVAL if yoffset is not page-aligned/out of range;
 *   -EAGAIN if the framebuffer is not ready.
 *
 ****************************************************************************/

static int board_pandisplay(FAR struct fb_vtable_s *vtable,
                            FAR struct fb_planeinfo_s *pinfo)
{
  FAR uint8_t *page;
  unsigned int idx;

  DEBUGASSERT(vtable != NULL && vtable == &g_board_vtable && pinfo != NULL);

  if (g_board_fb == NULL)
    {
      return -EAGAIN;
    }

  if (pinfo->yoffset < 0 || (pinfo->yoffset % BOARD_FB_HEIGHT) != 0)
    {
      return -EINVAL;
    }

  idx = (unsigned int)pinfo->yoffset / BOARD_FB_HEIGHT;
  if (idx >= BOARD_FB_COUNT)
    {
      return -EINVAL;
    }

  page = (FAR uint8_t *)g_board_fb + (size_t)idx * BOARD_FB_PAGE_SIZE;

  /* 先把该页写回内存：CPU 走 cache 写，DMA 读的是内存。 */

  (void)esp_mipi_dsi_flush_framebuffer(page, BOARD_FB_PAGE_SIZE);

  return esp_mipi_dsi_queue_frame_buffer(page, BOARD_FB_PAGE_SIZE);
}

/****************************************************************************
 * Name: board_frame_done
 *
 * Description:
 *   DSI 帧完成回调（**ISR 上下文**）。
 *
 *   目前只做计数：CONFIG_FB_SYNC 未开时没有 vsync 等待者。留着这个钩子是因为
 *   一旦将来启用 FB_SYNC / 双缓冲渲染器，这就是唯一正确的挂载点 —— 面板
 *   扫完一页的那一刻，前页才真正可以复用。
 *
 ****************************************************************************/

static void board_frame_done(FAR void *arg)
{
  UNUSED(arg);

  g_board_frame_done++;

  /* 消费 fb.c 的 pan 队列并唤醒 vsync 等待者。
   *
   * 这一步**不能省**。LVGL 每帧发一次 FBIOPAN_DISPLAY，fb.c 会把这条请求压进
   * 一个环形队列（circbuf）。队列只有消费掉才会重新 non-full，而
   * fb_poll() 只在 `!circbuf_is_full(panbuf)` 时报 POLLOUT ——
   * lv_nuttx_fbdev 的刷新定时器正是用 POLLOUT 来判断"有没有空闲页"：
   *
   *   if (poll(...) == 0 && 没 POLLOUT && yres_virtual == yres*2) return;
   *
   * 所以没人消费队列时，双缓冲下 LVGL 会**永久停止刷新**。
   * 实测症状就是：开双缓冲后 commits 卡在 2、面板保活计数归零、看门狗 6 秒后
   * 误判停摆并去重建 DSI，然后在共享 GDMA 上撞 Exception 7。
   *
   * 这里在每帧完成（面板正好扫完一页）时消费一条，语义上就是"这一页用完了"。
   */

  (void)fb_remove_paninfo(&g_board_vtable, FB_NO_OVERLAY);
  (void)fb_notify_vsync(&g_board_vtable);
}
#endif /* CONFIG_FB_UPDATE */

/****************************************************************************
 * Name: board_fb_fill
 *
 * Description:
 *   Fill the whole plane with a colour and write it back to memory.
 *
 * Input Parameters:
 *   color - The RGB565 colour to fill with.
 *
 ****************************************************************************/

static void board_fb_fill(uint16_t color)
{
  size_t i;
  size_t npix = (size_t)BOARD_FB_WIDTH * BOARD_FB_HEIGHT;
  int ret;

  for (i = 0; i < npix; i++)
    {
      g_board_fb[i] = color;
    }

  ret = esp_mipi_dsi_flush_framebuffer(g_board_fb, BOARD_FB_SIZE);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: FB cache write-back failed: %d\n", ret);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: up_fbinitialize
 *
 * Description:
 *   Initialize the framebuffer and bring up the panel. Called by
 *   fb_register().
 *
 * Input Parameters:
 *   display - The display number.
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int up_fbinitialize(int display)
{
  FAR struct mipi_dsi_host *host;
  FAR struct mipi_dsi_device *device;
  struct esp_mipi_dsi_dpi_config_s dpi;
  int ret;

  if (display != 0)
    {
      return -ENODEV;
    }

  if (g_board_fb_ready)
    {
      return OK;
    }

  /* 1024*600*2 = 1.2 MB: only the PSRAM heap region can satisfy this, so the
   * allocation lands in PSRAM as required for DW-GDMA streaming.
   */

  g_board_fb = kumm_memalign(64, BOARD_FB_SIZE);
  if (g_board_fb == NULL)
    {
      syslog(LOG_ERR,
             "ERROR: FB alloc failed (%u bytes; enable PSRAM)\n",
             (unsigned int)BOARD_FB_SIZE);
      return -ENOMEM;
    }

  memset(g_board_fb, 0, BOARD_FB_SIZE);
  g_board_plane.fbmem = g_board_fb;

  host = esp_mipi_dsi_host_get();
  if (host == NULL)
    {
      syslog(LOG_ERR,
             "ERROR: MIPI-DSI host not ready "
             "(enable ESP32P4_FUNCTION_EV_BOARD_MIPI_DSI)\n");
      ret = -EAGAIN;
      goto errout_fb;
    }

  /* ESP-IDF order: DPI config before the panel DCS init; display_on after
   * video mode is enabled.
   */

  syslog(LOG_INFO, "Configuring DPI %ux%u @ %u MHz...\n",
         BOARD_FB_WIDTH, BOARD_FB_HEIGHT,
         (unsigned int)BOARD_MIPI_DSI_DPI_CLK_MHZ);
  board_mipi_dsi_dpi_config(&dpi);
  ret = esp_mipi_dsi_configure_dpi(&dpi);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: configure_dpi failed: %d\n", ret);
      goto errout_fb;
    }

  syslog(LOG_INFO, "%s panel init...\n", BOARD_LCD_PANEL_NAME);
  device = board_ek79007_initialize(host);
  if (device == NULL)
    {
      ret = -EIO;
      goto errout_fb;
    }

  /* 注意第二个参数是**一帧**的字节数（= 一页），不是两块的总和。
   *
   * 这里的 fb_size 决定 DW-GDMA 每帧要搬多少字节，也决定桥的
   * `num_pixel_bits`。双缓冲时如果传整块（2 页），DMA 每帧会连扫两页 ——
   * 面板只显示前 600 行、每帧多搬 1.2 MB，实测直接导致帧完成中断不再规律、
   * 看门狗误判停摆。页的范围由 queue_frame_buffer 单独校验。
   */

  ret = esp_mipi_dsi_bind_framebuffer(g_board_fb, BOARD_FB_PAGE_SIZE,
                                      BOARD_FB_WIDTH, BOARD_FB_HEIGHT,
                                      BOARD_FB_BPP);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: bind_framebuffer failed: %d\n", ret);
      goto errout_fb;
    }

  /* 每帧完成时通知上层（ISR 上下文）。双缓冲的读者靠它知道前页已被扫完、
   * 可以安全复用。注册失败不影响出图，所以只告警。
   */

  (void)esp_mipi_dsi_set_frame_done_callback(board_frame_done, NULL);

  /* 告诉驱动这块区域有几页，queue_frame_buffer() 才会接受第 2 页及以后。 */

  esp_mipi_dsi_set_fb_pages(BOARD_FB_COUNT);

  board_fb_fill(BOARD_FB_TEST_COLOR);

  ret = mipi_dsi_dcs_exit_sleep_mode(device);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: pre-video sleep_out failed: %d\n", ret);
      goto errout_fb;
    }

  up_mdelay(120);

  syslog(LOG_INFO, "Starting DSI video...\n");
  ret = esp_mipi_dsi_video_start();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: video_start failed: %d\n", ret);
      goto errout_fb;
    }

  ret = mipi_dsi_dcs_set_display_on(device);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: post-video display_on failed: %d\n", ret);
      goto errout_fb;
    }

  board_fb_fill(BOARD_FB_TEST_COLOR);

  /* Only now is it safe to light the panel up. */

  ret = board_lcd_backlight(true);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: backlight failed: %d\n", ret);
    }

  g_board_fb_ready = true;
  syslog(LOG_INFO, "/dev/fb0 ready %ux%u RGB565 @ %p (%u bytes)\n",
         BOARD_FB_WIDTH, BOARD_FB_HEIGHT, g_board_fb,
         (unsigned int)BOARD_FB_SIZE);

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_LCD_APO_TEST
  /* All-Pixels-On flash: proves DCS + backlight even if the DSI video
   * stream itself is misconfigured.  Restores the red test fill after.
   */

  mipi_dsi_dcs_write(device, BOARD_DCS_ALL_PIXELS_ON, NULL, 0);
  up_mdelay(BOARD_APO_TEST_MS);
  mipi_dsi_dcs_write(device, BOARD_DCS_ALL_PIXELS_OFF, NULL, 0);
  mipi_dsi_dcs_write(device, MIPI_DCS_ENTER_NORMAL_MODE, NULL, 0);
  mipi_dsi_dcs_set_display_on(device);
  board_fb_fill(BOARD_FB_TEST_COLOR);
#endif

  return OK;

errout_fb:
  kumm_free(g_board_fb);
  g_board_fb = NULL;
  g_board_plane.fbmem = NULL;
  return ret;
}

/****************************************************************************
 * Name: up_fbgetvplane
 *
 * Description:
 *   Return the framebuffer vtable for the requested plane.
 *
 * Input Parameters:
 *   display - The display number.
 *   vplane  - The vplane number.
 *
 * Returned Value:
 *   Pointer to the vtable on success; NULL on failure.
 *
 ****************************************************************************/

FAR struct fb_vtable_s *up_fbgetvplane(int display, int vplane)
{
  if (display != 0 || vplane != 0 || !g_board_fb_ready)
    {
      return NULL;
    }

  return &g_board_vtable;
}

/****************************************************************************
 * Name: up_fbuninitialize
 *
 * Description:
 *   Uninitialize the framebuffer.
 *
 * Input Parameters:
 *   display - The display number.
 *
 ****************************************************************************/

void up_fbuninitialize(int display)
{
  UNUSED(display);
}

/****************************************************************************
 * Name: board_lcd_reload_test_pattern
 *
 * Description:
 *   Call after fb_register(): the generic FB driver memsets the plane and
 *   would otherwise leave a black DMA buffer.
 *
 * Returned Value:
 *   Zero (OK) on success; -EAGAIN if the plane is not ready.
 *
 ****************************************************************************/

int board_lcd_reload_test_pattern(void)
{
  if (!g_board_fb_ready || g_board_fb == NULL)
    {
      return -EAGAIN;
    }

  board_fb_fill(BOARD_FB_TEST_COLOR);
  return OK;
}

#endif /* CONFIG_ESP32P4_FUNCTION_EV_BOARD_LCD */

/****************************************************************************
 * Name: esp32p4_display_reinit
 *
 * Description:
 *   "显示重建"入口。相机连续流与 DSI 抢 DW-GDMA/PSRAM 带宽时会把面板打停
 *   （dm_camera.c 里记录过的已知坑），此前只能复位整机；这个入口把视频流重新
 *   拉起来：**停流 → 重新绑定帧缓冲（重新武装 DW-GDMA）→ 开流**。
 *
 *   可安全重复调用（幂等）：停流在未启动时是空操作，绑定与开流都是重新配置。
 *
 * Returned Value:
 *   Zero (OK) on success; a negated errno value on failure.
 *
 ****************************************************************************/

int esp32p4_display_reinit(void)
{
  int ret;

  if (!g_board_fb_ready || g_board_fb == NULL)
    {
      return -EINVAL;
    }

  (void)esp_mipi_dsi_video_stop();

  /* 注意第二个参数是**一帧**的字节数（= 一页），不是两块的总和。
   *
   * 这里的 fb_size 决定 DW-GDMA 每帧要搬多少字节，也决定桥的
   * `num_pixel_bits`。双缓冲时如果传整块（2 页），DMA 每帧会连扫两页 ——
   * 面板只显示前 600 行、每帧多搬 1.2 MB，实测直接导致帧完成中断不再规律、
   * 看门狗误判停摆。页的范围由 queue_frame_buffer 单独校验。
   */

  ret = esp_mipi_dsi_bind_framebuffer(g_board_fb, BOARD_FB_PAGE_SIZE,
                                      BOARD_FB_WIDTH, BOARD_FB_HEIGHT,
                                      BOARD_FB_BPP);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: display reinit: bind failed: %d\n", ret);
      return ret;
    }

  ret = esp_mipi_dsi_video_start();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: display reinit: video_start failed: %d\n", ret);
      return ret;
    }

  syslog(LOG_INFO, "display: reinit OK (video restarted)\n");
  return OK;
}
