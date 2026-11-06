/****************************************************************************
 * apps/examples/deskmate/src/hal/dm_camera.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Camera half of the DeskMate HAL: see dm_camera.h.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <unistd.h>

#if defined(CONFIG_VIDEO) || defined(CONFIG_VIDEO_STREAM)
#  include <nuttx/video/video.h>
#  include <nuttx/video/v4l2_cap.h>
#  define DM_HAVE_V4L2 1
#endif

/* struct imgdata_s 只在驱动层用到；CSI 不通的配置里不必把它拉进来。 */

#if defined(CONFIG_ARCH_CHIP_ESP32P4) && defined(CONFIG_ESPRESSIF_MIPI_CSI)
#  include <nuttx/video/imgdata.h>
#endif

#include <time.h>

#include "dm_config.h"
#include "dm_log.h"
#include "hal/dm_camera.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* DM_CAMERA_PATH 现在由 dm_config.h 统一定义（NSH 的用法文本也要引用它）。
 * 这里不再重复定义，避免两处 fallback 悄悄分叉。
 */

#ifndef DM_CAMERA_PATH
#  define DM_CAMERA_PATH "/dev/video0"
#endif

#define DM_CAMERA_BUFFERS 3

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct dm_camera_buf_s
{
  void   *mem;
  size_t  len;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static int  g_captured;
static int   g_fd = -1;
static bool  g_ready;
static bool  g_synthetic = true;
static int   g_width;
static int   g_height;
static int   g_src_width;
static int   g_src_height;
static uint32_t g_src_format;
static int   g_seq;

static struct dm_camera_buf_s g_bufs[DM_CAMERA_BUFFERS];
static int g_nbufs;
static bool g_streaming;

static struct dm_camera_frame_s g_frame;
static uint16_t *g_pixels;      /* 采集线程正在写的缓冲 */
static uint16_t *g_pixels_pub;  /* 最近一次完成、交给 UI 画的缓冲 */
static uint16_t *g_prev;        /* 上一帧副本：运动检测在采集线程里做 */
static volatile int g_motion;   /* 最近一次运动占比 (%) */
static volatile uint32_t g_pub_seq;   /* 已发布的帧序号 */

/* 诊断（JTAG 可读）：采集线程到底在干什么。 */

volatile uint32_t g_dbg_cap_calls;
volatile uint32_t g_dbg_cap_eagain;
volatile uint32_t g_dbg_cap_pub;
static uint32_t g_ui_seq;             /* UI 已取走的帧序号 */
static int g_burst_frames;            /* 让路限流：本突发已采帧数 */

/* 诊断（JTAG 可读）：单帧"格式转换 + 缩放"耗时（微秒，累计 / 单次最大） */

volatile uint32_t g_dbg_cap_gap_max_ms;
volatile uint32_t g_dbg_cap_gap_over_100;
volatile uint32_t g_dbg_cap_last_ms;

volatile uint32_t g_dbg_conv_us;
volatile uint32_t g_dbg_conv_us_max;
volatile uint32_t g_dbg_decim_skip;     /* 因跳帧而没做转换的帧数 */
static uint32_t g_decim_count;
static volatile bool g_cap_run;
static pthread_t g_cap_thread;
static bool g_cap_started;
static bool g_disabled;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#ifdef DM_HAVE_V4L2
static int dm_camera_try_fmt(uint32_t pixfmt)
{
  struct v4l2_format fmt;

  memset(&fmt, 0, sizeof(fmt));
  fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width       = g_src_width;
  fmt.fmt.pix.height      = g_src_height;
  fmt.fmt.pix.pixelformat = pixfmt;
  fmt.fmt.pix.field       = V4L2_FIELD_ANY;

  if (ioctl(g_fd, VIDIOC_S_FMT, (unsigned long)&fmt) < 0)
    {
      return -errno;
    }

  g_src_width  = fmt.fmt.pix.width;
  g_src_height = fmt.fmt.pix.height;
  g_src_format = fmt.fmt.pix.pixelformat;

  return 0;
}
#endif

#ifdef DM_HAVE_V4L2
/* 2x2 quad demosaic of a BGGR RAW8 frame, resampled straight into the preview
 * buffer.  Averaging each quad gives one RGB pixel, so the preview is half the
 * sensor resolution; the scale factor then maps that onto the canvas.
 */

static void dm_camera_convert_bayer(const uint8_t *src, int sw, int sh,
                                    uint16_t *dst, int dw, int dh)
{
  /* 顺序推进指针的 BGGR -> RGB565 + 降采样。
   *
   * 旧实现给每个输出像素算一个源偏移（sxmap），在 1280 宽的帧上按 320 个散点读，
   * cache 极不友好；而且每帧只用到 921KB 里的一小部分却按行触发了大量 cache miss。
   *
   * 新实现：每个输出行只取源的两行，指针在行内**顺序前进**（步长 = qw/dw），
   * 每像素只读 3 个字节且都在同一 cache line 附近。真机实测见 docs §19.5。
   */

  int qw = sw / 2;              /* 2x2 quad 的列数 */
  int qh = sh / 2;              /* 2x2 quad 的行数 */
  int step;

  if (qw <= 0 || qh <= 0 || dw <= 0 || dh <= 0)
    {
      return;
    }

  step = qw / dw;
  if (step < 1)
    {
      step = 1;
    }

  /* 行映射改成增量累加（Bresenham）而不是每行一次除法。
   *
   * 原来是 `sy = (y * qh) / dh` —— 每输出行一次 32 位除法。RISC-V 的除法是
   * 软件库实现（几十个周期），dh=180 时虽然只有 180 次，但真正的收益在于
   * 让内层循环完全无分支：增量只在外层更新，编译器能把内层更好地展开。
   */

  int sy = 0;
  int yerr = 0;

  for (int y = 0; y < dh; y++)
    {
      const uint8_t *p0 = src + (size_t)(sy * 2) * sw;   /* 偶行：B G B G ... */
      const uint8_t *p1 = p0 + sw;                       /* 奇行：G R G R ... */
      uint16_t *out = dst + (size_t)y * dw;

      for (int x = 0; x < dw; x++)
        {
          /* BGGR：p0[0]=B、p0[1]=G、p1[1]=R */

          int b = p0[0];
          int g = p0[1];
          int r = p1[1];

          out[x] = (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) |
                              ((b & 0xf8) >> 3));

          p0 += step;
          p1 += step;
        }

      /* 推进输出行对应的源 quad 行 */

      yerr += qh;
      while (yerr >= dh)
        {
          yerr -= dh;
          sy++;
        }
    }
}

/* RGB565 (or RGB888) source -> RGB565 preview. */

static void dm_camera_convert_rgb(const uint8_t *src, int sw, int sh,
                                  uint16_t *dst, int dw, int dh)
{
  for (int y = 0; y < dh; y++)
    {
      int sy = (y * sh) / dh;
      uint16_t *out = dst + (size_t)y * dw;

      for (int x = 0; x < dw; x++)
        {
          int sx = (x * sw) / dw;

          if (g_src_format == V4L2_PIX_FMT_RGB565)
            {
              out[x] = ((const uint16_t *)src)[(size_t)sy * sw + sx];
            }
          else
            {
              const uint8_t *p = src + ((size_t)sy * sw + sx) * 3;
              out[x] = (uint16_t)(((p[0] & 0xf8) << 8) |
                                  ((p[1] & 0xfc) << 3) | ((p[2] & 0xf8) >> 3));
            }
        }
    }
}

#endif /* DM_HAVE_V4L2 */

/* Synthetic pattern: a colour ramp plus a moving block, so the preview is
 * obviously "live" and its motion can drive the proactive engine.
 */

static void dm_camera_synth(uint16_t *dst, int dw, int dh, int seq)
{
  int bx = (seq * 7) % (dw > 40 ? dw - 40 : 1);
  int by = (dh > 40) ? (dh - 40) / 2 + (seq % 20) : 0;

  for (int y = 0; y < dh; y++)
    {
      uint16_t *out = dst + (size_t)y * dw;

      for (int x = 0; x < dw; x++)
        {
          int r = (x * 255) / (dw > 0 ? dw : 1);
          int g = (y * 255) / (dh > 0 ? dh : 1);
          int b = ((x + y + seq) * 3) & 0xff;

          if (x >= bx && x < bx + 40 && y >= by && y < by + 40)
            {
              r = 240;
              g = 200;
              b = 40;
            }

          out[x] = (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) |
                              ((b & 0xf8) >> 3));
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void dm_camera_set_disabled(bool disabled)
{
  g_disabled = disabled;
}

static int  dm_camera_capture_once(void);
static void dm_camera_capture_start(void);

int dm_camera_create(int preview_w, int preview_h)
{
  if (g_disabled)
    {
      DM_LOG("camera: disabled by request (--no-camera)");
      return -ENODEV;
    }

  if (g_ready)
    {
      return 0;
    }

  if (preview_w <= 0 || preview_h <= 0)
    {
      return -EINVAL;
    }

  g_width  = preview_w;
  g_height = preview_h;

  /* 128B 对齐：PPA（2D 硬件）读写外部 PSRAM 缓冲要求 L1+L2 cache line 对齐 */

  g_pixels     = memalign(128, (size_t)preview_w * preview_h * sizeof(uint16_t));
  g_pixels_pub = memalign(128, (size_t)preview_w * preview_h * sizeof(uint16_t));
  g_prev       = memalign(128, (size_t)preview_w * preview_h * sizeof(uint16_t));
  if (g_pixels == NULL || g_pixels_pub == NULL || g_prev == NULL)
    {
      DM_ERR("camera: no memory for a %dx%d preview", preview_w, preview_h);
      return -ENOMEM;
    }

  memset(g_pixels, 0, (size_t)preview_w * preview_h * sizeof(uint16_t));
  memset(g_pixels_pub, 0, (size_t)preview_w * preview_h * sizeof(uint16_t));
  memset(g_prev, 0, (size_t)preview_w * preview_h * sizeof(uint16_t));

  /* 先把 frame 描述好：真机首帧没就绪时 poll() 会返回 -EAGAIN，页面仍然要知道
   * 预览缓冲在哪（否则 lv_canvas_set_buffer(NULL) 会直接断言死机）。
   */

  memset(&g_frame, 0, sizeof(g_frame));
  g_frame.pixels = g_pixels_pub;
  g_frame.width  = preview_w;
  g_frame.height = preview_h;

#ifdef DM_HAVE_V4L2
  g_fd = open(DM_CAMERA_PATH, O_RDWR | O_NONBLOCK);
  if (g_fd >= 0)
    {
      /* 采集分辨率保持 1280x720。
       *
       * 试过优先 640x480（想降低 PSRAM 带宽竞争），实测反而更差：预览掉到 5fps
       * 且出现 1.5 秒停顿（SC2336 在这个尺寸下走的是另一条出图模式）。已回退。
       */

      g_src_width  = 1280;
      g_src_height = 720;

      if (dm_camera_try_fmt(V4L2_PIX_FMT_SBGGR8) == 0 ||
          dm_camera_try_fmt(V4L2_PIX_FMT_RGB565) == 0 ||
          dm_camera_try_fmt(V4L2_PIX_FMT_RGB24) == 0)
        {
          struct v4l2_requestbuffers req;
          int i;

          memset(&req, 0, sizeof(req));
          req.count  = DM_CAMERA_BUFFERS;
          req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
          req.memory = V4L2_MEMORY_USERPTR;

          if (ioctl(g_fd, VIDIOC_REQBUFS, (unsigned long)&req) == 0)
            {
              size_t bytes = (size_t)g_src_width * g_src_height *
                             (g_src_format == V4L2_PIX_FMT_RGB565 ? 2 :
                              g_src_format == V4L2_PIX_FMT_RGB24 ? 3 : 1);

#ifdef DM_CSI_ISP_RGB565
              /* 驱动走 ISP 硬件去马赛克，**固定输出 RGB565**（每像素 2 字节），
               * 而 V4L2 协商可能仍回报 RAW8（驱动的格式表里没有 RGB565）。
               * 这里按驱动的实际输出分配缓冲，避免 set_buf 的 "buffer too small"。
               */

              bytes = (size_t)g_src_width * g_src_height * 2;
              g_src_format = V4L2_PIX_FMT_RGB565;
#endif

              for (i = 0; i < DM_CAMERA_BUFFERS && i < (int)req.count; i++)
                {
                  struct v4l2_buffer buf;

                  /* 必须按缓存行对齐（64B，这里给 128B 余量）：CSI HAL 在 start 时会对这个
       * 缓冲区做 esp_cache_msync(M2C)，而它要求地址按 cache line 对齐 ——
       * 用 malloc 的 8/16B 对齐会被判非法，直接返回 ESP_ERR_INVALID_ARG(258)，
       * 现象就是 esp_cam_ctlr_start failed: 258（实测）。
       */

      g_bufs[i].mem = memalign(128, bytes);
                  g_bufs[i].len = bytes;

                  if (g_bufs[i].mem == NULL)
                    {
                      break;
                    }

                  memset(&buf, 0, sizeof(buf));
                  buf.type      = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                  buf.memory    = V4L2_MEMORY_USERPTR;
                  buf.index     = i;
                  buf.m.userptr = (unsigned long)g_bufs[i].mem;
                  buf.length    = bytes;

                  if (ioctl(g_fd, VIDIOC_QBUF, (unsigned long)&buf) < 0)
                    {
                      DM_WARN("camera: QBUF %d failed: %d", i, errno);
                      free(g_bufs[i].mem);
                      g_bufs[i].mem = NULL;
                      break;
                    }
                }

              g_nbufs = i;

              if (g_nbufs > 0)
                {
                  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

                  if (ioctl(g_fd, VIDIOC_STREAMON, (unsigned long)&type) == 0)
                    {
                      g_streaming  = true;
                      g_synthetic  = false;
                      g_ready      = true;
                      dm_camera_capture_start();

                      DM_LOG("camera: v4l2 %dx%d fmt=0x%08x -> preview %dx%d",
                             g_src_width, g_src_height,
                             (unsigned)g_src_format, g_width, g_height);
                      return 0;
                    }

                  DM_ERR("camera: STREAMON failed: %d", errno);
                }
            }
          else
            {
              DM_WARN("camera: REQBUFS failed: %d", errno);
            }
        }
      else
        {
          DM_WARN("camera: no supported pixel format on %s", DM_CAMERA_PATH);
        }

      close(g_fd);
      g_fd = -1;
    }
  else
    {
      DM_LOG("camera: %s not available (%d), using the synthetic source",
             DM_CAMERA_PATH, errno);
    }
#endif

  g_synthetic = true;
  g_ready = true;
  dm_camera_capture_start();

  DM_LOG("camera: synthetic %dx%d source", g_width, g_height);
  return 0;
}

const char *dm_camera_backend(void)
{
  return g_synthetic ? "synthetic" : "v4l2";
}

bool dm_camera_ready(void)
{
  return g_ready;
}

int dm_camera_motion(void)
{
  return g_motion;
}

uint32_t dm_camera_frames_now(void)
{
  return g_dbg_cap_pub;
}

/* 采集一次：只允许采集线程调用。转换到写缓冲，成功后与发布缓冲对调。
 * 这样 UI 线程拿到的永远是"已完成"的那块，不会看到半张图。
 */

static int dm_camera_capture_once(void)
{
  g_dbg_cap_calls++;

  if (!g_ready)
    {
      return -ENODEV;
    }

  if (g_synthetic)
    {
      dm_camera_synth(g_pixels, g_width, g_height, g_seq++);
    }
  else
    {
#ifdef DM_HAVE_V4L2
      struct v4l2_buffer buf;
      struct pollfd pfd;
      int ret;

      pfd.fd     = g_fd;
      pfd.events = POLLIN;

      ret = poll(&pfd, 1, 0);
      if (ret <= 0)
        {
          g_dbg_cap_eagain++;
          return -EAGAIN;
        }

      memset(&buf, 0, sizeof(buf));
      buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_USERPTR;

      if (ioctl(g_fd, VIDIOC_DQBUF, (unsigned long)&buf) < 0)
        {
          return -EAGAIN;
        }

      if (buf.index < (unsigned)DM_CAMERA_BUFFERS && g_bufs[buf.index].mem)
        {
          const uint8_t *src = g_bufs[buf.index].mem;

          /* 跳帧：每 DM_CAMERA_DECIMATE 帧才真正转换一次。
           *
           * 依据是本文件上面的 JTAG 计数器（conv 峰值 220 ms/帧），而 UI 侧
           * 的预览节流是 66 ms —— 也就是说大部分转换完就被丢掉了。
           * 注意仍然 **每帧都 DQBUF+QBUF**，只是跳过转换：不交还缓冲会让
           * V4L2 的队列变空，反过来把驱动侧的 dropped 计数打满。
           */

          if (DM_CAMERA_DECIMATE > 1 &&
              (++g_decim_count % DM_CAMERA_DECIMATE) != 0)
            {
              g_dbg_decim_skip++;

              if (ioctl(g_fd, VIDIOC_QBUF,
                        (unsigned long)&buf) < 0)
                {
                  return -EAGAIN;
                }

              return 0;
            }

          {
            uint32_t dbg_t0 = (uint32_t)TICK2USEC(clock_systime_ticks());

            if (g_src_format == V4L2_PIX_FMT_SBGGR8)
              {
                dm_camera_convert_bayer(src, g_src_width, g_src_height,
                                        g_pixels, g_width, g_height);
              }
            else
              {
                /* 缩放仍走 CPU：实测 PPA（esp_ppa_scale_rgb565）功能正常但
                 * **比 CPU 慢 4 倍**（52.6ms vs 12.6ms/帧），主因是每帧对
                 * 1.84MB 输入做 cache msync。helper 保留备用（docs §19.9）。
                 */

                dm_camera_convert_rgb(src, g_src_width, g_src_height,
                                      g_pixels, g_width, g_height);
              }

            {
              uint32_t dbg_dt = (uint32_t)TICK2USEC(clock_systime_ticks()) -
                                dbg_t0;

              g_dbg_conv_us += dbg_dt;
              if (dbg_dt > g_dbg_conv_us_max)
                {
                  g_dbg_conv_us_max = dbg_dt;
                }
            }
          }
        }

      (void)ioctl(g_fd, VIDIOC_QBUF, (unsigned long)&buf);

      /* 让路：每采 DM_CAMERA_BURST_FRAMES 帧歇 DM_CAMERA_BURST_PAUSE_MS，
       * 把 PSRAM/DW-GDMA 带宽让回给显示扫描（ISP 路径必须，见 §20.1）。
       */

      if (DM_CAMERA_BURST_FRAMES > 0 &&
          ++g_burst_frames >= DM_CAMERA_BURST_FRAMES)
        {
          g_burst_frames = 0;
          usleep(DM_CAMERA_BURST_PAUSE_MS * 1000);
        }

      /* **单帧/少帧模式**：抓够几帧就把流停掉。
       *
       * 真机实测：相机与面板共用同一个 DW-GDMA 控制器，连续流会把 DSI 的显示
       * 打停（面板蓝屏、要复位才恢复，证据见文档 §16.6/§16.7）。改成"抓几帧
       * 就停"，让两条通道不长时间并发。
       */

      if (DM_CAMERA_SHOT_FRAMES > 0 &&
          ++g_captured >= DM_CAMERA_SHOT_FRAMES)
        {
          enum v4l2_buf_type stop = V4L2_BUF_TYPE_VIDEO_CAPTURE;

          (void)ioctl(g_fd, VIDIOC_STREAMOFF, (unsigned long)&stop);
          g_streaming = false;
          DM_LOG("camera: single-shot stop after %d frames", g_captured);
        }
#endif
    }

  /* 发布：换缓冲 + 递增序号。UI 看到序号变化时，那块缓冲已经写完了。 */

  {
    uint16_t *tmp = g_pixels_pub;

    g_pixels_pub = g_pixels;
    g_pixels     = tmp;
  }

  g_seq++;

  g_frame.pixels    = g_pixels_pub;
  g_frame.width     = g_width;
  g_frame.height    = g_height;
  g_frame.seq       = g_seq;
  g_frame.synthetic = g_synthetic;

  /* 运动检测在这里做（采集线程），**不要**留给 UI 线程 —— 它是 O(像素) 的：
   * 原来放在 dm_ui_tick() 里，每帧要扫 8k+ 个采样点再 memcpy 115KB。
   */

  {
    const size_t samples = (size_t)g_width * (size_t)g_height;
    const size_t step = 7;
    int changed = 0;

    if (samples > 0 && g_prev != NULL)
      {
        for (size_t i = 0; i + step < samples; i += step)
          {
            int d = (int)g_prev[i] - (int)g_pixels_pub[i];

            if (d < 0)
              {
                d = -d;
              }

            if (d > DM_CAMERA_MOTION_DIFF)
              {
                changed++;
              }
          }

        memcpy(g_prev, g_pixels_pub, samples * sizeof(uint16_t));

        g_motion = (int)(((size_t)changed * 100) / (samples / step));
      }
  }

  g_pub_seq = g_seq;
  g_dbg_cap_pub++;

  /* 失帧度量（相机侧）：相邻发布帧的间隔 */

  {
    uint32_t now = (uint32_t)TICK2MSEC(clock_systime_ticks());

    if (g_dbg_cap_last_ms != 0)
      {
        uint32_t gap = now - g_dbg_cap_last_ms;

        if (gap > g_dbg_cap_gap_max_ms)
          {
            g_dbg_cap_gap_max_ms = gap;
          }

        if (gap > 100u)
          {
            g_dbg_cap_gap_over_100++;
          }
      }

    g_dbg_cap_last_ms = now;
  }
  return 0;
}

/* UI 侧取帧：只在有新帧时返回描述符（无转换、无阻塞）。 */

int dm_camera_poll(struct dm_camera_frame_s *out)
{
  if (!g_ready)
    {
      return -ENODEV;
    }

  if (g_pub_seq == g_ui_seq)
    {
      return -EAGAIN;
    }

  g_ui_seq = g_pub_seq;

  if (out != NULL)
    {
      *out = g_frame;
    }

  return 0;
}

static FAR void *dm_camera_capture_thread(FAR void *arg)
{
  (void)arg;

  while (g_cap_run)
    {
      if (dm_camera_capture_once() != 0)
        {
          usleep(2000);
        }
      else if (g_synthetic)
        {
          /* 合成源没有"等下一帧"这一步：capture_once() 每次都成功，于是这个
           * 循环会**满速空转**（实测宿主侧 nuttx 进程 100% CPU），把同进程的
           * UI 线程挤到拿不到 CPU —— 表现就是 `--selftest --camera` 卡在
           * touch:target 之后再也不往前走（模拟器上必现，真机 v4l2 路径因为
           * poll() 等帧不会触发）。
           *
           * 这里按 30fps 的节奏主动让出 CPU。相机页本来就有 66ms 的预览节流，
           * 所以 30fps 的合成帧率对界面完全够用。
           */

          usleep(33 * 1000);
        }
    }

  return NULL;
}

static void dm_camera_capture_start(void)
{
  if (g_cap_started)
    {
      return;
    }

  g_cap_run     = true;
  g_cap_started = true;

  {
    pthread_attr_t attr;
    struct sched_param param;
    int ret;

    /* 采集线程优先级**低于**主循环（100）。
     *
     * 原来这里是 pthread_create(..., NULL, ...)，即用默认优先级 100 —— 与
     * UI 主循环完全相同。单核 + CONFIG_RR_INTERVAL=200 下，一个正在跑软件
     * 去马赛克的采集线程可以连着占用 200ms 时间片，主循环就被拖到 700ms 的
     * 空档（JTAG 实测 g_ui_max_gap_ms = 700）。把采集放低一档，让"没画面"
     * 优先于"画面不新鲜"，符合这个产品的交互需要。
     */

    /* 栈用**默认值**（CONFIG_PTHREAD_STACK_DEFAULT = 16384），不要自己压小。
     *
     * 曾设为 4096，理由是"采集线程只是 poll + 一次转换"。真机结果是在相机
     * 起来之后几十毫秒内触发 Load access fault（EPC 落在 lv_obj_get_screen
     * 里，MTVAL 是 0x7d 这种明显不是指针的值）—— 那是典型的栈溢出冲掉相邻
     * 堆对象，报出来的位置和真正的凶手完全无关。
     *
     * 这条线程会走 V4L2 ioctl、Bayer 去马赛克（内含多层内联）、以及
     * dm_camera_convert_rgb() 的逐行处理，实际栈需求远大于 4KB。
     */

    pthread_attr_init(&attr);
    memset(&param, 0, sizeof(param));
    param.sched_priority = DM_CAMERA_THREAD_PRIORITY;
    (void)pthread_attr_setschedparam(&attr, &param);

    ret = pthread_create(&g_cap_thread, &attr, dm_camera_capture_thread, NULL);
    pthread_attr_destroy(&attr);

    if (ret != 0)
      {
        DM_ERR("camera: capture thread create failed: %d", errno);
        g_cap_run     = false;
        g_cap_started = false;
      }
    else
      {
        (void)pthread_setname_np(g_cap_thread, "dm-camera");
        DM_LOG("camera: capture thread started (prio %d, UI 不再做采集/转换)",
               DM_CAMERA_THREAD_PRIORITY);
      }
  }
}

const struct dm_camera_frame_s *dm_camera_last(void)
{
  return g_ready ? &g_frame : NULL;
}

void dm_camera_destroy(void)
{
  /* 先停采集线程：它可能在 DQBUF/转换中，必须等它退出再动 fd 和缓冲。 */

  if (g_cap_started)
    {
      g_cap_run = false;
      pthread_join(g_cap_thread, NULL);
      g_cap_started = false;
    }

#ifdef DM_HAVE_V4L2
  if (g_fd >= 0 && g_streaming)
    {
      enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

      (void)ioctl(g_fd, VIDIOC_STREAMOFF, (unsigned long)&type);
    }

  for (int i = 0; i < DM_CAMERA_BUFFERS; i++)
    {
      if (g_bufs[i].mem != NULL)
        {
          free(g_bufs[i].mem);
          g_bufs[i].mem = NULL;
        }
    }

  if (g_fd >= 0)
    {
      close(g_fd);
      g_fd = -1;
    }
#endif

  if (g_pixels != NULL)
    {
      free(g_pixels);
      g_pixels = NULL;
    }

  if (g_pixels_pub != NULL)
    {
      free(g_pixels_pub);
      g_pixels_pub = NULL;
    }

  if (g_prev != NULL)
    {
      free(g_prev);
      g_prev = NULL;
    }

  g_ready = false;
  g_streaming = false;
}

/****************************************************************************
 * Name: dm_camera_driver_frames
 *
 * Description:
 *   读 CSI 驱动自己的帧计数（delivered / dropped / received）。
 *
 *   为什么应用需要它：应用侧的 frames 计数只统计"拿到的帧"，无法区分
 *   "相机没出帧"和"出了但被丢了"—— 这两者的修法完全相反（前者查 CSI/PHY，
 *   后者查缓冲排队深度）。驱动里的三个计数正好把它们分开：
 *     delivered 交给上层（V4L2 回调）的帧
 *     dropped   因为没有空闲缓冲而被丢弃的帧
 *     received  HAL 报告完成的帧（≈ 实际收到的帧）
 *
 *   返回 0 表示拿到数据；负值表示这条路径上没有 CSI 驱动（模拟器）。
 *
 ****************************************************************************/

int dm_camera_driver_frames(unsigned int *delivered, unsigned int *dropped,
                            unsigned int *received)
{
#if defined(CONFIG_ARCH_CHIP_ESP32P4) && defined(CONFIG_ESPRESSIF_MIPI_CSI)
  struct imgdata_s *data;
  uint32_t d = 0;
  uint32_t p = 0;
  uint32_t r = 0;

  extern struct imgdata_s *esp_csi_imgdata_get(void);
  extern void esp_csi_frames(struct imgdata_s *data, uint32_t *delivered,
                             uint32_t *dropped, uint32_t *received);

  data = esp_csi_imgdata_get();
  if (data == NULL)
    {
      return -1;
    }

  esp_csi_frames(data, &d, &p, &r);

  if (delivered != NULL)
    {
      *delivered = (unsigned int)d;
    }

  if (dropped != NULL)
    {
      *dropped = (unsigned int)p;
    }

  if (received != NULL)
    {
      *received = (unsigned int)r;
    }

  return 0;
#else
  (void)delivered;
  (void)dropped;
  (void)received;
  return -1;
#endif
}
