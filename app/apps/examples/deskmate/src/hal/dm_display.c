/****************************************************************************
 * apps/examples/deskmate/src/hal/dm_display.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Display half of the DeskMate HAL: see dm_display.h for the two backends.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <nuttx/clock.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <lvgl/lvgl.h>

#include "dm_config.h"
#include <pthread.h>
#include <unistd.h>

#include "dm_log.h"
#include "hal/dm_display.h"

#if DM_DISPLAY_FBDEV
#  include <nuttx/video/fb.h>
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct dm_display_metrics_s g_metrics;
static lv_display_t *g_disp;

/* 看门狗"代际"：每次 create()/destroy() 递增，线程只在"自己那一代"里工作，
 * 代际一变就退出。
 *
 * 为什么用代际而不是布尔停止标志（2026-11-04 真机验证）：
 *   自检退出后立刻再启动 deskmate 时，旧线程还在 500ms 的轮询睡眠里，没来得及
 *   看到停止标志；而新 create() 若把标志复位，旧线程就"复活"了 —— `ps` 里出现
 *   两个 dm-disp-wd，两个看门狗同时重建显示，把触摸/显示通路搞崩（Load access
 *   fault @ nsh_main）。代际语义没有这个竞争：旧线程看到代际变了就退出，
 *   且不可能被复位救回。
 */

static volatile uint32_t g_wd_gen;

/* 显示提交计数：LVGL 每完成一次渲染/刷写就 +1，并记录时间。
 * 自检用它做 display:fed 判据 —— "面板真的在被喂数据"，而不是只看帧缓冲内容
 * （真机踩过：帧缓冲里 UI 完好，但面板早就停了，靠内容判据分辨不出来）。
 */

volatile uint32_t g_disp_commits;

/* 失帧度量（JTAG 可读）：
 *   disp_gap_max_ms   相邻两次提交的最大间隔（ms）
 *   disp_gap_over_100 间隔 >100ms 的次数（明显卡顿/掉帧）
 *   disp_gap_over_33  间隔 >33ms 的次数（低于 ~30fps 的帧期）
 *   disp_slice_*      最近 16 帧的"切片"：哈希 + 时刻，用于按序列判断
 *                     是否有重复帧（静止/冻结）或大间隔（丢帧）
 */

volatile uint32_t g_dbg_disp_gap_max_ms;
volatile uint32_t g_dbg_disp_gap_over_100;
volatile uint32_t g_dbg_disp_gap_over_33;
volatile uint32_t g_dbg_disp_last_commit_ms;

/* 看门狗线程优先级。主循环是 CONFIG_EXAMPLES_DESKMATE_PRIORITY(100)，
 * 相机采集线程是 DM_CAMERA_THREAD_PRIORITY(90) 之下；看门狗只需要偶尔跑一下，
 * 但要跑得及时，所以放在主循环之上（数值越大优先级越高）。
 */

#define DM_DISP_WD_PRIORITY 115

#define DM_DISP_SLICE_N 16
volatile uint32_t g_dbg_disp_slice_hash[DM_DISP_SLICE_N];
volatile uint32_t g_dbg_disp_slice_ms[DM_DISP_SLICE_N];
volatile uint32_t g_dbg_disp_slice_idx;
volatile uint32_t g_dbg_disp_slice_frames;
volatile uint32_t g_disp_px;
volatile uint32_t g_disp_last_ms;

static void dm_display_render_ready_cb(lv_event_t *e)
{
  (void)e;

  uint32_t now = (uint32_t)TICK2MSEC(clock_systime_ticks());

  g_disp_commits++;

  if (g_dbg_disp_last_commit_ms != 0)
    {
      uint32_t gap = now - g_dbg_disp_last_commit_ms;

      if (gap > g_dbg_disp_gap_max_ms)
        {
          g_dbg_disp_gap_max_ms = gap;
        }

      if (gap > 100u)
        {
          g_dbg_disp_gap_over_100++;
        }
      else if (gap > 33u)
        {
          g_dbg_disp_gap_over_33++;
        }
    }

  g_dbg_disp_last_commit_ms = now;

  /* 切片：每 4 帧取一次哈希（读 1.2MB 帧缓冲，不能每帧都算） */

  if ((g_disp_commits & 3u) == 0)
    {
      uint32_t idx = g_dbg_disp_slice_idx % DM_DISP_SLICE_N;

      g_dbg_disp_slice_hash[idx] = dm_display_frame_hash();
      g_dbg_disp_slice_ms[idx]   = now;
      g_dbg_disp_slice_idx++;
      g_dbg_disp_slice_frames++;
    }

  g_disp_last_ms = now;
}

uint32_t dm_display_commits(void)
{
  return g_disp_commits;
}

uint32_t dm_display_last_ms(void)
{
  return g_disp_last_ms;
}

uint32_t dm_display_panel_beat(void)
{
#if defined(CONFIG_ARCH_CHIP_ESP32P4) && defined(CONFIG_ESPRESSIF_MIPI_DSI)
  extern uint32_t esp_mipi_dsi_dma_isr_count(void);

  return esp_mipi_dsi_dma_isr_count();
#else
  return 0;
#endif
}

uint32_t dm_display_panel_underrun(void)
{
#if defined(CONFIG_ARCH_CHIP_ESP32P4) && defined(CONFIG_ESPRESSIF_MIPI_DSI)
  extern uint32_t esp_mipi_dsi_bridge_underrun_count(void);

  /* 这个函数**会清中断标志**，所以只能由看门狗以固定节奏调用一次。
   *
   * 它读的是 MIPI-DSI 桥的「DPI 要像素、而 FIFO 里没有」标志 —— 也即是
   * "面板真的被饿着了"。和 panel_beat（DSI DMA 中断还在不在涨）是两件事：
   * beat 只能说明通道活着，underrun 才能说明画面有没有真的断供。
   */

  return esp_mipi_dsi_bridge_underrun_count();
#else
  return 0;
#endif
}

static bool g_created;

#if DM_DISPLAY_FBDEV
static int g_fb_fd = -1;
static struct fb_planeinfo_s g_plane;
#endif

#if DM_DISPLAY_MEM
static uint8_t *g_mem_pixels;
static size_t g_mem_size;

/* With a full-screen render buffer the draw buffer *is* the frame: the flush
 * callback only has to acknowledge it.
 */

static void dm_flush_cb(lv_display_t *disp, const lv_area_t *area,
                        uint8_t *px_map)
{
  (void)area;
  (void)px_map;
  lv_display_flush_ready(disp);
}
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void *dm_display_watchdog(void *arg);

/* Create the parent directory of a snapshot path, best effort. */

static void dm_makedirs_for(const char *path)
{
  char tmp[128];
  char *slash;
  size_t len;

  len = strlen(path);
  if (len == 0 || len >= sizeof(tmp))
    {
      return;
    }

  strlcpy(tmp, path, sizeof(tmp));
  slash = strrchr(tmp, '/');
  if (slash == NULL || slash == tmp)
    {
      return;
    }

  *slash = '\0';

  for (char *p = tmp + 1; *p != '\0'; p++)
    {
      if (*p == '/')
        {
          *p = '\0';
          (void)mkdir(tmp, 0777);
          *p = '/';
        }
    }

  (void)mkdir(tmp, 0777);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int dm_display_create(void)
{
  lv_nuttx_dsc_t info;
  lv_nuttx_result_t result;

  if (g_created)
    {
      return 0;
    }

  memset(&g_metrics, 0, sizeof(g_metrics));
  memset(&result, 0, sizeof(result));

  lv_nuttx_dsc_init(&info);

#if DM_DISPLAY_FBDEV
  {
    struct fb_videoinfo_s vinfo;

    g_fb_fd = open(DM_FB_PATH, O_RDWR | O_CLOEXEC);
    if (g_fb_fd < 0)
      {
        DM_ERR("cannot open framebuffer %s: %d", DM_FB_PATH, errno);
        return -errno;
      }

    memset(&vinfo, 0, sizeof(vinfo));
    memset(&g_plane, 0, sizeof(g_plane));

    if (ioctl(g_fb_fd, FBIOGET_VIDEOINFO, (unsigned long)&vinfo) < 0 ||
        ioctl(g_fb_fd, FBIOGET_PLANEINFO, (unsigned long)&g_plane) < 0)
      {
        DM_ERR("FBIOGET_* failed on %s: %d", DM_FB_PATH, errno);
        close(g_fb_fd);
        g_fb_fd = -1;
        return -errno;
      }

    g_metrics.width       = vinfo.xres;
    g_metrics.height      = vinfo.yres;
    g_metrics.bpp         = g_plane.bpp;
    g_metrics.stride      = g_plane.stride;
    g_metrics.memory      = false;
    g_metrics.pixels      = (uint8_t *)g_plane.fbmem;
    g_metrics.pixels_size = (size_t)g_plane.stride * g_metrics.height;
    g_metrics.backend     = "fbdev";

    /* 双缓冲探测结果必须打出来。
     *
     * LVGL 的 nuttx fbdev 后端只在 `yres_virtual == yres * 2` 时才走双缓冲，
     * 而这个条件取决于**板级驱动**有没有真的暴露第二页。这条日志是"双缓冲到底
     * 生效了没有"的唯一直接证据 —— 不看它就只能靠肉眼看撕裂，而撕裂是间歇的。
     */

    DM_LOG("display: yres=%d yres_virtual=%d => fbdev %s buffering",
           vinfo.yres, g_plane.yres_virtual,
           (g_plane.yres_virtual == vinfo.yres * 2) ? "double" : "single");

    info.fb_path    = DM_FB_PATH;

    /* **不要把设备路径交给 LVGL**：lv_nuttx 会为它建一个"设备 indev"，
     * 而那个 read(/dev/input0) 是在 lv_timer_handler（UI 线程）里执行的。
     * 真机实测：I2C0 与相机 SCCB/ES8311 共用，单次读最坏 230~800ms ⇒ UI 一起卡。
     * 改由 dm_touch.c 的**独立线程**轮询，UI 只读"最新样本"（零阻塞）。
     */

    info.input_path = NULL;
#if defined(CONFIG_UINPUT_TOUCH)
    info.utouch_path = NULL;
#endif

    lv_nuttx_init(&info, &result);

    if (result.disp != NULL)
      {
        lv_display_add_event_cb(result.disp, dm_display_render_ready_cb,
                                LV_EVENT_RENDER_READY, NULL);
      }

    if (result.disp == NULL)
      {
        DM_ERR("LVGL could not attach to %s", DM_FB_PATH);
        close(g_fb_fd);
        g_fb_fd = -1;
        return -ENODEV;
      }

    g_disp = result.disp;

    /* 把 LVGL 的刷新节拍从默认 33ms 收到 16ms。
     *
     * 为什么必须显式设：面板提交只发生在 lv_timer_handler() 里，也就是**主循环
     * 那个线程**里（本工程没有 LVGL render thread），所以"LVGL 的刷新周期"直接
     * 就是帧率上限。默认 33ms 再叠加 usleep 的 10ms tick 量化
     * （CONFIG_USEC_PER_TICK=10000 ⇒ usleep(33ms) 实际睡 40ms），真机 JTAG 实测
     * 帧周期 ~67ms = 15fps。收到 16ms 后主循环每 ~20ms（2 tick）醒一次，
     * 上限从 15fps 提到 ~50fps。
     *
     * LVGL 9.1 没有 lv_display_set_refr_period()，只能拿刷新定时器改周期。
     */

    {
      lv_timer_t *refr = lv_display_get_refr_timer(g_disp);

      if (refr != NULL)
        {
          lv_timer_set_period(refr, 16);
          DM_LOG("display: LVGL refr period 33ms -> 16ms");
        }
    }

    DM_LOG("display: %s %dx%d bpp=%d stride=%d", g_metrics.backend,
           g_metrics.width, g_metrics.height, g_metrics.bpp,
           g_metrics.stride);

    /* 显示看门狗：相机连续流把 DSI 打停时能自救（见 dm_display_watchdog） */

    /* 递增代际：上一次运行的看门狗线程（若有）会在下一轮轮询里看到代际变了
     * 而退出；新线程捕获的是新代际，二者不会同时工作。
     */

    g_wd_gen++;

    {
      pthread_t wd;
      pthread_attr_t attr;
      struct sched_param param;

      /* 看门狗优先级高于主循环（100），低于相机采集（90 是数值更小的
       * 更高优先级 —— 这里全部用"数值越大优先级越高"的 NuttX 约定）。
       *
       * 单核（CONFIG_NCPUS=1）下这是必要的：看门狗每次只读两个计数器和
       * 一个寄存器，但"什么时候被调度到"决定了它发现停摆的延迟。默认
       * 优先级 + CONFIG_RR_INTERVAL=200 意味着它可能被相机线程压住 200ms。
       */

      /* 栈保持**默认值**（CONFIG_PTHREAD_STACK_DEFAULT = 16384）。
       *
       * 这条线程在停摆路径上会走 esp32p4_display_reinit() →
       * video_stop / bind_framebuffer / video_start，那是穿过 DSI 驱动和 HAL
       * 的深调用链；压小栈只会让溢出以"毫不相关的 LVGL load fault"的形式
       * 出现（本项目已经踩过两次）。
       */

      pthread_attr_init(&attr);
      memset(&param, 0, sizeof(param));
      param.sched_priority = DM_DISP_WD_PRIORITY;
      (void)pthread_attr_setschedparam(&attr, &param);

      if (pthread_create(&wd, &attr, dm_display_watchdog, NULL) == 0)
        {
          (void)pthread_setname_np(wd, "dm-disp-wd");
          DM_LOG("display: watchdog started (prio %d)", DM_DISP_WD_PRIORITY);
        }

      pthread_attr_destroy(&attr);
    }
  }
#else /* DM_DISPLAY_MEM */
  {
    int width  = DM_MEM_WIDTH;
    int height = DM_MEM_HEIGHT;
    size_t stride = (size_t)width * 2;      /* RGB565 */

    /* Initialise the LVGL NuttX entry for its tick and cache setup, but do
     * not let it look for a framebuffer or a touchscreen device: there is
     * neither in a headless run.
     */

    info.fb_path    = NULL;
    info.input_path = NULL;
#if defined(CONFIG_UINPUT_TOUCH)
    info.utouch_path = NULL;
#endif
#if defined(CONFIG_LV_USE_NUTTX_MOUSE)
    info.mouse_path = NULL;
#endif

    lv_nuttx_init(&info, &result);

    g_mem_size = stride * height;
    if (g_mem_size > DM_SNAP_MAX_BYTES)
      {
        DM_ERR("in-memory display too large: %u bytes", (unsigned)g_mem_size);
        return -E2BIG;
      }

    g_mem_pixels = malloc(g_mem_size);
    if (g_mem_pixels == NULL)
      {
        DM_ERR("no memory for a %ux%u RGB565 frame",
               (unsigned)width, (unsigned)height);
        return -ENOMEM;
      }

    memset(g_mem_pixels, 0, g_mem_size);

    g_disp = lv_display_create(width, height);
    if (g_disp == NULL)
      {
        DM_ERR("lv_display_create failed");
        free(g_mem_pixels);
        g_mem_pixels = NULL;
        return -ENOMEM;
      }

    lv_display_set_color_format(g_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(g_disp, dm_flush_cb);
    lv_display_set_buffers(g_disp, g_mem_pixels, NULL, (uint32_t)g_mem_size,
                           LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_add_event_cb(g_disp, dm_display_render_ready_cb,
                            LV_EVENT_RENDER_READY, NULL);

    g_metrics.width       = width;
    g_metrics.height      = height;
    g_metrics.bpp         = 16;
    g_metrics.stride      = (int)stride;
    g_metrics.memory      = true;
    g_metrics.pixels      = g_mem_pixels;
    g_metrics.pixels_size = g_mem_size;
    g_metrics.backend     = "mem";

    DM_LOG("display: %s %dx%d bpp=%d stride=%d (headless)",
           g_metrics.backend, g_metrics.width, g_metrics.height,
           g_metrics.bpp, g_metrics.stride);
  }
#endif

  g_created = true;
  return 0;
}

const struct dm_display_metrics_s *dm_display_metrics(void)
{
  return g_created ? &g_metrics : NULL;
}

void *dm_display_lv(void)
{
  return g_disp;
}

int dm_display_snapshot(const char *path)
{
  FILE *fp;
  size_t written;
  char meta[128];
  int meta_len;

  if (!g_created || path == NULL)
    {
      return -EINVAL;
    }

  if (g_metrics.pixels == NULL || g_metrics.pixels_size == 0)
    {
      return -ENODEV;
    }

  if (g_metrics.pixels_size > DM_SNAP_MAX_BYTES)
    {
      return -E2BIG;
    }

  dm_makedirs_for(path);

  fp = fopen(path, "wb");
  if (fp == NULL)
    {
      DM_ERR("cannot create snapshot %s: %d", path, errno);
      return -errno;
    }

  written = fwrite(g_metrics.pixels, 1, g_metrics.pixels_size, fp);
  fclose(fp);

  if (written != g_metrics.pixels_size)
    {
      DM_ERR("short snapshot write: %u/%u", (unsigned)written,
             (unsigned)g_metrics.pixels_size);
      return -EIO;
    }

  meta_len = snprintf(meta, sizeof(meta),
                      "%s.txt", path);
  if (meta_len > 0 && (size_t)meta_len < sizeof(meta))
    {
      fp = fopen(meta, "w");
      if (fp != NULL)
        {
          fprintf(fp, "width=%d\nheight=%d\nbpp=%d\nstride=%d\nbackend=%s\n",
                  g_metrics.width, g_metrics.height, g_metrics.bpp,
                  g_metrics.stride, g_metrics.backend);
          fclose(fp);
        }
    }

  DM_LOG("snapshot: %s (%u bytes, %dx%d %dbpp %s)", path,
         (unsigned)g_metrics.pixels_size, g_metrics.width, g_metrics.height,
         g_metrics.bpp, g_metrics.backend);

  return (int)g_metrics.pixels_size;
}

/* Perceived luminance of one pixel of the frame, 0..255. */

static int dm_pixel_luma(const uint8_t *row, int x, int bpp)
{
  int r;
  int g;
  int b;

  if (bpp == 16)
    {
      const uint16_t *p16 = (const uint16_t *)row;
      uint16_t c = p16[x];

      r = ((c >> 11) & 0x1f) << 3;
      g = ((c >> 5) & 0x3f) << 2;
      b = (c & 0x1f) << 3;
    }
  else if (bpp == 32)
    {
      /* NuttX RGB32 frames are stored BGRA on little endian targets. */

      const uint8_t *p8 = row + (size_t)x * 4;

      b = p8[0];
      g = p8[1];
      r = p8[2];
    }
  else
    {
      r = g = b = row[x];
    }

  return (r * 299 + g * 587 + b * 114) / 1000;
}

int dm_display_stats(struct dm_display_stats_s *stats)
{
  const uint8_t *pixels;
  int width;
  int height;
  int stride;
  int bpp;
  int step;
  int samples = 0;
  int grad_sum = 0;
  int grad_count = 0;
  int best_count = 0;
  int i;

  static uint16_t colours[256];
  static int counts[256];
  int distinct = 0;

  if (stats == NULL || !g_created || g_metrics.pixels == NULL)
    {
      return -EINVAL;
    }

  pixels = g_metrics.pixels;
  width  = g_metrics.width;
  height = g_metrics.height;
  stride = g_metrics.stride;
  bpp    = g_metrics.bpp;

  memset(stats, 0, sizeof(*stats));

  /* Sample every 7th pixel in both directions: cheap even on the board and
   * plenty to tell a drawn UI from a blank or noisy panel.  The gradient is
   * measured between *adjacent* pixels even though the samples are sparse, so
   * the numbers stay comparable with tools/test/fb2png.py.
   */

  step = 7;

  for (int y = 0; y < height; y += step)
    {
      const uint8_t *row = pixels + (size_t)y * stride;

      for (int x = 0; x + 1 < width; x += step)
        {
          uint16_t colour;

          if (bpp == 16)
            {
              colour = ((const uint16_t *)row)[x];
            }
          else if (bpp == 32)
            {
              colour = (uint16_t)(((const uint32_t *)row)[x] & 0xffff);
            }
          else
            {
              colour = row[x];
            }

          samples++;

          {
            int l0 = dm_pixel_luma(row, x, bpp);
            int l1 = dm_pixel_luma(row, x + 1, bpp);
            int d = l0 - l1;

            if (d < 0)
              {
                d = -d;
              }

            grad_sum += d;
            grad_count++;
          }

          /* Colour histogram, saturating at 256 buckets. */

          for (i = 0; i < distinct; i++)
            {
              if (colours[i] == colour)
                {
                  counts[i]++;
                  break;
                }
            }

          if (i == distinct && distinct < (int)(sizeof(colours) /
                                                sizeof(colours[0])))
            {
              colours[distinct] = colour;
              counts[distinct] = 1;
              distinct++;
            }
        }
    }

  for (i = 0; i < distinct; i++)
    {
      if (counts[i] > best_count)
        {
          best_count = counts[i];
        }
    }

  stats->distinct     = distinct;
  stats->dominant_pct = samples > 0 ? (best_count * 100) / samples : 0;
  stats->grad_mean    = grad_count > 0 ? grad_sum / grad_count : 0;
  stats->sampled      = samples;

  return 0;
}

uint32_t dm_display_frame_hash(void)
{
  const uint8_t *pixels;
  uint32_t hash = 2166136261u;
  int width;
  int height;
  int stride;
  int bpp;

  if (!g_created || g_metrics.pixels == NULL)
    {
      return 0;
    }

  pixels = g_metrics.pixels;
  width  = g_metrics.width;
  height = g_metrics.height;
  stride = g_metrics.stride;
  bpp    = g_metrics.bpp;

  for (int y = 0; y < height; y += 11)
    {
      const uint8_t *row = pixels + (size_t)y * stride;

      for (int x = 0; x < width; x += 11)
        {
          uint8_t v;

          if (bpp == 16)
            {
              v = row[(size_t)x * 2];
            }
          else if (bpp == 32)
            {
              v = row[(size_t)x * 4 + 1];
            }
          else
            {
              v = row[x];
            }

          hash ^= v;
          hash *= 16777619u;
        }
    }

  return hash;
}

void dm_display_destroy(void)
{
  if (!g_created)
    {
      return;
    }

  /* 让看门狗线程退出（见 g_wd_gen 的说明）：递增代际即可，线程在下一轮
   * 500ms 轮询里看到代际变了就走，不需要阻塞等待。
   */

  g_wd_gen++;

#if DM_DISPLAY_FBDEV
  if (g_fb_fd >= 0)
    {
      close(g_fb_fd);
      g_fb_fd = -1;
    }
#else
  if (g_mem_pixels != NULL)
    {
      free(g_mem_pixels);
      g_mem_pixels = NULL;
    }
#endif

  g_disp = NULL;
  g_created = false;
}

/****************************************************************************
 * Name: dm_display_watchdog
 *
 * Description:
 *   显示看门狗：定期看驱动的"提交计数"是否还在增长。相机连续流把 DSI 打停时
 *   （共用 DW-GDMA/PSRAM 带宽的已知坑）计数会冻住 —— 这时调用板级的显示重建
 *   入口自救，而不是只能复位整机。
 *
 *   判定：连续 STALL_TICKS × 500ms（= 6 秒）没有新提交 ⇒ 认为停摆。
 *   注意：页面完全空闲时 LVGL 本来就不会重绘，所以阈值不能太短（3 秒会误报）。
 *   自救：最多 REINIT_MAX 次，之后只告警（避免在真硬件故障上反复折腾）。
 *
 ****************************************************************************/

#define DM_DISP_WD_PERIOD_MS 500
/* 6 秒：太短会在"页面完全空闲、LVGL 没有脏区可重绘"时误判（实测 RAW8 自检里
 * 出现过 stalls=3 的误报）。真停摆（面板被抢死）是**永久**不恢复，6 秒足够区分。
 */

#define DM_DISP_WD_STALL_TICKS 12       /* 12 × 500ms = 6 秒 */
#define DM_DISP_WD_REINIT_MAX 3


/* 诊断（JTAG 可读） */
volatile uint32_t g_dbg_disp_stalls;
volatile uint32_t g_dbg_disp_reinits;

static void *dm_display_watchdog(void *arg)
{
  uint32_t last = dm_display_commits();
  uint32_t last_beat = dm_display_panel_beat();
  int stall = 0;
  unsigned int reinits = 0;
  uint32_t my_gen = g_wd_gen;    /* 只服务"我这一代" */

  (void)arg;

  while (g_wd_gen == my_gen)
    {
      usleep(DM_DISP_WD_PERIOD_MS * 1000);

      uint32_t now = dm_display_commits();
      uint32_t beat = dm_display_panel_beat();

      /* 每 500ms 采一次桥 underrun（读即清，所以节奏必须由这里固定）。 */

      (void)dm_display_panel_underrun();

      /* 面板保活优先：只要 DSI 的传输完成中断还在涨，面板就一定在被喂。
       *
       * 反过来，commits 涨但 beat 不涨 = "LVGL 在画、面板已死" —— 这正是相机
       * 抢死 DSI 之后的真实现象，旧的判据（只看 commits）永远发现不了。
       */

      if (beat != 0 && last_beat != 0 && beat != last_beat)
        {
          last_beat = beat;
          last = now;
          stall = 0;
          continue;
        }

      last_beat = beat;

      if (now != last)
        {
          last  = now;
          stall = 0;
          continue;
        }

      if (++stall < DM_DISP_WD_STALL_TICKS)
        {
          continue;
        }

      stall = 0;
      g_dbg_disp_stalls++;

#ifdef CONFIG_ARCH_CHIP_ESP32P4
      extern int esp32p4_display_reinit(void);
#endif

      DM_ERR("display: 停摆 6s（commits=%u panel_beat=%u），尝试重建",
             (unsigned)now, (unsigned)beat);

#ifdef CONFIG_ARCH_CHIP_ESP32P4
      if (reinits < DM_DISP_WD_REINIT_MAX)
        {
          reinits++;
          if (esp32p4_display_reinit() == 0)
            {
              g_dbg_disp_reinits++;
              DM_LOG("display: 重建成功（第 %u 次）", reinits);
            }
          else
            {
              DM_ERR("display: 重建失败（第 %u 次）", reinits);
            }
        }
#else
      (void)reinits;
#endif
    }

  return NULL;
}
