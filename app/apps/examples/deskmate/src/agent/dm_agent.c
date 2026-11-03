/****************************************************************************
 * apps/examples/deskmate/src/agent/dm_agent.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Agent bridge: see dm_agent.h.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/time.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <time.h>
#include <unistd.h>

#include <lvgl/lvgl.h>

#include "agent/dm_agent.h"
#include "dm_log.h"
#include "hal/dm_net.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_EXAMPLES_DESKMATE_AGENT_HOST
#  define DM_AGENT_HOST "127.0.0.1"
#else
#  define DM_AGENT_HOST CONFIG_EXAMPLES_DESKMATE_AGENT_HOST
#endif

#ifndef CONFIG_EXAMPLES_DESKMATE_AGENT_PORT
#  define DM_AGENT_PORT 28789
#else
#  define DM_AGENT_PORT CONFIG_EXAMPLES_DESKMATE_AGENT_PORT
#endif

#define DM_AGENT_CHAT_ID   "deskmate-ui"
#define DM_AGENT_QDEPTH    8
#define DM_AGENT_MSGLEN    512

/****************************************************************************
 * Private Types
 ****************************************************************************/

enum dm_agent_mode_e
{
  DM_AGENT_OFF = 0,
  DM_AGENT_MOCK,
  DM_AGENT_WS
};

struct dm_agent_msg_s
{
  char text[DM_AGENT_MSGLEN];
  bool proactive;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static enum dm_agent_mode_e g_mode = DM_AGENT_MOCK;
static char g_host[48] = DM_AGENT_HOST;
static int  g_port = DM_AGENT_PORT;
static int  g_fd = -1;
static bool g_ws_ready;

static struct dm_agent_msg_s g_queue[DM_AGENT_QDEPTH];
static int g_head;
static int g_tail;

/* 最近一条**来自智能体**的消息（Agent 页展示"真实回复"用）。 */

static char g_last_recv[DM_AGENT_MSGLEN];

static unsigned int g_sent;
static unsigned int g_recv;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void dm_agent_enqueue(const char *text, bool proactive)
{
  int next = (g_head + 1) % DM_AGENT_QDEPTH;

  if (next == g_tail)
    {
      DM_WARN("agent: queue full, dropping message");
      return;
    }

  strlcpy(g_queue[g_head].text, text, DM_AGENT_MSGLEN);
  g_queue[g_head].proactive = proactive;
  g_head = next;

  if (!proactive)
    {
      strlcpy(g_last_recv, text, sizeof(g_last_recv));
    }

  if (proactive)
    {
      DM_LOG("agent: proactive message: %.60s", text);
    }
}

/* ── offline responder ─────────────────────────────────────────────────── */

static bool dm_agent_contains(const char *haystack, const char *needle)
{
  return haystack != NULL && needle != NULL && strstr(haystack, needle) != NULL;
}

static void dm_agent_mock_answer(const char *text)
{
  char reply[DM_AGENT_MSGLEN];
  const struct dm_net_status_s *net;

  if (dm_agent_contains(text, "time") || dm_agent_contains(text, "clock") ||
      dm_agent_contains(text, "几点"))
    {
      time_t now = time(NULL);
      struct tm tm_buf;

      if (localtime_r(&now, &tm_buf) != NULL)
        {
          snprintf(reply, sizeof(reply),
                   "现在是 %02d:%02d:%02d。我在离线应答模式，这个答案由设备本机给出。",
                   tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
        }
      else
        {
          snprintf(reply, sizeof(reply), "读不到时钟。");
        }
    }
  else if (dm_agent_contains(text, "network") ||
           dm_agent_contains(text, "wifi") ||
           dm_agent_contains(text, "ethernet") ||
           dm_agent_contains(text, "网络"))
    {
      char desc[192];

      (void)dm_net_refresh();
      net = dm_net_status();
      (void)dm_net_describe(desc, sizeof(desc));

      snprintf(reply, sizeof(reply),
               "接口：%s。%s", desc,
               (net != NULL && net->online) ? "链路是通的。" : "还没有地址。");
    }
  else if (dm_agent_contains(text, "camera") ||
           dm_agent_contains(text, "see") ||
           dm_agent_contains(text, "相机"))
    {
      snprintf(reply, sizeof(reply),
               "摄像头页正在用 %s 后端采集。在那页点「抓拍」可以留一帧。",
               "camera");
    }
  else if (dm_agent_contains(text, "note") ||
           dm_agent_contains(text, "记"))
    {
      snprintf(reply, sizeof(reply),
               "记下了。完整产品里这是「速记」技能：把这句话整理成结构化提醒，"
               "交给定时任务。" );
    }
  else if (dm_agent_contains(text, "hello") ||
           dm_agent_contains(text, "hi") ||
           dm_agent_contains(text, "你好"))
    {
      snprintf(reply, sizeof(reply),
               "你好，我是桌伴，openvela 中控屏。可以问我时间、网络状态或摄像头画面。");
    }
  else
    {
      snprintf(reply, sizeof(reply),
               "我听到的是「%.120s」。这里是离线应答；接上 ai_agent 的 WebSocket "
               "后端后，这句话会转给大模型。", text);
    }

  g_recv++;
  dm_agent_enqueue(reply, false);
}

/* ── minimal WebSocket client ──────────────────────────────────────────── */

/* Read exactly n bytes with a bounded wait. */

static int dm_agent_recv_exact(int fd, uint8_t *buf, size_t n, int timeout_ms)
{
  size_t got = 0;

  while (got < n)
    {
      struct pollfd pfd = { .fd = fd, .events = POLLIN };
      int pr = poll(&pfd, 1, timeout_ms);

      if (pr == 0)
        {
          return -ETIMEDOUT;
        }

      if (pr < 0)
        {
          return -errno;
        }

      {
        ssize_t r = recv(fd, buf + got, n - got, 0);

        if (r <= 0)
          {
            return -ECONNRESET;
          }

        got += (size_t)r;
      }
    }

  return 0;
}

static int dm_agent_ws_handshake(void)
{
  char request[256];
  char response[512];
  size_t used = 0;

  snprintf(request, sizeof(request),
           "GET / HTTP/1.1\r\n"
           "Host: %s:%d\r\n"
           "Upgrade: websocket\r\n"
           "Connection: Upgrade\r\n"
           "Sec-WebSocket-Key: ZGVza21hdGUtc2VsZnRlc3Q=\r\n"
           "Sec-WebSocket-Version: 13\r\n\r\n",
           g_host, g_port);

  if (send(g_fd, request, strlen(request), 0) < 0)
    {
      return -errno;
    }

  /* Read the header block; the ai_agent server answers 101 and sends nothing
   * else until a message is pushed to us.
   */

  while (used + 1 < sizeof(response))
    {
      struct pollfd pfd = { .fd = g_fd, .events = POLLIN };
      ssize_t r;

      if (poll(&pfd, 1, 2000) <= 0)
        {
          return -ETIMEDOUT;
        }

      r = recv(g_fd, response + used, sizeof(response) - 1 - used, 0);
      if (r <= 0)
        {
          return -ECONNRESET;
        }

      used += (size_t)r;
      response[used] = '\0';

      if (strstr(response, "\r\n\r\n") != NULL)
        {
          break;
        }
    }

  if (strstr(response, "101") == NULL)
    {
      DM_WARN("agent: websocket handshake rejected: %.40s", response);
      return -EPROTO;
    }

  return 0;
}

static int dm_agent_ws_frame(const char *text)
{
  uint8_t header[8];
  size_t len = strlen(text);
  size_t hlen;
  uint8_t mask[4] = { 0x12, 0x34, 0x56, 0x78 };
  uint8_t *masked;
  size_t i;

  header[0] = 0x81;                    /* FIN + text */

  if (len < 126)
    {
      header[1] = 0x80 | (uint8_t)len;
      hlen = 2;
    }
  else if (len < 65536)
    {
      header[1] = 0x80 | 126;
      header[2] = (uint8_t)(len >> 8);
      header[3] = (uint8_t)(len & 0xff);
      hlen = 4;
    }
  else
    {
      return -E2BIG;
    }

  memcpy(header + hlen, mask, sizeof(mask));
  hlen += sizeof(mask);

  masked = malloc(len);
  if (masked == NULL)
    {
      return -ENOMEM;
    }

  for (i = 0; i < len; i++)
    {
      masked[i] = (uint8_t)text[i] ^ mask[i % 4];
    }

  if (send(g_fd, header, hlen, 0) < 0 ||
      send(g_fd, masked, len, 0) < 0)
    {
      free(masked);
      return -errno;
    }

  free(masked);
  return 0;
}

/* Try to read one text frame; returns 1 when a message was extracted. */

static int dm_agent_ws_read(char *out, size_t out_len)
{
  uint8_t header[2];
  size_t len;
  size_t got;
  bool masked;

  if (dm_agent_recv_exact(g_fd, header, 2, 0) != 0)
    {
      return 0;
    }

  masked = (header[1] & 0x80) != 0;
  len = header[1] & 0x7f;

  if (len == 126)
    {
      uint8_t ext[2];

      if (dm_agent_recv_exact(g_fd, ext, 2, 1000) != 0)
        {
          return 0;
        }

      len = ((size_t)ext[0] << 8) | ext[1];
    }
  else if (len == 127)
    {
      return 0;                          /* Not used by the agent server */
    }

  if (len >= out_len || len == 0)
    {
      return 0;
    }

  if (masked)
    {
      uint8_t mask[4];

      if (dm_agent_recv_exact(g_fd, mask, 4, 1000) != 0)
        {
          return 0;
        }

      if (dm_agent_recv_exact(g_fd, (uint8_t *)out, len, 1000) != 0)
        {
          return 0;
        }

      for (got = 0; got < len; got++)
        {
          out[got] = (char)(out[got] ^ mask[got % 4]);
        }
    }
  else
    {
      if (dm_agent_recv_exact(g_fd, (uint8_t *)out, len, 1000) != 0)
        {
          return 0;
        }
    }

  out[len] = '\0';

  /* The payload is JSON: pull out "content". */

  {
    const char *key = strstr(out, "\"content\"");
    const char *colon;

    if (key == NULL)
      {
        return 0;
      }

    colon = strchr(key, ':');
    if (colon == NULL)
      {
        return 0;
      }

    colon = strchr(colon, '"');
    if (colon == NULL)
      {
        return 0;
      }

    {
      char *dst = out;
      const char *src = colon + 1;

      while (*src != '\0' && *src != '"')
        {
          if (*src == '\\' && src[1] != '\0')
            {
              src++;

              if (*src == 'n')
                {
                  *dst++ = '\n';
                  src++;
                  continue;
                }
            }

          *dst++ = *src++;
        }

      *dst = '\0';
    }
  }

  return 1;
}

static int dm_agent_ws_connect(void);
static volatile int g_ws_last_try_ms;

static void *dm_agent_ws_worker(void *arg)
{
  (void)arg;

  (void)dm_agent_ws_connect();

  /* 失败就等下一次 poll 触发重连（5s 节流）。 */

  if (!g_ws_ready)
    {
      g_ws_last_try_ms = (int)(clock() / (CLOCKS_PER_SEC / 1000));
    }

  return NULL;
}

static void dm_agent_ws_kick(void)
{
  pthread_t tid;
  pthread_attr_t attr;

  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_attr_setstacksize(&attr, 8192);

  if (pthread_create(&tid, &attr, dm_agent_ws_worker, NULL) != 0)
    {
      DM_WARN("agent: cannot start the ws connect worker");
    }

  pthread_attr_destroy(&attr);
}

static int dm_agent_ws_connect(void)
{
  struct sockaddr_in addr;
  int fd;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port   = htons((uint16_t)g_port);

  if (inet_pton(AF_INET, g_host, &addr.sin_addr) != 1)
    {
      DM_WARN("agent: endpoint %s is not an IPv4 address", g_host);
      return -EINVAL;
    }

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    {
      return -errno;
    }

  /* 有界连接：绝不允许"后端没起来"把调用者（含 UI 启动路径）堵死。
   * 非阻塞 connect + 1.2s 轮询；再给收发各 2s 超时，防止对端不读时 send 卡住。
   */

  {
    int flags = fcntl(fd, F_GETFL, 0);
    struct pollfd pfd = { .fd = fd, .events = POLLOUT };
    int err = 0;
    socklen_t elen = sizeof(err);
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };

    (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 &&
        errno != EINPROGRESS)
      {
        DM_LOG("agent: connect(%s:%d) failed: %d", g_host, g_port, errno);
        close(fd);
        return -errno;
      }

    if (poll(&pfd, 1, 1200) <= 0 ||
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0)
      {
        DM_LOG("agent: connect(%s:%d) timed out or refused", g_host, g_port);
        close(fd);
        return -ETIMEDOUT;
      }

    (void)fcntl(fd, F_SETFL, flags);
  }

  g_fd = fd;

  if (dm_agent_ws_handshake() != 0)
    {
      close(g_fd);
      g_fd = -1;
      return -EPROTO;
    }

  g_ws_ready = true;
  DM_LOG("agent: websocket connected to %s:%d", g_host, g_port);

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int dm_agent_init(const char *mode)
{
  if (mode != NULL)
    {
      if (strcmp(mode, "off") == 0)
        {
          g_mode = DM_AGENT_OFF;
        }
      else if (strcmp(mode, "ws") == 0)
        {
          g_mode = DM_AGENT_WS;
        }
      else
        {
          g_mode = DM_AGENT_MOCK;
        }
    }

  DM_LOG("agent: mode %s, endpoint %s:%d",
         g_mode == DM_AGENT_WS ? "ws" :
         g_mode == DM_AGENT_MOCK ? "mock" : "off", g_host, g_port);

  if (g_mode == DM_AGENT_WS)
    {
      /* 关键：**不要**在这里同步连接。
       * 实测：ai_agent 没起来时阻塞式 connect/handshake 会把整个应用卡在启动路径上
       * （UI 都不出来）。改成后台线程连接 + 失败 5s 后重试。
       */

      dm_agent_ws_kick();
    }

  return 0;
}

void dm_agent_set_endpoint(const char *host, int port)
{
  if (host != NULL && host[0] != '\0')
    {
      strlcpy(g_host, host, sizeof(g_host));
    }

  if (port > 0)
    {
      g_port = port;
    }
}

const char *dm_agent_state(void)
{
  if (g_mode == DM_AGENT_OFF)
    {
      return "off";
    }

  if (g_mode == DM_AGENT_MOCK)
    {
      return "mock";
    }

  return g_ws_ready ? "online" : "offline";
}

int dm_agent_send(const char *text)
{
  if (text == NULL || text[0] == '\0')
    {
      return -EINVAL;
    }

  g_sent++;
  DM_LOG("agent: -> %.80s", text);

  if (g_mode == DM_AGENT_WS)
    {
      char payload[DM_AGENT_MSGLEN + 64];

      if (!g_ws_ready && dm_agent_ws_connect() != 0)
        {
          DM_WARN("agent: no backend, answering locally");
          dm_agent_mock_answer(text);
          return 0;
        }

      snprintf(payload, sizeof(payload),
               "{\"type\":\"message\",\"chat_id\":\"%s\",\"content\":\"%s\"}",
               DM_AGENT_CHAT_ID, text);

      if (dm_agent_ws_frame(payload) == 0)
        {
          return 0;
        }

      DM_WARN("agent: send failed, dropping to the local responder");
      g_ws_ready = false;
    }

  if (g_mode == DM_AGENT_OFF)
    {
      dm_agent_enqueue("The agent bridge is switched off.", false);
      return 0;
    }

  dm_agent_mock_answer(text);
  return 0;
}

int dm_agent_poll(char *buf, size_t len)
{
  if (buf == NULL || len == 0)
    {
      return -EINVAL;
    }

  /* Anything the WebSocket backend pushed at us (including proactive
   * messages the agent sends to "deskmate-ui") comes first.
   */

  if (g_mode == DM_AGENT_WS && !g_ws_ready)
    {
      int now = (int)(clock() / (CLOCKS_PER_SEC / 1000));

      if (now - g_ws_last_try_ms >= 5000)
        {
          g_ws_last_try_ms = now;
          dm_agent_ws_kick();
        }
    }

  if (g_mode == DM_AGENT_WS && g_ws_ready)
    {
      char frame[DM_AGENT_MSGLEN];

      if (dm_agent_ws_read(frame, sizeof(frame)) == 1)
        {
          g_recv++;
          dm_agent_enqueue(frame, false);
        }
    }

  if (g_head == g_tail)
    {
      return 0;
    }

  strlcpy(buf, g_queue[g_tail].text, len);
  g_tail = (g_tail + 1) % DM_AGENT_QDEPTH;

  return 1;
}

unsigned int dm_agent_sent_count(void)
{
  return g_sent;
}

const char *dm_agent_last_recv(void)
{
  return g_last_recv;
}

void dm_agent_reconnect(void)
{
  if (g_mode == DM_AGENT_WS)
    {
      g_ws_ready = false;
      g_ws_last_try_ms = 0;
      dm_agent_ws_kick();
    }
}

unsigned int dm_agent_recv_count(void)
{
  return g_recv;
}

void dm_agent_push_proactive(const char *text)
{
  if (text == NULL || text[0] == '\0')
    {
      return;
    }

  dm_agent_enqueue(text, true);
}

void dm_agent_destroy(void)
{
  if (g_fd >= 0)
    {
      close(g_fd);
      g_fd = -1;
    }

  g_ws_ready = false;
}
