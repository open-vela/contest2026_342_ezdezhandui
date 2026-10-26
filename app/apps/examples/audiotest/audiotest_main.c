/****************************************************************************
 * apps/examples/audiotest/audiotest_main.c
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
 * A self-contained audio output test
 *
 * The point of this program is to answer one question on real hardware --
 * "does the codec actually make a sound?" -- without depending on an SD card,
 * a network, or a WAV file that has to be transferred onto the board first.
 *
 * It synthesises a WAV stream in RAM and feeds it to the audio device, so a
 * successful run exercises the whole path at once:
 *
 *   this program -> /dev/audio/pcm0 -> pcm_decode -> codec driver
 *                -> I2S/DMA -> ES8311 -> NS4150B -> speaker
 *
 * ## Why this uses ioctls rather than write()
 *
 * An earlier version of this program simply write()'d the stream to the
 * device.  That cannot work, and it is worth recording so nobody retries it:
 * audio_write() in the audio upper half forwards to the lower half's write()
 * method, and *no* lower half in this tree implements one -- the codec
 * drivers, the I2S front end and the PCM decoder all leave it NULL.  The call
 * therefore returns 0, so a caller checking only for a negative return sees
 * success while a caller checking the byte count sees a short write.  That is
 * exactly how this program first failed on hardware: it reported an error with
 * errno 2, which was the count mismatch, not a real ENOENT.
 *
 * The path that does exist is the buffer/ioctl one: allocate with
 * AUDIOIOC_ALLOCBUFFER, fill, hand over with AUDIOIOC_ENQUEUEBUFFER, and let
 * the driver return the buffers through the registered callback.  That is what
 * nxplayer does, and it is what this program does.
 *
 * ## Why the format is not a free choice
 *
 * The output is 16-bit *stereo* on purpose.  The ES8311 is a mono codec and
 * takes the left slot, while the I2S port is configured with two slots (its
 * default), so a mono stream would play at half speed and an 8-bit one would
 * be mis-framed.  The sample rate is left to the caller, but only rates the
 * codec can derive from the I2S master clock are useful -- 16 kHz and 24 kHz
 * are the sensible choices for speech, and 48 kHz costs twice the APLL
 * frequency and twice the memory bandwidth for no benefit on a small speaker.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <nuttx/audio/audio.h>
#include <nuttx/audio/audio.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_EXAMPLES_AUDIOTEST_DEVPATH
#  define CONFIG_EXAMPLES_AUDIOTEST_DEVPATH "/dev/audio/pcm0"
#endif

#define DEFAULT_RATE        24000
#define DEFAULT_SECONDS     2
#define DEFAULT_TONE_HZ     440

#define AMPLITUDE           20000  /* Of 32767: loud, well clear of clipping */

/* The canonical 44-byte WAV header, written as bytes.
 *
 * Deliberately not a struct: a struct with these field types is padded to 48
 * bytes by alignment rules, and writing sizeof(struct) bytes would hand the
 * decoder four bytes of padding that are not part of the format.
 */

#define WAV_HEADER_SIZE     44

/* Bytes generated per write when producing a file.  A multiple of four so a
 * chunk boundary always falls on a whole stereo frame -- audiotest_fill()
 * indexes frame by absolute stream offset, but keeping chunks frame-aligned
 * avoids a partial frame at the end of every write.
 */

#define WAV_CHUNK_BYTES     4096

#define WAV_OFF_RIFF         0
#define WAV_OFF_SIZE         4
#define WAV_OFF_WAVE         8
#define WAV_OFF_FMT         12
#define WAV_OFF_FMTSIZE     16
#define WAV_OFF_FORMAT      20
#define WAV_OFF_CHANNELS    22
#define WAV_OFF_RATE        24
#define WAV_OFF_BYTERATE    28
#define WAV_OFF_ALIGN       32
#define WAV_OFF_BITS        34
#define WAV_OFF_DATA        36
#define WAV_OFF_DATASIZE    40

#ifndef CONFIG_EXAMPLES_AUDIOTEST_MQNAME
#  define CONFIG_EXAMPLES_AUDIOTEST_MQNAME "/tmp/audiotest_mq"
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Completion bookkeeping.  The driver decrements inflight through the
 * callback, so the drain wait is a real handshake rather than a sleep that is
 * probably long enough.
 */

struct audiotest_state_s
{
  volatile int  inflight;         /* Buffers still owned by the driver */
  volatile int  completed;
  volatile bool failed;
  bool          stream_complete;  /* Driver reported AUDIO_MSG_COMPLETE */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct audiotest_state_s g_state;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void put32(FAR uint8_t *p, uint32_t v)
{
  p[0] = v & 0xff;
  p[1] = (v >> 8) & 0xff;
  p[2] = (v >> 16) & 0xff;
  p[3] = (v >> 24) & 0xff;
}

static void put16(FAR uint8_t *p, uint16_t v)
{
  p[0] = v & 0xff;
  p[1] = (v >> 8) & 0xff;
}

/****************************************************************************
 * Name: audiotest_usage
 ****************************************************************************/

static void audiotest_usage(FAR const char *progname)
{
  printf("Usage: %s [options]\n", progname);
  printf("  -d <path>   audio device (default %s)\n",
         CONFIG_EXAMPLES_AUDIOTEST_DEVPATH);
  printf("              with -c this is the input device\n");
  printf("  -r <hz>     sample rate (default %d; prefer 16000/24000)\n",
         DEFAULT_RATE);
  printf("  -t <secs>   duration in seconds (default %d)\n", DEFAULT_SECONDS);
  printf("  -f <hz>     tone frequency (default %d)\n", DEFAULT_TONE_HZ);
  printf("  -s          sweep instead of a steady tone\n");
  printf("  -w <file>   write a WAV file instead of playing (for nxplayer)\n");
  printf("  -c <secs>   capture from the input device and report levels\n");
  printf("  -h          this text\n");
  printf("\n");
  printf("With -w the stream is written to a file, so the standard player can\n");
  printf("consume it:  %s -w /tmp/t.wav ; then  nxplayer> device pcm0 ;\n", progname);
  printf("nxplayer> play /tmp/t.wav\n");
}

/****************************************************************************
 * Name: audiotest_header
 *
 * Description:
 *   Lay the 44-byte WAV header into a buffer that begins at `offset` in the
 *   stream, copying only the part that actually falls inside it.
 *
 ****************************************************************************/

static void audiotest_header(FAR uint8_t *buf, size_t offset, size_t nbytes,
                             uint32_t payload, int rate)
{
  uint8_t hdr[WAV_HEADER_SIZE];
  size_t room;

  if (offset >= WAV_HEADER_SIZE)
    {
      return;
    }

  room = WAV_HEADER_SIZE - offset;
  if (room > nbytes)
    {
      room = nbytes;
    }

  memset(hdr, 0, sizeof(hdr));
  memcpy(hdr + WAV_OFF_RIFF, "RIFF", 4);
  put32(hdr + WAV_OFF_SIZE, 36 + payload);
  memcpy(hdr + WAV_OFF_WAVE, "WAVE", 4);
  memcpy(hdr + WAV_OFF_FMT, "fmt ", 4);
  put32(hdr + WAV_OFF_FMTSIZE, 16);
  put16(hdr + WAV_OFF_FORMAT, 1);            /* PCM                */
  put16(hdr + WAV_OFF_CHANNELS, 2);          /* stereo             */
  put32(hdr + WAV_OFF_RATE, (uint32_t)rate);
  put32(hdr + WAV_OFF_BYTERATE, (uint32_t)rate * 4);
  put16(hdr + WAV_OFF_ALIGN, 4);
  put16(hdr + WAV_OFF_BITS, 16);
  memcpy(hdr + WAV_OFF_DATA, "data", 4);
  put32(hdr + WAV_OFF_DATASIZE, payload);

  memcpy(buf, hdr + offset, room);
}

/****************************************************************************
 * Name: audiotest_fill
 *
 * Description:
 *   Produce the stream bytes for one buffer.
 *
 *   Sample frames are indexed from the start of the whole stream, so the tone
 *   stays phase-continuous across buffer boundaries.  Resetting the phase per
 *   buffer would put a click at every join and make a working codec sound
 *   broken.  Bytes past the end of the stream are zero-filled so the caller can
 *   always hand the driver a full buffer.
 *
 ****************************************************************************/

static void audiotest_fill(FAR uint8_t *buf, size_t offset, size_t nbytes,
                           uint32_t payload, int rate, int tone, bool sweep)
{
  size_t end = (size_t)WAV_HEADER_SIZE + payload;
  uint32_t nframes = payload / 4;
  size_t i;

  memset(buf, 0, nbytes);

  audiotest_header(buf, offset, nbytes, payload, rate);

  for (i = 0; i + 3 < nbytes; i += 4)
    {
      size_t pos = offset + i;
      size_t frame;
      double f;
      double phase;
      int16_t s;

      if (pos < WAV_HEADER_SIZE || pos + 3 >= end)
        {
          continue;
        }

      frame = (pos - WAV_HEADER_SIZE) / 4;

      if (sweep)
        {
          f = (double)tone * (1.0 + 3.0 * (double)frame /
                              (double)(nframes ? nframes : 1));
        }
      else
        {
          f = (double)tone;
        }

      phase = 2.0 * M_PI * f * (double)frame / (double)rate;
      s     = (int16_t)(AMPLITUDE * sin(phase));

      /* Both channels get the same sample: the codec takes the left slot, and
       * driving both keeps the stream well formed for any sink that expects
       * interleaved stereo.
       */

      buf[i]     = s & 0xff;
      buf[i + 1] = (s >> 8) & 0xff;
      buf[i + 2] = s & 0xff;
      buf[i + 3] = (s >> 8) & 0xff;
    }
}

/****************************************************************************
 * Name: audiotest_dispatch
 *
 * Description:
 *   Drain completion messages until a buffer comes back, or the stream ends.
 *
 *   The audio upper half reports completion through a POSIX message queue, not
 *   through a callback: AUDIOIOC_REGISTERMQ takes an mqd_t, and the driver
 *   posts AUDIO_MSG_DEQUEUE with each finished buffer and AUDIO_MSG_COMPLETE
 *   once the last one is done.  An earlier version passed a function pointer to
 *   that ioctl and got EBADF, which is why the completion wait had to be a
 *   timeout guess.
 *
 * Return Value:
 *   True if the stream reported complete.
 *
 ****************************************************************************/

static bool audiotest_dispatch(mqd_t mq,
                               FAR struct ap_buffer_s **buffers,
                               FAR bool *inuse, int nbuffers)
{
  struct audio_msg_s msg;
  unsigned int prio;
  bool complete = false;

  for (; ; )
    {
      ssize_t n = mq_receive(mq, (FAR char *)&msg, sizeof(msg), &prio);
      if (n < 0)
        {
          /* EAGAIN means nothing is ready right now; the caller will come
           * back.  Any other error means the queue is gone, which ends the
           * wait rather than spinning on it.
           */

          break;
        }

      switch (msg.msg_id)
        {
          case AUDIO_MSG_DEQUEUE:
            {
              int i;

              g_state.inflight--;
              g_state.completed++;

              /* Mark the returned buffer as ours again, so the pump loop can
               * refill it.  Matching on the pointer is what ties the message
               * to a specific buffer.
               */

              for (i = 0; i < nbuffers; i++)
                {
                  if (buffers[i] == (FAR struct ap_buffer_s *)msg.u.ptr)
                    {
                      inuse[i] = false;
                      break;
                    }
                }

              return false;
            }

          case AUDIO_MSG_COMPLETE:
            complete = true;
            break;

          default:
            break;
        }

      if (complete)
        {
          break;
        }
    }

  return complete;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/


/****************************************************************************
 * Name: audiotest_capture
 *
 * Description:
 *   Record from the input device for a few seconds and report what came back.
 *
 *   The value of this check is not the audio itself but the proof that the
 *   capture direction is wired: buffers go in empty, the codec's ADC fills them
 *   through I2S/GDMA, and they come back with a plausible signal.  "All zeros"
 *   and "all full scale" are the two failure signatures, so both are called out
 *   explicitly rather than being left for the reader to spot in an RMS figure.
 *
 *   Like the playback path, this drives the buffer ioctls rather than read():
 *   the audio upper half's read() also just forwards to the lower half, which
 *   does not implement it.
 *
 ****************************************************************************/

static int audiotest_capture(FAR const char *devpath, int seconds, int rate)
{
  struct audio_caps_desc_s cap_desc;
  struct audio_buf_desc_s buf_desc;
  struct ap_buffer_info_s buf_info;
  FAR struct ap_buffer_s **buffers = NULL;
  struct mq_attr attr;
  mqd_t mq = (mqd_t)-1;
  bool mq_ok = false;
  bool started = false;
  int fd = -1;
  int chunks = 0;
  int x;
  int32_t peak = 0;
  uint64_t sumsq = 0;
  uint64_t nsamp = 0;
  int guard;

  memset(&buf_info, 0, sizeof(buf_info));

  fd = open(devpath, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    {
      printf("audiotest: cannot open %s: %d\n", devpath, errno);
      return ERROR;
    }

  memset(&cap_desc, 0, sizeof(cap_desc));
  cap_desc.caps.ac_len            = sizeof(struct audio_caps_s);
  cap_desc.caps.ac_type           = AUDIO_TYPE_INPUT;
  cap_desc.caps.ac_channels       = 2;
  cap_desc.caps.ac_chmap          = 3;
  cap_desc.caps.ac_controls.hw[0] = (uint16_t)rate;
  cap_desc.caps.ac_controls.b[2]  = 16;

  if (ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&cap_desc) < 0)
    {
      printf("audiotest: input CONFIGURE failed: %d\n", errno);
      goto errout;
    }

  if (ioctl(fd, AUDIOIOC_GETBUFFERINFO, (unsigned long)&buf_info) < 0)
    {
      printf("audiotest: input GETBUFFERINFO failed: %d\n", errno);
      goto errout;
    }

  memset(&attr, 0, sizeof(attr));
  attr.mq_maxmsg  = buf_info.nbuffers + 8;
  attr.mq_msgsize = sizeof(struct audio_msg_s);
  mq_unlink("/tmp/audiotest_in_mq");
  /* Non-blocking: the receive loop must be able to fall through to its
   * timeout guard when the input direction produces nothing.  Opening this
   * queue blocking made a silent capture path look like a hang instead of
   * reporting it.
   */

  mq = mq_open("/tmp/audiotest_in_mq", O_RDWR | O_CREAT | O_NONBLOCK,
               0644, &attr);
  if (mq != (mqd_t)-1)
    {
      mq_ok = true;
      ioctl(fd, AUDIOIOC_REGISTERMQ, (unsigned long)mq);
    }

  buffers = calloc(buf_info.nbuffers, sizeof(FAR struct ap_buffer_s *));
  if (buffers == NULL)
    {
      printf("audiotest: out of memory\n");
      goto errout;
    }

  for (x = 0; x < buf_info.nbuffers; x++)
    {
      buf_desc.numbytes  = buf_info.buffer_size;
      buf_desc.u.pbuffer = &buffers[x];
      if (ioctl(fd, AUDIOIOC_ALLOCBUFFER, (unsigned long)&buf_desc) !=
          sizeof(buf_desc))
        {
          printf("audiotest: input ALLOCBUFFER %d failed: %d\n", x, errno);
          goto errout;
        }
    }

  if (ioctl(fd, AUDIOIOC_START, 0) < 0)
    {
      printf("audiotest: input START failed: %d\n", errno);
      goto errout;
    }

  started = true;

  printf("audiotest: capturing from %s, %d Hz, 16-bit, %d s\n",
         devpath, rate, seconds);

  /* Enqueue every buffer once; the driver fills each and hands it back. */

  for (x = 0; x < buf_info.nbuffers; x++)
    {
      buffers[x]->nbytes  = buf_info.buffer_size;
      buffers[x]->curbyte = 0;
      buffers[x]->flags   = 0;
      buf_desc.numbytes   = buf_info.buffer_size;
      buf_desc.u.buffer   = buffers[x];

      if (ioctl(fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)&buf_desc) < 0)
        {
          printf("audiotest: input ENQUEUEBUFFER %d failed: %d\n", x, errno);
          goto errout;
        }
    }

  guard = (seconds + 5) * 100;

  while (chunks < seconds * 4 && guard-- > 0)
    {
      struct audio_msg_s msg;
      unsigned int prio = 0;

      if (!mq_ok)
        {
          break;
        }

      if (mq_receive(mq, (FAR char *)&msg, sizeof(msg), &prio) < 0)
        {
          usleep(10000);
          continue;
        }

      if (msg.msg_id == AUDIO_MSG_COMPLETE)
        {
          break;
        }

      if (msg.msg_id == AUDIO_MSG_DEQUEUE)
        {
          FAR struct ap_buffer_s *apb = (FAR struct ap_buffer_s *)msg.u.ptr;
          FAR const int16_t *samp = (FAR const int16_t *)apb->samp;
          uint32_t n = apb->nbytes / sizeof(int16_t);
          uint32_t i;

          for (i = 0; i < n; i++)
            {
              int32_t v = samp[i];
              int32_t a = v < 0 ? -v : v;

              if (a > peak)
                {
                  peak = a;
                }

              sumsq += (uint64_t)(v * v);
              nsamp++;
            }

          chunks++;

          /* Re-arm the returned buffer. */

          apb->nbytes  = apb->nmaxbytes;
          apb->curbyte = 0;
          apb->flags   = 0;
          buf_desc.numbytes = apb->nmaxbytes;
          buf_desc.u.buffer = apb;
          ioctl(fd, AUDIOIOC_ENQUEUEBUFFER, (unsigned long)&buf_desc);
        }
    }

  if (nsamp == 0)
    {
      printf("audiotest: no capture data arrived\n");
      goto errout;
    }

  printf("audiotest: captured %d chunk(s), %llu samples\n",
         chunks, (unsigned long long)nsamp);
  printf("audiotest: peak=%ld rms=%lld (of 32767)\n",
         (long)peak, (long long)(nsamp ? (int64_t)sqrt((double)sumsq /
                        (double)nsamp) : 0));

  if (peak == 0)
    {
      printf("audiotest: signal is all zero -- input path carrying silence\n");
    }
  else
    {
      printf("audiotest: input path is live\n");
    }

  return OK;

errout:
  if (started)
    {
      ioctl(fd, AUDIOIOC_STOP, 0);
    }

  if (mq_ok)
    {
      ioctl(fd, AUDIOIOC_UNREGISTERMQ, (unsigned long)mq);
      mq_close(mq);
      mq_unlink("/tmp/audiotest_in_mq");
    }

  if (buffers != NULL)
    {
      for (x = 0; x < buf_info.nbuffers; x++)
        {
          if (buffers[x] != NULL)
            {
              buf_desc.numbytes = 0;
              buf_desc.u.buffer = buffers[x];
              ioctl(fd, AUDIOIOC_FREEBUFFER, (unsigned long)&buf_desc);
            }
        }

      free(buffers);
    }

  if (fd >= 0)
    {
      close(fd);
    }

  return ERROR;
}

int main(int argc, FAR char *argv[])
{
  FAR const char *devpath = CONFIG_EXAMPLES_AUDIOTEST_DEVPATH;
  struct audio_caps_desc_s cap_desc;
  struct audio_buf_desc_s buf_desc;
  struct ap_buffer_info_s buf_info;
  FAR struct ap_buffer_s **buffers = NULL;
  FAR bool *inuse = NULL;
  size_t total;
  size_t queued = 0;
  uint32_t payload;
  int rate = DEFAULT_RATE;
  int seconds = DEFAULT_SECONDS;
  int tone = DEFAULT_TONE_HZ;
  bool sweep = false;
  FAR const char *progname = argv[0];
  struct mq_attr attr;
  mqd_t mq = (mqd_t)-1;
  bool started = false;
  bool mq_open_ok = false;
  bool drain_reported = false;
  FAR const char *wavpath = NULL;
  int capture_secs = 0;
  int fd = -1;
  int x;
  int opt;

  while ((opt = getopt(argc, argv, "d:r:t:f:w:c:sh")) != ERROR)
    {
      switch (opt)
        {
          case 'd':
            devpath = optarg;
            break;
          case 'r':
            rate = atoi(optarg);
            break;
          case 't':
            seconds = atoi(optarg);
            break;
          case 'f':
            tone = atoi(optarg);
            break;
          case 's':
            sweep = true;
            break;
          case 'w':
            wavpath = optarg;
            break;
          case 'c':
            capture_secs = atoi(optarg);
            break;
          case 'h':
          default:
            audiotest_usage(progname);
            return opt == 'h' ? OK : ERROR;
        }
    }

  if (rate < 8000 || rate > 48000)
    {
      printf("audiotest: sample rate %d is out of range (8000..48000)\n", rate);
      return ERROR;
    }

  if (seconds <= 0)
    {
      seconds = DEFAULT_SECONDS;
    }

  memset(&buf_info, 0, sizeof(buf_info));

  payload = (uint32_t)rate * (uint32_t)seconds * 4;   /* 16-bit stereo */
  total   = WAV_HEADER_SIZE + payload;

  /* -w writes the same stream to a file instead of the device.
   *
   * The point is to exercise the *standard* playback path rather than this
   * program's: write a real WAV, then let nxplayer drive /dev/audio/pcm0.  If
   * nxplayer plays the file, then anything else that speaks the audio
   * interface -- including a voice/TTS channel -- will work too.  Synthesising
   * straight into the driver only proves the driver.
   */

  if (wavpath != NULL)
    {
      FAR uint8_t *buf;
      size_t off = 0;
      int wfd;

      buf = malloc(WAV_CHUNK_BYTES);
      if (buf == NULL)
        {
          printf("audiotest: out of memory\n");
          return ERROR;
        }

      wfd = creat(wavpath, 0644);
      if (wfd < 0)
        {
          printf("audiotest: cannot create %s: %d\n", wavpath, errno);
          free(buf);
          return ERROR;
        }

      while (off < total)
        {
          size_t n = total - off;

          if (n > WAV_CHUNK_BYTES)
            {
              n = WAV_CHUNK_BYTES;
            }

          audiotest_fill(buf, off, n, payload, rate, tone, sweep);

          if (write(wfd, buf, n) != (ssize_t)n)
            {
              printf("audiotest: write to %s failed: %d\n", wavpath, errno);
              close(wfd);
              free(buf);
              return ERROR;
            }

          off += n;
        }

      close(wfd);
      free(buf);

      printf("audiotest: wrote %s (%u bytes: 44-byte header + %u PCM)\n",
             wavpath, (unsigned)total, (unsigned)payload);
      printf("audiotest: play it with:  nxplayer\n");
      printf("audiotest:                 device pcm0\n");
      printf("audiotest:                 play %s\n", wavpath);
      return OK;
    }

  if (capture_secs > 0)
    {
      return audiotest_capture(devpath, capture_secs, rate);
    }

  fd = open(devpath, O_WRONLY | O_CLOEXEC);
  if (fd < 0)
    {
      printf("audiotest: cannot open %s: %d\n", devpath, errno);
      return ERROR;
    }

  /* Select the output function and describe the stream.  Without this the
   * codec keeps whatever rate and width it was last configured with.
   */

  memset(&cap_desc, 0, sizeof(cap_desc));
  cap_desc.caps.ac_len            = sizeof(struct audio_caps_s);
  cap_desc.caps.ac_type           = AUDIO_TYPE_OUTPUT;
  cap_desc.caps.ac_channels       = 2;
  cap_desc.caps.ac_chmap          = 3;            /* Front left + right */
  /* ac_controls is a union of uint8_t b[4] / uint16_t hw[2] / uint32_t w, so
   * hw[0] and b[0..1] are the same storage.  Writing hw[0] = rate and then b[2]
   * = width therefore stores a 16-bit rate and a 16-bit width side by side --
   * and an earlier version that also wrote b[3] = rate >> 16 stored the high
   * order of the rate *into the width*, which the codec read back as a width
   * of 0 and reported as a request for 48 kHz.
   *
   * The driver reads the rate from hw[0] and the width from b[2].
   */

  cap_desc.caps.ac_controls.hw[0] = (uint16_t)rate;
  cap_desc.caps.ac_controls.b[2]  = 16;           /* Bits per sample    */

  if (ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&cap_desc) < 0)
    {
      printf("audiotest: AUDIOIOC_CONFIGURE failed: %d\n", errno);
      goto errout;
    }

  if (ioctl(fd, AUDIOIOC_GETBUFFERINFO, (unsigned long)&buf_info) < 0)
    {
      printf("audiotest: AUDIOIOC_GETBUFFERINFO failed: %d\n", errno);
      goto errout;
    }

  if (buf_info.nbuffers <= 0 || buf_info.buffer_size <= 0)
    {
      printf("audiotest: driver reported %d buffers of %d bytes\n",
             buf_info.nbuffers, buf_info.buffer_size);
      goto errout;
    }

  /* Open the completion queue and register it before anything is enqueued, or
   * the first completions would be missed and the drain wait would never end.
   *
   * mq_msgsize is set to MQ_MAXMSGSIZE because the kernel rejects a receive
   * buffer smaller than the queue's declared maximum; struct audio_msg_s is
   * smaller than that, but the queue attribute is what the check uses.
   */

  /* mq_msgsize must be exactly the size mq_receive() will ask for.
   *
   * POSIX rejects a receive whose buffer is smaller than the queue's declared
   * maximum with EMSGSIZE -- it does not truncate.  This program sets
   * mq_msgsize to sizeof(struct audio_msg_s) so the two agree, which is what
   * nxplayer does.  (An earlier version declared CONFIG_MQ_MAXMSGSIZE, which is
   * larger, so every receive failed instantly, the completion loop spun, and
   * the enqueue call below then blocked on a full queue.  That looked like a
   * driver stall but was this one line.)
   */

  memset(&attr, 0, sizeof(attr));
  attr.mq_maxmsg  = buf_info.nbuffers + 8;
  attr.mq_msgsize = sizeof(struct audio_msg_s);
  attr.mq_curmsgs = 0;
  attr.mq_flags   = 0;

  mq_unlink(CONFIG_EXAMPLES_AUDIOTEST_MQNAME);
  mq = mq_open(CONFIG_EXAMPLES_AUDIOTEST_MQNAME,
               O_RDWR | O_CREAT, 0644, &attr);
  if (mq == (mqd_t)-1)
    {
      printf("audiotest: WARNING - cannot open the completion queue (%d); "
             "the drain wait falls back to a timeout\n", errno);
    }
  else
    {
      mq_open_ok = true;

      if (ioctl(fd, AUDIOIOC_REGISTERMQ, (unsigned long)mq) < 0)
        {
          printf("audiotest: WARNING - cannot register the completion "
                 "queue (%d); the drain wait falls back to a timeout\n",
                 errno);
        }
    }

  inuse = calloc(buf_info.nbuffers, sizeof(bool));
  buffers = calloc(buf_info.nbuffers, sizeof(FAR struct ap_buffer_s *));
  if (buffers == NULL || inuse == NULL)
    {
      printf("audiotest: out of memory\n");
      goto errout;
    }

  for (x = 0; x < buf_info.nbuffers; x++)
    {
      buf_desc.numbytes  = buf_info.buffer_size;
      buf_desc.u.pbuffer = &buffers[x];

      if (ioctl(fd, AUDIOIOC_ALLOCBUFFER, (unsigned long)&buf_desc) !=
          sizeof(buf_desc))
        {
          printf("audiotest: AUDIOIOC_ALLOCBUFFER %d failed: %d\n", x, errno);
          goto errout;
        }
    }

  printf("audiotest: %s, %d Hz, 16-bit stereo, %d s, %s %d Hz\n",
         devpath, rate, seconds, sweep ? "sweep from" : "tone", tone);
  printf("audiotest: %u stream bytes in %d x %d buffers\n",
         (unsigned)total, buf_info.nbuffers, buf_info.buffer_size);

  if (ioctl(fd, AUDIOIOC_START, 0) < 0)
    {
      printf("audiotest: AUDIOIOC_START failed: %d\n", errno);
      goto errout;
    }

  started = true;
  printf("audiotest: playing...\n");

  /* Pump the stream.
   *
   * The shape of this loop matters.  A driver only accepts a bounded number of
   * buffers, so the program cannot simply enqueue everything and wait: it has
   * to try to fill the pipeline, and when the pipeline is full, wait for the
   * device to hand a buffer back before trying again.
   *
   * An earlier version tried to enqueue all buffers and then waited for
   * inflight to fall below the count -- but that wait sat *inside* a loop whose
   * condition was still true, so with every buffer already in flight the
   * program waited for a buffer to be freed while holding the only code path
   * that could have freed it.  It deadlocked itself, not the driver.  Trying to
   * enqueue first and only then waiting is what makes each iteration able to
   * make progress.
   */

  /* Pump: keep every buffer the driver has returned full and enqueued.
   *
   * The driver owns a buffer from AUDIOIOC_ENQUEUEBUFFER until it posts
   * AUDIO_MSG_DEQUEUE for it.  Enqueueing one that is still owned -- which is
   * what an earlier version of this loop did by always reusing buffers[0] --
   * queues the same audio pipeline buffer repeatedly and eventually blocks
   * inside the ioctl.  So the loop only ever hands over a buffer it has been
   * told is free, and it waits on the completion queue rather than spinning.
   */

  while (queued < total || g_state.inflight > 0)
    {
      bool progressed = false;

      if (queued < total)
        {
          for (x = 0; x < buf_info.nbuffers && queued < total; x++)
            {
              FAR struct ap_buffer_s *apb = buffers[x];
              size_t n;

              if (inuse[x])
                {
                  continue;
                }

              n = apb->nmaxbytes;
              if (n > total - queued)
                {
                  n = total - queued;
                }

              audiotest_fill(apb->samp, queued, n, payload, rate, tone, sweep);

              apb->nbytes  = n;
              apb->curbyte = 0;
              apb->flags   = (queued + n >= total) ? AUDIO_APB_FINAL : 0;

              buf_desc.numbytes = n;
              buf_desc.u.buffer = apb;

              if (ioctl(fd, AUDIOIOC_ENQUEUEBUFFER,
                        (unsigned long)&buf_desc) < 0)
                {
                  printf("audiotest: AUDIOIOC_ENQUEUEBUFFER failed: %d\n",
                         errno);
                  goto errout;
                }

              inuse[x] = true;
              g_state.inflight++;
              queued += n;
              progressed = true;
            }
        }

      if (g_state.stream_complete || (!progressed && g_state.inflight == 0))
        {
          break;
        }

      if (!progressed)
        {
          /* Everything the driver will take is enqueued; sleep until it hands
           * one back.  With no queue to wait on, fall back to a bounded nap so
           * the program still terminates.
           */

          if (mq_open_ok)
            {
              if (audiotest_dispatch(mq, buffers, inuse, buf_info.nbuffers))
                {
                  g_state.stream_complete = true;
                }
            }
          else
            {
              usleep(5000);
            }
        }
    }

  /* Give the last buffers their time on the wire before tearing the stream
   * down; closing the device mid-transfer would cut the final samples off.
   */

  if (g_state.inflight > 0)
    {
      int guard = (seconds + 5) * 200;

      while (g_state.inflight > 0 && guard-- > 0)
        {
          if (mq_open_ok &&
              audiotest_dispatch(mq, buffers, inuse, buf_info.nbuffers))
            {
              break;
            }

          usleep(5000);
        }
    }

  printf("audiotest: queued %u bytes\n", (unsigned)queued);

  printf("audiotest: done\n");

out:
  if (started)
    {
      ioctl(fd, AUDIOIOC_STOP, 0);
    }

  if (mq_open_ok)
    {
      ioctl(fd, AUDIOIOC_UNREGISTERMQ, (unsigned long)mq);
      mq_close(mq);
      mq_unlink(CONFIG_EXAMPLES_AUDIOTEST_MQNAME);
    }

  if (buffers != NULL)
    {
      for (x = 0; x < buf_info.nbuffers; x++)
        {
          if (buffers[x] != NULL)
            {
              buf_desc.numbytes = 0;
              buf_desc.u.buffer = buffers[x];
              ioctl(fd, AUDIOIOC_FREEBUFFER, (unsigned long)&buf_desc);
            }
        }

      free(buffers);
    }

  free(inuse);

  if (fd >= 0)
    {
      close(fd);
    }

  return g_state.failed ? ERROR : OK;

errout:
  g_state.failed = true;
  goto out;
}
