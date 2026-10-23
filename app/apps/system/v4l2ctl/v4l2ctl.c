/****************************************************************************
 * apps/system/v4l2ctl/v4l2ctl.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <nuttx/video/video.h>
#include <nuttx/video/v4l2_cap.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define V4L2CTL_DEFAULT_DEV   "/dev/video0"
#define V4L2CTL_MAX_DEVS      8
#define V4L2CTL_MAX_BUFFERS   3

/* The SC2336 runs at its native RAW10 mode here.  1920x1080x10/8 is
 * 2,592,000 bytes; the CSI driver rounds the DMA length up to the cache line,
 * so the user buffer must be at least that big and 64-byte aligned.
 */

#define V4L2CTL_ALIGN         64

/* Follow the receiver window the board was built for.  S_FMT must match the
 * CSI controller configuration set up by esp_csi_get_default_config();
 * passing a different size here does not reconfigure the controller.
 */

#ifdef CONFIG_ESPRESSIF_MIPI_CSI_H_RES
#  define V4L2CTL_DEFAULT_W   CONFIG_ESPRESSIF_MIPI_CSI_H_RES
#else
#  define V4L2CTL_DEFAULT_W   1920
#endif

#ifdef CONFIG_ESPRESSIF_MIPI_CSI_V_RES
#  define V4L2CTL_DEFAULT_H   CONFIG_ESPRESSIF_MIPI_CSI_V_RES
#else
#  define V4L2CTL_DEFAULT_H   1080
#endif

#define V4L2CTL_DEFAULT_FMT   V4L2_PIX_FMT_SBGGR8
#define V4L2CTL_DEFAULT_FRM   1
#define V4L2CTL_DEFAULT_WAIT  10

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct v4l2ctl_buf_s
{
  FAR uint8_t *start;
  size_t       length;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void v4l2ctl_usage(void)
{
  printf("Usage:\n");
  printf("  v4l2ctl [list-devices]\n");
  printf("  v4l2ctl [-d DEV] --info\n");
  printf("  v4l2ctl [-d DEV] --list-formats-ext\n");
  printf("  v4l2ctl [-d DEV] --capture [N] [--timeout SEC] [--save FILE]"
         " [--preview]\n");
  printf("\n");
  printf("  DEV default: %s, N default: %d, timeout default: %d s\n",
         V4L2CTL_DEFAULT_DEV, V4L2CTL_DEFAULT_FRM, V4L2CTL_DEFAULT_WAIT);
  printf("  NuttX does not ship v4l2-ctl; this command is the target-side\n");
  printf("  replacement for the enumeration and capture steps.\n");
}

/****************************************************************************
 * Name: v4l2ctl_fourcc
 *
 * Description:
 *   Print a V4L2 fourcc in its human readable form.
 *
 ****************************************************************************/

static void v4l2ctl_fourcc(uint32_t pixelformat)
{
  printf("%c%c%c%c",
         (int)( pixelformat        & 0xff),
         (int)((pixelformat >>  8) & 0xff),
         (int)((pixelformat >> 16) & 0xff),
         (int)((pixelformat >> 24) & 0xff));
}

/****************************************************************************
 * Name: v4l2ctl_list_devices
 *
 * Description:
 *   Enumerate /dev/video0..N without opening the driver, so that listing the
 *   devices does not disturb a running capture.
 *
 ****************************************************************************/

static int v4l2ctl_list_devices(void)
{
  char path[32];
  struct stat st;
  int found = 0;
  int i;

  for (i = 0; i < V4L2CTL_MAX_DEVS; i++)
    {
      snprintf(path, sizeof(path), "/dev/video%d", i);
      if (stat(path, &st) == 0)
        {
          printf("V4L2CTL: FOUND %s (mode=0%o)\n", path,
                 (unsigned int)st.st_mode);
          found++;
        }
    }

  if (found == 0)
    {
      printf("V4L2CTL: FOUND none (no /dev/video? node)\n");
      return ERROR;
    }

  return OK;
}

/****************************************************************************
 * Name: v4l2ctl_info
 *
 * Description:
 *   VIDIOC_QUERYCAP, plus the negotiated format through VIDIOC_G_FMT.
 *
 ****************************************************************************/

static int v4l2ctl_info(int fd)
{
  struct v4l2_capability cap;
  struct v4l2_format fmt;
  int ret;

  memset(&cap, 0, sizeof(cap));
  ret = ioctl(fd, VIDIOC_QUERYCAP, (uintptr_t)&cap);
  if (ret < 0)
    {
      printf("V4L2CTL: FAIL VIDIOC_QUERYCAP errno=%d\n", errno);
      return ERROR;
    }

  printf("V4L2CTL: DRIVER %s\n", (FAR char *)cap.driver);
  printf("V4L2CTL: CARD   %s\n", (FAR char *)cap.card);
  printf("V4L2CTL: BUS    %s\n", (FAR char *)cap.bus_info);
  printf("V4L2CTL: CAPS   0x%08lx\n", (unsigned long)cap.capabilities);

  memset(&fmt, 0, sizeof(fmt));
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  ret = ioctl(fd, VIDIOC_G_FMT, (uintptr_t)&fmt);
  if (ret == 0)
    {
      printf("V4L2CTL: G_FMT  %lux%lu fourcc=",
             (unsigned long)fmt.fmt.pix.width,
             (unsigned long)fmt.fmt.pix.height);
      v4l2ctl_fourcc(fmt.fmt.pix.pixelformat);
      printf(" sizeimage=%lu bytesperline=%lu\n",
             (unsigned long)fmt.fmt.pix.sizeimage,
             (unsigned long)fmt.fmt.pix.bytesperline);
    }

  return OK;
}

/****************************************************************************
 * Name: v4l2ctl_list_formats
 *
 * Description:
 *   ENUM_FMT -> ENUM_FRAMESIZES -> ENUM_FRAMEINTERVALS.  The driver must
 *   publish imgsensor_s.frmsizes/frmintervals for the last two to return
 *   anything; otherwise v4l2_cap.c falls back to a hard-coded QVGA / 15 fps.
 *
 ****************************************************************************/

static int v4l2ctl_list_formats(int fd)
{
  struct v4l2_fmtdesc fmtdesc;
  struct v4l2_frmsizeenum frmsize;
  unsigned int nfmt = 0;
  unsigned int i;
  unsigned int j;

  for (i = 0; i < 16; i++)
    {
      memset(&fmtdesc, 0, sizeof(fmtdesc));
      fmtdesc.index = i;
      fmtdesc.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE;

      if (ioctl(fd, VIDIOC_ENUM_FMT, (uintptr_t)&fmtdesc) < 0)
        {
          break;
        }

      printf("V4L2CTL: FORMAT [%u] '", i);
      v4l2ctl_fourcc(fmtdesc.pixelformat);
      printf("' %s\n", (FAR char *)fmtdesc.description);
      nfmt++;

      for (j = 0; j < 8; j++)
        {
          memset(&frmsize, 0, sizeof(frmsize));
          frmsize.index        = j;
          frmsize.pixel_format = fmtdesc.pixelformat;
          frmsize.buf_type     = V4L2_BUF_TYPE_VIDEO_CAPTURE;

          if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, (uintptr_t)&frmsize) < 0)
            {
              break;
            }

          if (frmsize.type == V4L2_FRMSIZE_TYPE_DISCRETE)
            {
              struct v4l2_frmivalenum frmival;
              unsigned int k;

              printf("V4L2CTL:   SIZE %ux%u\n",
                     (unsigned)frmsize.discrete.width,
                     (unsigned)frmsize.discrete.height);

              for (k = 0; k < 8; k++)
                {
                  memset(&frmival, 0, sizeof(frmival));
                  frmival.index        = k;
                  frmival.pixel_format = fmtdesc.pixelformat;
                  frmival.buf_type     = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                  frmival.width        = frmsize.discrete.width;
                  frmival.height       = frmsize.discrete.height;

                  if (ioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS,
                            (uintptr_t)&frmival) < 0)
                    {
                      break;
                    }

                  if (frmival.type == V4L2_FRMIVAL_TYPE_DISCRETE &&
                      frmival.discrete.numerator != 0)
                    {
                      printf("V4L2CTL:     INTERVAL %u/%u (%u fps)\n",
                             (unsigned)frmival.discrete.numerator,
                             (unsigned)frmival.discrete.denominator,
                             (unsigned)(frmival.discrete.denominator /
                                        frmival.discrete.numerator));
                    }
                }
            }
        }
    }

  if (nfmt == 0)
    {
      printf("V4L2CTL: FAIL no format enumerated errno=%d\n", errno);
      return ERROR;
    }

  return OK;
}

/****************************************************************************
 * Name: v4l2ctl_sum
 *
 * Description:
 *   Cheap content signature for a frame.  A capture path that never writes
 *   memory usually leaves an all-zero or constant buffer behind, and a path
 *   that returns the same stale frame twice produces identical sums; both are
 *   visible here without dumping 2.5 MB over the console.
 *
 ****************************************************************************/

static uint32_t v4l2ctl_sum(FAR const uint8_t *data, size_t len,
                            FAR uint32_t *nonzero, FAR uint8_t *minval,
                            FAR uint8_t *maxval)
{
  uint32_t sum = 2166136261u;
  uint32_t nz = 0;
  uint8_t mn = 0xff;
  uint8_t mx = 0x00;
  size_t i;

  for (i = 0; i < len; i++)
    {
      uint8_t v = data[i];

      sum = (sum ^ v) * 16777619u;
      if (v != 0)
        {
          nz++;
        }

      if (v < mn)
        {
          mn = v;
        }

      if (v > mx)
        {
          mx = v;
        }
    }

  *nonzero = nz;
  *minval  = mn;
  *maxval  = mx;
  return sum;
}

/****************************************************************************
 * Name: v4l2ctl_preview
 *
 * Description:
 *   Print a coarse ASCII luminance map of a RAW8 (BGGR) frame so that a
 *   capture can be shown to contain a real image straight from the console.
 *
 ****************************************************************************/

#define V4L2CTL_PREVIEW_COLS 64
#define V4L2CTL_PREVIEW_ROWS 24

static void v4l2ctl_preview(FAR const uint8_t *data, uint32_t width,
                            uint32_t height)
{
  static const char ramp[] = " .:-=+*#%@";
  uint32_t x;
  uint32_t y;

  printf("V4L2CTL: PREVIEW %" PRIu32 "x%" PRIu32 " (ASCII luminance)\n",
         width, height);

  for (y = 0; y < V4L2CTL_PREVIEW_ROWS; y++)
    {
      char line[V4L2CTL_PREVIEW_COLS + 1];
      uint32_t y0 = (uint32_t)((uint64_t)y * height / V4L2CTL_PREVIEW_ROWS);

      for (x = 0; x < V4L2CTL_PREVIEW_COLS; x++)
        {
          uint32_t x0 = (uint32_t)((uint64_t)x * width / V4L2CTL_PREVIEW_COLS);
          uint32_t v  = data[(size_t)y0 * width + x0];

          line[x] = ramp[(v * (sizeof(ramp) - 2)) / 255];
        }

      line[V4L2CTL_PREVIEW_COLS] = '\0';
      printf("V4L2CTL: |%s|\n", line);
    }
}

/****************************************************************************
 * Name: v4l2ctl_capture
 *
 * Description:
 *   One-shot capture: S_FMT -> REQBUFS(USERPTR, RING) -> QBUF xN ->
 *   STREAMON -> DQBUF xN -> STREAMOFF.  The device is opened O_NONBLOCK and
 *   DQBUF is polled with a deadline, so a sensor that never delivers a frame
 *   reports FAIL instead of blocking the shell until the board is reset.
 *
 ****************************************************************************/

static int v4l2ctl_capture(int fd, int frames, int wait_sec,
                           FAR const char *savepath,
                           bool preview)
{
  struct v4l2_format fmt;
  struct v4l2_requestbuffers req;
  struct v4l2_buffer buf;
  struct v4l2ctl_buf_s bufs[V4L2CTL_MAX_BUFFERS];
  size_t size;
  uint32_t prev_sum = 0;
  int got = 0;
  int nbuf;
  int ret;
  int i;

  memset(&fmt, 0, sizeof(fmt));
  fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width       = V4L2CTL_DEFAULT_W;
  fmt.fmt.pix.height      = V4L2CTL_DEFAULT_H;
  fmt.fmt.pix.pixelformat = V4L2CTL_DEFAULT_FMT;
  fmt.fmt.pix.field       = V4L2_FIELD_ANY;

  ret = ioctl(fd, VIDIOC_S_FMT, (uintptr_t)&fmt);
  if (ret < 0)
    {
      printf("V4L2CTL: FAIL VIDIOC_S_FMT errno=%d\n", errno);
      return ERROR;
    }

  size = (size_t)fmt.fmt.pix.width * fmt.fmt.pix.height;
  size = (size * (fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_SBGGR8 ? 8u : 10u)
          + 7u) / 8u;
  size = (size + (V4L2CTL_ALIGN - 1)) & ~(size_t)(V4L2CTL_ALIGN - 1);

  nbuf = frames + 1;
  if (nbuf < 2)
    {
      nbuf = 2;
    }

  if (nbuf > V4L2CTL_MAX_BUFFERS)
    {
      nbuf = V4L2CTL_MAX_BUFFERS;
    }

  printf("V4L2CTL: S_FMT %lux%lu fourcc=",
         (unsigned long)fmt.fmt.pix.width,
         (unsigned long)fmt.fmt.pix.height);
  v4l2ctl_fourcc(fmt.fmt.pix.pixelformat);
  printf(" buffer=%lu bytes x%d\n", (unsigned long)size, nbuf);

  memset(&req, 0, sizeof(req));
  req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_USERPTR;
  req.count  = nbuf;
  req.mode   = V4L2_BUF_MODE_RING;

  ret = ioctl(fd, VIDIOC_REQBUFS, (uintptr_t)&req);
  if (ret < 0)
    {
      printf("V4L2CTL: FAIL VIDIOC_REQBUFS errno=%d\n", errno);
      return ERROR;
    }

  memset(bufs, 0, sizeof(bufs));

  for (i = 0; i < nbuf; i++)
    {
      bufs[i].length = size;
      bufs[i].start  = memalign(V4L2CTL_ALIGN, size);
      if (bufs[i].start == NULL)
        {
          printf("V4L2CTL: FAIL out of memory for buffer %d\n", i);
          goto errout;
        }

      memset(&buf, 0, sizeof(buf));
      buf.type        = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory      = V4L2_MEMORY_USERPTR;
      buf.index       = i;
      buf.m.userptr   = (uintptr_t)bufs[i].start;
      buf.length      = size;

      ret = ioctl(fd, VIDIOC_QBUF, (uintptr_t)&buf);
      if (ret < 0)
        {
          printf("V4L2CTL: FAIL VIDIOC_QBUF[%d] errno=%d\n", i, errno);
          goto errout;
        }
    }

  {
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    ret = ioctl(fd, VIDIOC_STREAMON, (uintptr_t)&type);
    if (ret < 0)
      {
        printf("V4L2CTL: FAIL VIDIOC_STREAMON errno=%d\n", errno);
        goto errout;
      }
  }

  printf("V4L2CTL: STREAMON ok, waiting up to %d s for %d frame(s)\n",
         wait_sec, frames);

  /* Poll at 10 Hz: DQBUF is opened O_NONBLOCK and returns EAGAIN until a
   * frame is complete, so wait_sec * 10 iterations is a true wait_sec
   * deadline (the previous * 100 made a 20 s timeout take 200 s).
   */

  for (i = 0; i < wait_sec * 10 && got < frames; i++)
    {
      uint32_t sum;
      uint32_t nonzero;
      uint8_t minval;
      uint8_t maxval;

      memset(&buf, 0, sizeof(buf));
      buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buf.memory = V4L2_MEMORY_USERPTR;

      ret = ioctl(fd, VIDIOC_DQBUF, (uintptr_t)&buf);
      if (ret < 0)
        {
          if (errno == EAGAIN)
            {
              usleep(100 * 1000);
              continue;
            }

          printf("V4L2CTL: FAIL VIDIOC_DQBUF errno=%d\n", errno);
          break;
        }

      {
        size_t used = (size_t)buf.bytesused;

        /* RAW10 userspace buffer: the driver may report the whole buffer as
         * used, or leave bytesused at 0.  Fall back to the queued length so
         * the signature always covers the payload that was actually written.
         */

        if ((size_t)buf.index < (size_t)nbuf &&
            (used == 0 || used > bufs[buf.index].length))
          {
            used = bufs[buf.index].length;
          }

        sum = v4l2ctl_sum((FAR const uint8_t *)buf.m.userptr, used,
                          &nonzero, &minval, &maxval);

        printf("V4L2CTL: FRAME %d index=%lu bytesused=%lu used=%lu seq=%lu "
               "sum=0x%08lx nonzero=%lu min=%u max=%u delta=%ld\n",
               got, (unsigned long)buf.index,
               (unsigned long)buf.bytesused, (unsigned long)used,
               (unsigned long)buf.sequence,
               (unsigned long)sum,
               (unsigned long)nonzero, (unsigned)minval, (unsigned)maxval,
               (long)(sum - prev_sum));
      }

      if (preview && got == 0)
        {
          v4l2ctl_preview((FAR const uint8_t *)buf.m.userptr,
                          fmt.fmt.pix.width, fmt.fmt.pix.height);
        }

      if (savepath != NULL && got == 0)
        {
          FILE *fp = fopen(savepath, "wb");

          if (fp != NULL)
            {
              size_t n = fwrite((FAR const void *)buf.m.userptr, 1,
                                buf.bytesused, fp);
              fclose(fp);
              printf("V4L2CTL: SAVED %lu bytes to %s\n",
                     (unsigned long)n, savepath);
            }
          else
            {
              printf("V4L2CTL: WARN cannot open %s: %d\n",
                     savepath, errno);
            }
        }

      prev_sum = sum;
      got++;

      /* Return the buffer so the next frame has somewhere to land. */

      ret = ioctl(fd, VIDIOC_QBUF, (uintptr_t)&buf);
      if (ret < 0)
        {
          printf("V4L2CTL: WARN re-QBUF failed errno=%d\n", errno);
        }
    }

  {
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    (void)ioctl(fd, VIDIOC_STREAMOFF, (uintptr_t)&type);
  }

  for (i = 0; i < nbuf; i++)
    {
      if (bufs[i].start != NULL)
        {
          free(bufs[i].start);
          bufs[i].start = NULL;
        }
    }

  if (got < frames)
    {
      printf("V4L2CTL: RESULT FAIL frames=%d/%d\n", got, frames);
      return ERROR;
    }

  printf("V4L2CTL: RESULT PASS frames=%d/%d\n", got, frames);
  return OK;

errout:
  for (i = 0; i < nbuf; i++)
    {
      if (bufs[i].start != NULL)
        {
          free(bufs[i].start);
        }
    }

  printf("V4L2CTL: RESULT FAIL setup aborted\n");
  return ERROR;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  FAR const char *dev = V4L2CTL_DEFAULT_DEV;
  FAR const char *savepath = NULL;
  bool preview = false;
  int frames = V4L2CTL_DEFAULT_FRM;
  int wait_sec = V4L2CTL_DEFAULT_WAIT;
  int list_devs = 0;
  int do_info = 0;
  int do_formats = 0;
  int do_capture = 0;
  int fd;
  int ret = OK;
  int i;

  if (argc == 1)
    {
      list_devs = 1;
    }

  for (i = 1; i < argc; i++)
    {
      if (strcmp(argv[i], "list-devices") == 0)
        {
          list_devs = 1;
        }
      else if (strcmp(argv[i], "--info") == 0)
        {
          do_info = 1;
        }
      else if (strcmp(argv[i], "--list-formats-ext") == 0 ||
               strcmp(argv[i], "--list-formats") == 0)
        {
          do_formats = 1;
        }
      else if (strcmp(argv[i], "--capture") == 0)
        {
          do_capture = 1;

          if (i + 1 < argc && argv[i + 1][0] != '-')
            {
              frames = atoi(argv[++i]);
            }
        }
      else if (strcmp(argv[i], "--timeout") == 0)
        {
          if (i + 1 < argc)
            {
              wait_sec = atoi(argv[++i]);
            }
        }
      else if (strcmp(argv[i], "--preview") == 0)
        {
          preview = true;
        }
      else if (strcmp(argv[i], "--save") == 0)
        {
          if (i + 1 < argc)
            {
              savepath = argv[++i];
            }
        }
      else if (strcmp(argv[i], "-d") == 0)
        {
          if (i + 1 < argc)
            {
              dev = argv[++i];
            }
        }
      else if (strcmp(argv[i], "-h") == 0 ||
               strcmp(argv[i], "--help") == 0)
        {
          v4l2ctl_usage();
          return OK;
        }
      else
        {
          printf("V4L2CTL: unknown argument '%s'\n", argv[i]);
          v4l2ctl_usage();
          return ERROR;
        }
    }

  if (list_devs)
    {
      ret = v4l2ctl_list_devices();
      if (!do_info && !do_formats && !do_capture)
        {
          return ret;
        }
    }

  if (!do_info && !do_formats && !do_capture)
    {
      return ret;
    }

  /* O_NONBLOCK makes VIDIOC_DQBUF return EAGAIN instead of sleeping forever;
   * v4l2ctl_capture() turns that into a bounded wait.
   */

  fd = open(dev, O_RDWR | O_NONBLOCK);
  if (fd < 0)
    {
      printf("V4L2CTL: FAIL open %s errno=%d\n", dev, errno);
      return ERROR;
    }

  printf("V4L2CTL: OPEN %s\n", dev);

  if (do_info)
    {
      ret |= v4l2ctl_info(fd);
    }

  if (do_formats)
    {
      ret |= v4l2ctl_list_formats(fd);
    }

  if (do_capture)
    {
      ret |= v4l2ctl_capture(fd, frames, wait_sec, savepath, preview);
    }

  close(fd);
  return ret;
}
