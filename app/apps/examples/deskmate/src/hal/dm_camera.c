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
#include <sys/ioctl.h>
#include <unistd.h>

#if defined(CONFIG_VIDEO) || defined(CONFIG_VIDEO_STREAM)
#  include <nuttx/video/video.h>
#  include <nuttx/video/v4l2_cap.h>
#  define DM_HAVE_V4L2 1
#endif

#include <time.h>

#include "dm_config.h"
#include "dm_log.h"
#include "hal/dm_camera.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifdef CONFIG_EXAMPLES_DESKMATE_CAMERA_DEVPATH
#  define DM_CAMERA_PATH CONFIG_EXAMPLES_DESKMATE_CAMERA_DEVPATH
#else
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
static uint16_t *g_pixels;
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
  int qw = sw / 2;
  int qh = sh / 2;

  if (qw <= 0 || qh <= 0)
    {
      return;
    }

  for (int y = 0; y < dh; y++)
    {
      int sy = (y * qh) / dh;
      const uint8_t *row0 = src + (size_t)(sy * 2) * sw;
      const uint8_t *row1 = row0 + sw;
      uint16_t *out = dst + (size_t)y * dw;

      for (int x = 0; x < dw; x++)
        {
          int sx = (x * qw) / dw;
          const uint8_t *q0 = row0 + sx * 2;
          const uint8_t *q1 = row1 + sx * 2;

          /* BGGR: (0,0)=B (0,1)=G / (1,0)=G (1,1)=R */

          int b = q0[0];
          int g = (q0[1] + q1[0]) / 2;
          int r = q1[1];

          out[x] = (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) |
                              ((b & 0xf8) >> 3));
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

  g_pixels = malloc((size_t)preview_w * preview_h * sizeof(uint16_t));
  if (g_pixels == NULL)
    {
      DM_ERR("camera: no memory for a %dx%d preview", preview_w, preview_h);
      return -ENOMEM;
    }

  memset(g_pixels, 0, (size_t)preview_w * preview_h * sizeof(uint16_t));

  /* 先把 frame 描述好：真机首帧没就绪时 poll() 会返回 -EAGAIN，页面仍然要知道
   * 预览缓冲在哪（否则 lv_canvas_set_buffer(NULL) 会直接断言死机）。
   */

  memset(&g_frame, 0, sizeof(g_frame));
  g_frame.pixels = g_pixels;
  g_frame.width  = preview_w;
  g_frame.height = preview_h;

#ifdef DM_HAVE_V4L2
  g_fd = open(DM_CAMERA_PATH, O_RDWR);
  if (g_fd >= 0)
    {
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

              for (i = 0; i < DM_CAMERA_BUFFERS && i < (int)req.count; i++)
                {
                  struct v4l2_buffer buf;

                  g_bufs[i].mem = malloc(bytes);
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

int dm_camera_poll(struct dm_camera_frame_s *out)
{
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
          g_frame.pixels = g_pixels;
          g_frame.width  = g_width;
          g_frame.height = g_height;
          g_frame.synthetic = false;
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

          if (g_src_format == V4L2_PIX_FMT_SBGGR8)
            {
              dm_camera_convert_bayer(src, g_src_width, g_src_height,
                                      g_pixels, g_width, g_height);
            }
          else
            {
              dm_camera_convert_rgb(src, g_src_width, g_src_height,
                                    g_pixels, g_width, g_height);
            }
        }

      (void)ioctl(g_fd, VIDIOC_QBUF, (unsigned long)&buf);
#endif
    }

  g_seq++;

  g_frame.pixels    = g_pixels;
  g_frame.width     = g_width;
  g_frame.height    = g_height;
  g_frame.seq       = g_seq;
  g_frame.synthetic = g_synthetic;

  if (out != NULL)
    {
      *out = g_frame;
    }

  return 0;
}

const struct dm_camera_frame_s *dm_camera_last(void)
{
  return g_ready ? &g_frame : NULL;
}

void dm_camera_destroy(void)
{
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

  g_ready = false;
  g_streaming = false;
}
