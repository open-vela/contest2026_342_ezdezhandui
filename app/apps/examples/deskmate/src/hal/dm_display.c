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

    info.fb_path    = DM_FB_PATH;
    info.input_path = DM_INPUT_PATH;
#if defined(CONFIG_UINPUT_TOUCH)
    info.utouch_path = NULL;
#endif

    lv_nuttx_init(&info, &result);

    if (result.disp == NULL)
      {
        DM_ERR("LVGL could not attach to %s", DM_FB_PATH);
        close(g_fb_fd);
        g_fb_fd = -1;
        return -ENODEV;
      }

    g_disp = result.disp;

    DM_LOG("display: %s %dx%d bpp=%d stride=%d", g_metrics.backend,
           g_metrics.width, g_metrics.height, g_metrics.bpp,
           g_metrics.stride);
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
