/****************************************************************************
 * apps/examples/deskmate/src/ui/dm_i18n.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * DeskMate UI 的全部面向用户文案（中文）。两条纪律：
 *
 *   1. 页面与控件里**不写**字面量，一律引用这里；中英文切换/改字只动本文件。
 *   2. tools/engineering/gen_dm_font.sh 从本文件（以及页面里的字面量）抽字形，
 *      生成 dm_font_16 / dm_font_24 子集字体——所以新增中文必须落在字面量里。
 *
 * 不翻译的部分（工程标识，脚本/文档按它们断言）：
 *   [dm-check] 键名、DM-READY/DM-EXIT/DM-TOUR DONE/DM-SELFTEST RESULT:、
 *   DM_LOG 诊断行、NSH 命令与参数（deskmate --selftest/--tour/...）。
 ****************************************************************************/

#ifndef __APPS_EXAMPLES_DESKMATE_UI_DM_I18N_H
#define __APPS_EXAMPLES_DESKMATE_UI_DM_I18N_H

/* 品牌与状态栏 */

#define DM_I18N_APP_TITLE       "桌伴"
#define DM_I18N_APP_SUBTITLE    "openvela 智能中控屏"

/* 底部标签栏（2~4 字，1024×600 下 5 等分不挤） */

#define DM_I18N_TAB_HOME        "首页"
#define DM_I18N_TAB_CAMERA      "相机"
#define DM_I18N_TAB_CHAT        "对话"
#define DM_I18N_TAB_NET         "网络"
#define DM_I18N_TAB_SETTINGS    "设置"

/* 首页 */

#define DM_I18N_HOME            "首页"
#define DM_I18N_SYSTEM_CARD     "系统"
#define DM_I18N_BUILD           "版本"
#define DM_I18N_LVGL            "图形库"
#define DM_I18N_DISPLAY         "显示"
#define DM_I18N_BACKEND         "后端"
#define DM_I18N_UPTIME          "运行时长"
#define DM_I18N_SECONDS         "秒"
#define DM_I18N_STATE_CARD      "能力"
#define DM_I18N_CAMERA_STATE    "摄像头"
#define DM_I18N_NET_STATE       "网络"
#define DM_I18N_AGENT_STATE     "智能体"
#define DM_I18N_TOUCH_CARD      "触摸"
#define DM_I18N_TAP_BUTTON      "点我"
#define DM_I18N_TAP_HINT        "点击这里，证明触摸已到达控件"
#define DM_I18N_TAP_COUNT       "次"
#define DM_I18N_NOT_OPENED      "未打开"

/* 智能体状态（dm_agent_state() 返回 off/mock/online/offline，这里给中文名） */

#define DM_I18N_AGENT_OFF       "未启用"
#define DM_I18N_AGENT_MOCK      "离线应答"
#define DM_I18N_AGENT_ONLINE    "已连服务"
#define DM_I18N_AGENT_OFFLINE   "未连服务"

/* 相机页 */

#define DM_I18N_CAMERA_TITLE    "摄像头预览"
#define DM_I18N_CAMERA_SYNTH    "合成画面（本机无摄像头）"
#define DM_I18N_CAMERA_V4L2     "V4L2 摄像头 /dev/video0"
#define DM_I18N_CAMERA_FRAMES   "帧数"
#define DM_I18N_CAMERA_MOTION   "运动量"
#define DM_I18N_CAPTURE         "抓拍"
#define DM_I18N_ASK_VISION      "让智能体看一眼"
#define DM_I18N_CAMERA_ABSENT   "摄像头不可用"
#define DM_I18N_CAMERA_OFF_HINT "摄像头默认关闭（省电、省 PSRAM 带宽）。CSI 与 DSI 并不互斥：板级 bringup 已在 MIPI-DSI 启动前做完 ISP bypass，用 --camera 可以边看画面边显示。"
#define DM_I18N_CAMERA_ASK      "看一下摄像头现在的画面（第 %d 帧，%s 后端，%d×%d），描述你看到了什么。"
#define DM_I18N_MOTION_EVENT    "摄像头检测到画面在动。"

/* 对话页 */

#define DM_I18N_CHAT_TITLE      "对话"
#define DM_I18N_CHAT_HINT       "从下面挑一句，或直接输入"
#define DM_I18N_CHAT_SEND       "发送"
#define DM_I18N_CHAT_CLEAR      "清空"
#define DM_I18N_CHAT_YOU        "我"
#define DM_I18N_CHAT_AGENT      "桌伴"
#define DM_I18N_CHAT_READY      "桌伴已就绪。当前是离线应答后端；连上 ai_agent 服务后会自动转给大模型。"
#define DM_I18N_CHAT_READY_WS   "桌伴已就绪。已连上 ai_agent，消息会转给大模型。"
#define DM_I18N_CHAT_READY_LOCAL "桌伴已就绪。当前是离线应答（未连上 ai_agent）；用 --agent-spawn 或先启动 ai_agent。"
#define DM_I18N_PRESET_TIME     "现在几点了？"
#define DM_I18N_PRESET_NET      "网络现在怎么样？"
#define DM_I18N_PRESET_CAMERA   "你看到什么了？"
#define DM_I18N_PRESET_NOTE     "帮我记一下这件事"

/* 网络页 */

#define DM_I18N_NET_TITLE       "网络"
#define DM_I18N_NET_IFACES      "网络接口"
#define DM_I18N_NET_WIFI        "无线"
#define DM_I18N_NET_SCAN        "扫描"
#define DM_I18N_NET_CONNECT     "连接"
#define DM_I18N_NET_NO_WIFI     "本机没有无线网卡"
#define DM_I18N_NET_NO_AP       "没有扫描到接入点"
#define DM_I18N_NET_NONE        "没有可用接口"
#define DM_I18N_NET_LINKED      "已连接到"
#define DM_I18N_NET_RUNNING     "运行中"
#define DM_I18N_NET_UP          "已启用"
#define DM_I18N_NET_DOWN        "已关闭"
#define DM_I18N_NET_MASK        "掩码"
#define DM_I18N_NET_WIRELESS    "无线"
#define DM_I18N_NET_SIGNAL      "信号"
#define DM_I18N_NET_SCANNING    "扫描中…"
#define DM_I18N_NET_SCAN_FAIL   "扫描失败"
#define DM_I18N_NET_AP_FOUND    "个接入点"
#define DM_I18N_NET_MODE_AUTO   "自动 (DHCP)"
#define DM_I18N_NET_MODE_MANUAL "手动"
#define DM_I18N_NET_ADDR        "地址"
#define DM_I18N_NET_GATEWAY     "网关"
#define DM_I18N_NET_APPLY       "应用"
#define DM_I18N_NET_CANCEL      "取消"
#define DM_I18N_NET_KEEP        "保留设置"
#define DM_I18N_NET_DHCP_WAIT   "正在获取地址…"
#define DM_I18N_NET_DHCP_FAIL   "获取地址失败"
#define DM_I18N_NET_BAD_INPUT   "地址格式不对"
#define DM_I18N_NET_PASSWORD    "密码"
#define DM_I18N_NET_CONNECTING  "连接中…"
#define DM_I18N_NET_CONN_FAIL   "连接失败"
#define DM_I18N_NET_FORGET      "断开"
#define DM_I18N_NET_HOTSPOT     "热点"
#define DM_I18N_NET_AP_UNSUP    "本机从机固件为 STA 版，不支持热点"
#define DM_I18N_NET_ROLLBACK    "5 秒内未确认，已回滚"
#define DM_I18N_NET_STATIC_OK   "已应用静态地址"

/* 扫描/连接失败的具体原因（区别于"空气里没有 AP"） */

#define DM_I18N_NET_DRV_FAIL    "驱动/RPC 失败"
#define DM_I18N_NET_DISC_FAIL   "断开失败"
#define DM_I18N_NET_TRUNCATED   "结果过多已截断"
#define DM_I18N_NET_NO_ASSOC    "未关联（请重试或检查密码）"

/* 设置页 */

#define DM_I18N_SETTINGS_TITLE  "设置"
#define DM_I18N_AGENT_ENDPOINT  "智能体地址"
#define DM_I18N_SELFTEST        "运行自检"
#define DM_I18N_SNAPSHOT        "保存快照"
#define DM_I18N_ANNOUNCE        "主动播报演练"
#define DM_I18N_ABOUT           "关于"
#define DM_I18N_ABOUT_TEXT      "四路能力在一块屏上：摄像头（V4L2）、无线（WAPI/wlan0）、有线（eth0）、触摸（/dev/input0）。"
#define DM_I18N_SETTINGS_ANNOUNCE_TEXT "你让我主动说句话。"
#define DM_I18N_SNAP_FAILED     "快照失败：%d"
#define DM_I18N_SNAP_WRITTEN    "快照已保存：%d 字节 → %s"
#define DM_I18N_QUEUED          "主动消息已入队"

/* 通用 */

#define DM_I18N_YES             "是"
#define DM_I18N_NO              "否"

#endif /* __APPS_EXAMPLES_DESKMATE_UI_DM_I18N_H */
