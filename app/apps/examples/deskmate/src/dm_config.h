/****************************************************************************
 * apps/examples/deskmate/src/dm_config.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Compile-time view of the DeskMate configuration.  Everything the rest of
 * the application needs from Kconfig is funnelled through this header so the
 * sources stay free of configuration ifdef soup.
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_DESKMATE_DM_CONFIG_H
#define __APPS_EXAMPLES_DESKMATE_DM_CONFIG_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define DM_APP_NAME      "deskmate"
#define DM_APP_VERSION   "0.1.0"

/* NSH built-in entry point name */

#ifdef CONFIG_EXAMPLES_DESKMATE_PROGNAME
#  define DM_PROGNAME    CONFIG_EXAMPLES_DESKMATE_PROGNAME
#else
#  define DM_PROGNAME    DM_APP_NAME
#endif

/* Display backend --------------------------------------------------------- */

#ifdef CONFIG_EXAMPLES_DESKMATE_DISPLAY_FBDEV
#  define DM_DISPLAY_FBDEV   1
#else
#  define DM_DISPLAY_FBDEV   0
#endif

#define DM_DISPLAY_MEM   (!DM_DISPLAY_FBDEV)

/* In-memory backend geometry */

#ifdef CONFIG_EXAMPLES_DESKMATE_WIDTH
#  define DM_MEM_WIDTH   CONFIG_EXAMPLES_DESKMATE_WIDTH
#else
#  define DM_MEM_WIDTH   1024
#endif

#ifdef CONFIG_EXAMPLES_DESKMATE_HEIGHT
#  define DM_MEM_HEIGHT  CONFIG_EXAMPLES_DESKMATE_HEIGHT
#else
#  define DM_MEM_HEIGHT  600
#endif

/* Framebuffer device */

#if DM_DISPLAY_FBDEV
#  ifdef CONFIG_EXAMPLES_DESKMATE_FB_PATH
#    define DM_FB_PATH   CONFIG_EXAMPLES_DESKMATE_FB_PATH
#  else
#    define DM_FB_PATH   "/dev/fb0"
#  endif
#endif

/* Touchscreen device */

#ifdef CONFIG_EXAMPLES_DESKMATE_INPUT_DEVPATH
#  define DM_INPUT_PATH  CONFIG_EXAMPLES_DESKMATE_INPUT_DEVPATH
#else
#  define DM_INPUT_PATH  "/dev/input0"
#endif

/* Camera capture device (V4L2).  Declared here as well so the NSH usage text
 * can quote the configured path without duplicating the fallback.
 */

#if defined(CONFIG_EXAMPLES_DESKMATE_CAMERA_DEVPATH)
#  define DM_CAMERA_PATH CONFIG_EXAMPLES_DESKMATE_CAMERA_DEVPATH
#elif !defined(DM_CAMERA_PATH)
#  define DM_CAMERA_PATH "/dev/video0"
#endif

/* Optional hostfs mount (simulator snapshot exchange) */

#ifdef CONFIG_EXAMPLES_DESKMATE_HOSTFS_PATH
#  define DM_HOSTFS_PATH CONFIG_EXAMPLES_DESKMATE_HOSTFS_PATH
#else
#  define DM_HOSTFS_PATH ""
#endif

/* Automatic shutdown for scripted runs */

#ifdef CONFIG_EXAMPLES_DESKMATE_EXIT_AFTER
#  define DM_EXIT_AFTER  CONFIG_EXAMPLES_DESKMATE_EXIT_AFTER
#else
#  define DM_EXIT_AFTER  0
#endif

/* Agent bridge default mode and endpoint */

#ifdef CONFIG_EXAMPLES_DESKMATE_AGENT_MODE
#  define DM_AGENT_MODE CONFIG_EXAMPLES_DESKMATE_AGENT_MODE
#else
#  define DM_AGENT_MODE "mock"
#endif

/* Camera preview geometry (both the page and the self-test use it) */

#define DM_CAMERA_PREVIEW_W 320
/* 相机每次开流抓几帧就停：0 = 连续流（正常预览）。
 *
 * 曾经因为"连续流会打停显示"而设成 3，后来定位到真因是 ISP bypass 在相机打开时
 * 才 bring-up（见文档 §16.8/§16.11）——改成开机预配 ISP 之后，连续预览与显示可以共存。
 */

#define DM_CAMERA_SHOT_FRAMES 0

/* 运动检测（采集线程里做，见 C3）：采样步长 7，单点差值超过 DIFF 算"变了"，
 * 变化占比 ≥ PCT 判定为有运动。
 */

#define DM_CAMERA_MOTION_DIFF 1200  /* RGB565 打包值的差 */

/* 让 CSI 驱动走 ISP 的硬件去马赛克、直接输出 RGB565（对应驱动里的
 * CONFIG_ESP_CSI_ISP_RGB565）。两边必须同时打开。
 * 默认关：RAW8 + CPU 转换是当前验证过的路径。
 */

/* #define DM_CSI_ISP_RGB565 1 */

/* 相机的"让路"占空比：每采 N 帧、歇 M 毫秒，把带宽让回给 DSI 扫描。
 *
 * 为什么需要：ISP 路径每帧 DMA 流量翻倍（RGB565 1,843,200B vs RAW8 921,600B），
 * 连续流会把面板的 DW-GDMA/PSRAM 带宽抢死（§20.1 的卡死根因）。
 * RAW8 路径不需要限流（0 = 关闭）。
 */

#ifdef DM_CSI_ISP_RGB565
#  define DM_CAMERA_BURST_FRAMES   12
#  define DM_CAMERA_BURST_PAUSE_MS 80
#else
#  define DM_CAMERA_BURST_FRAMES   0
#  define DM_CAMERA_BURST_PAUSE_MS 0
#endif
#define DM_CAMERA_MOTION_PCT  6

/* 相机预览的目标帧间隔（ms）。主循环每轮都会调当前页的 update()，不节流的话
 * 相机页会以最高频率做"采集+转换+运动检测+整块重绘"，UI 直接卡死（实测）。
 * 0 = 不节流。可用 --cam-fps N 覆盖。
 */

/* 预览节流（ms）。真机实验：66ms(15fps) 时相机页偶发 200ms 刷写尖峰（怀疑与
 * 采集/转换争 PSRAM 带宽有关）；100ms(10fps) 用于验证该假设。--cam-fps 可覆盖。
 */

#define DM_CAMERA_PREVIEW_MS 66

/* 采集线程的跳帧数：每 N 帧只做 1 次软件去马赛克 + 缩放。
 *
 * 依据（JTAG 累计计数器）：`g_dbg_conv_us_max` 峰值 220 ms/帧，
 * `g_dbg_conv_us` 在相机页打开期间几乎线性增长 —— 也就是**每帧都在花整帧的
 * 转换成本，而 UI 只有 66 ms（15 fps）才取一次**。中间那些帧转完就扔了。
 *
 * 跳帧不是"降低画质"，而是"不转换没人要的帧"：预览仍然每次取最新一帧，
 * 只是不再为丢弃的帧付费。1 = 不跳帧。
 */

#ifndef DM_CAMERA_DECIMATE
#  define DM_CAMERA_DECIMATE 2
#endif

/* 相机采集线程的调度优先级（数值越大优先级越高）。
 *
 * **实测结论：不要把它降到主循环之下。**
 *
 * 曾按"相机让路给 UI"的思路设为 90（低于主循环的 100），A/B 结果是**两边都
 * 变差**（自检同一场景，4 秒窗口）：
 *
 *   prio 100（默认，与 UI 同权）：cam 4 fps，ui:responsive max_busy = 70 ms
 *   prio  90（低于 UI）        ：cam 2 fps，ui:responsive max_busy = 219 ms
 *
 * 原因：压住采集线程并不会让它"温柔地"让路 —— 它只是在 poll()/mutex 上等更久，
 * 一旦拿到 CPU 仍要一次性做完 1280x720 → 320x180 的整帧转换，于是尖峰更大、
 * 出帧更稀。真正的解法是把单帧转换本身变便宜（跳帧/去内除法），不是调优先级。
 *
 * 保持默认（与 UI 同权）由 RR 时间片公平轮转。留这个宏是为了让上面的实验可复现。
 */

#ifndef DM_CAMERA_THREAD_PRIORITY
#  define DM_CAMERA_THREAD_PRIORITY 100
#endif
#define DM_CAMERA_PREVIEW_H 180

/* Simulator convenience: write a wifi_sim BSS table before the UI starts, so
 * the wireless page has something to list on a host with no radio.
 */

#ifdef CONFIG_EXAMPLES_DESKMATE_WIFI_BSS
#  define DM_WIFI_BSS CONFIG_EXAMPLES_DESKMATE_WIFI_BSS
#else
#  define DM_WIFI_BSS ""
#endif

/* 默认启动（不带参数直接跑 deskmate）时要主动拉起来的三件事 -----------------
 *
 * 产品定位是"上电就是一个能用的中控屏"，所以三件事默认全开：
 *
 *   DM_CAMERA_AUTOSTART : 打开摄像头，相机页一进去就有画面
 *   DM_AGENT_AUTOSPAWN  : 把内置 ai_agent 起成同进程任务并 ws 连上去，
 *                         顶栏直接显示"已连服务"，对话直接走大模型
 *   DM_WLAN_AUTOSTART   : 把无线口拉起来（ifup + 可选关联 + DHCP）
 *
 * 这三个默认**只作用于交互式启动**：--selftest / --tour / --wifi-probe /
 * --ap-probe / --wifi-connect 这些取证与诊断路径要求可重复、可对比，不能被
 * 后台线程和 1.5 秒的 agent 启动等待搅乱，所以它们仍按各自的显式参数走。
 *
 * 三项都能在命令行关掉：--no-camera / --no-agent-spawn / --no-wlan。
 */

#ifdef CONFIG_EXAMPLES_DESKMATE_CAMERA_AUTOSTART
#  define DM_CAMERA_AUTOSTART 1
#else
#  define DM_CAMERA_AUTOSTART 0
#endif

#ifdef CONFIG_EXAMPLES_DESKMATE_AGENT_AUTOSPAWN
#  define DM_AGENT_AUTOSPAWN 1
#else
#  define DM_AGENT_AUTOSPAWN 0
#endif

/* 没有 ai_agent 的镜像（sim / goldfish 配置）就算 Kconfig 开着也起不来，
 * 直接把默认值归零，免得每次启动都刷一条"spawn 不支持"的告警噪声。
 */

#if !defined(CONFIG_EXAMPLES_AI_AGENT_VELA)
#  undef  DM_AGENT_AUTOSPAWN
#  define DM_AGENT_AUTOSPAWN 0
#endif

#ifdef CONFIG_EXAMPLES_DESKMATE_WLAN_AUTOSTART
#  define DM_WLAN_AUTOSTART 1
#else
#  define DM_WLAN_AUTOSTART 0
#endif

/* 无线 STA 的默认凭据：SSID 留空表示"只把口拉起来并要 DHCP"，不主动关联
 * （ESP-Hosted 的从机固件常常自己已经连上了，应用再重复关联只会打断它）。
 */

#ifdef CONFIG_EXAMPLES_DESKMATE_WLAN_SSID
#  define DM_WLAN_SSID CONFIG_EXAMPLES_DESKMATE_WLAN_SSID
#else
#  define DM_WLAN_SSID ""
#endif

#ifdef CONFIG_EXAMPLES_DESKMATE_WLAN_PSK
#  define DM_WLAN_PSK  CONFIG_EXAMPLES_DESKMATE_WLAN_PSK
#else
#  define DM_WLAN_PSK  ""
#endif

/* Snapshot directory used when no explicit path is given */

#ifndef DM_SNAP_DIR
#  define DM_SNAP_DIR    "/data/deskmate"
#endif

#ifndef DM_SNAP_HOST_DIR
#  define DM_SNAP_HOST_DIR "/host/deskmate"
#endif

/* Geometry helpers -------------------------------------------------------- */

#define DM_STATUS_BAR_H  56
#define DM_FOOTER_H      40

/* Snapshots are limited so a full tmpfs cannot be exhausted by accident.  A
 * 1024x600 RGB565 frame is 1.2 MB, so allow two frames.
 */

#define DM_SNAP_MAX_BYTES (3 * 1024 * 1024)

#endif /* __APPS_EXAMPLES_DESKMATE_DM_CONFIG_H */
