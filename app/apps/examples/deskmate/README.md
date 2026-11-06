# DeskMate（桌伴）—— openvela 中控屏应用层

一块 7″ 触控中控屏的应用层：把 **摄像头 / WiFi / 网口 / 触摸屏** 四条板载能力收进
一个 LVGL 中控界面，并接上 `ai_agent`（LLM 对话 + 主动事件）。同一套代码可以在
**ESP32-P4 真机**、**NuttX 模拟器（宿主二进制）** 与 **openvela goldfish 官方模拟器**
上运行；模拟器侧是**无头可脚本化**的，一条命令跑完 UI 与逻辑并产出 PNG 证据。

本文件是这个应用的**操作手册**：每个界面上能点的东西、每条命令行参数、每个可供脚本
断言的输出标记、自检的 28 条判据、三套构建路径、Kconfig 开关、配套宿主工具、线程模型，
以及诚实的已知边界。

---

## 目录

| 章节 | 内容 |
|---|---|
| [一、快速开始](#一快速开始) | 最常用的 5 条命令 |
| [二、界面：五个页面与每个操作](#二界面五个页面与每个操作) | 逐页列出按钮/控件及其效果 |
| [三、命令行参数（完整）](#三命令行参数完整) | 默认启动行为 + `deskmate` 的全部选项与退出码 |
| [四、控制台标记（脚本断言的锚点）](#四控制台标记脚本断言的锚点) | `DM-*`、`[dm-check]`、`[dm-wifi-probe]` |
| [五、自检 `--selftest`（28 条判据）](#五自检---selftest28-条判据) | 每条断言证明什么 |
| [六、巡览 `--tour`（9 帧 + GIF）](#六巡览---tour9-帧--gif) | 演示轨，不做断言 |
| [七、构建与运行（三套目标）](#七构建与运行三套目标) | P4 真机 / NuttX sim / goldfish |
| [八、Kconfig 配置项](#八kconfig-配置项) | 21 个开关与默认值 |
| [九、字体与文案](#九字体与文案) | 中文子集字体怎么生成 |
| [十、配套宿主工具](#十配套宿主工具) | 取证/截图/字体/烧录脚本 |
| [十一、已知边界（诚实记录）](#十一已知边界诚实记录) | 踩过的坑与仍未解决的事 |
| [十二、架构与线程模型](#十二架构与线程模型) | 目录结构、辅助线程、agent 协议 |

---

## 一、快速开始

命令都在**作品仓根**（`contest2026_342_ezdezhandui/`）下执行：

```bash
# ① 模拟器一条命令：配置 + 构建 + 无头自检 + 截图 + 断言 + 归档证据
tools/test/sim_deskmate.sh

# ② 复用已有构建（约 8 秒）
tools/test/sim_deskmate.sh --no-build

# ③ 巡览演示：逐页截图 + 拼 GIF（无断言）
tools/test/sim_deskmate.sh --tour

# ④ 真机：烧两镜像 → 起界面 → 把控制台输出断言一遍
tools/build_flash/usb_stable.sh
python3 tools/test/board.py run "deskmate"

# ⑤ 真机/模拟器上直接跑（不需要任何参数）
nsh> deskmate          # 摄像头 + 内置 ai_agent(ws) + wlan0 自动拉起
nsh> deskmate --help   # 看全部参数
```

判定行只有一句：**`RESULT: PASS`**（脚本）与 **`DM-SELFTEST RESULT: PASS (28/28)`**（应用）。
证据默认落在 `/tmp/deskmate-runs/<时间戳>/`，要入库就 `--out logs/sim-deskmate`。

> **直接 `deskmate` 就是产品态**：摄像头打开、内置 `ai_agent` 同进程起来并连上 ws、
> `wlan0` 自己 ifup + DHCP —— 不需要先敲 `wapi`/`ifup`。取证模式
> （`--selftest` / `--tour` / `--wifi-*` / `--ap-probe` / `--wifi-connect`）
> **不套用**这三个默认，保证结果可重复可对比。详见[三、命令行参数](#三命令行参数完整)。

---

## 二、界面：五个页面与每个操作

状态栏（72px）：左边品牌「桌伴」、中间当前页标题、右边智能体状态徽标 + 时钟。
底部标签栏（72px）：`首页 | 相机 | 对话 | 网络 | 设置`。
页面**首次进入时才构建**（`built` 标志），构建后常驻。

### 2.1 首页 `home`

| 控件 | 操作 | 结果 |
|---|---|---|
| 系统卡 · 版本 | — | `deskmate 0.1.0` |
| 系统卡 · 图形库 | — | `lv_version_info()` |
| 系统卡 · 显示 | — | `宽x高 bpp (后端)`，后端是 `fbdev` 或 `mem` |
| 系统卡 · 运行时长 | — | `CLOCK_MONOTONIC` 的系统秒数（开机以来的秒数），每秒跳 |
| 系统卡 · 时钟 | — | `HH:MM:SS`，每秒跳 |
| 能力卡 · 摄像头 | — | `摄像头: v4l2` / `synthetic` / `未打开` |
| 能力卡 · 网络 | — | `网络: <dm_net_describe() 的一行摘要>` |
| 能力卡 · 智能体 | — | `智能体: <off/mock/online/offline> (已发/已收)` |
| 触摸卡 · **点我** | 点一下 | 右侧计数 `N 次` +1；**自检也点这个按钮**来证明触摸链路通 |
| 触摸卡 · 提示 | — | 点我，证明触摸已到达控件 |

> 这一页每个标签都走 `dm_label_set()`（文字没变就不写），避免每帧把整屏标脏；
> 网络摘要由 `dm_ui_tick()` 统一刷新（网络页可见时 2 秒一次，其它页只在开机补一次）。

### 2.2 相机 `camera`

| 控件 | 操作 | 结果 |
|---|---|---|
| 预览画布 | — | 左侧卡片里 320×180 RGB565，**直接指向**相机 HAL 刚发布的缓冲（原地改 `draw_buf->data`，不动尺寸）；默认 66ms 节流 ≈15fps，`--cam-fps` 可改 |
| 右侧信息行 1 | — | `摄像头: <v4l2\|synthetic>` + `帧数 <N>`，**每秒**刷新一次 |
| 右侧信息行 2 | — | `运动量 <N>%` + `<V4L2 摄像头 /dev/video0 \| 合成画面（本机无摄像头）>`，文字变了才写 |
| 内部统计 | — | 5 秒一条 `DM_LOG`：`camera: frames=N cost=N us/frame throttle=N ms` |
| **抓拍** | 点击 | 写 `/tmp/deskmate/capture-<seq>.rgb565`（裸 RGB565，无文件头） |
| **让智能体看一眼** | 点击 | 用帧号/后端/尺寸拼一句 prompt 交给 `dm_agent_send()` |
| 运动事件 | 自动 | 运动量首次 ≥6% 时 `dm_agent_push_proactive("摄像头检测到画面在动。")`，非对话页会弹顶栏提示条 |

摄像头默认**关闭**（省电/省带宽），`--camera` 打开；打不开 `/dev/video0` 时自动退回
**合成动态源**（画面里有移动方块），页面与自检都标注 `synthetic`。摄像头不可用时页面
仍然构建，只显示「摄像头不可用」+ 原因。

### 2.3 对话 `chat`

| 控件 | 操作 | 结果 |
|---|---|---|
| 4 个预设短语 | 点击 | 立即发送：`现在几点了？` / `网络现在怎么样？` / `你看到什么了？` / `帮我记一下这件事` |
| 6 个常用语 | 点击 | `你好` / `今天天气怎么样` / `打开相机` / `记一下明早九点开会` / `现在几点` / `谢谢`——**追加到输入框**（不是直接发），给中文输入法做兜底；本机无拼音词库 |
| 输入框 | 点击 | 弹出 190px 高的软键盘，合成卡（176px 高）整体上移到键盘之上，输入框始终可见 |
| 键盘 OK / 收起 | 点击 | `LV_EVENT_READY` 发送并清空输入框；`LV_EVENT_CANCEL` 只收起键盘并复原合成区位置（以前这两个事件一个都没挂回调，收起键是空操作） |
| **发送** | 点击 | 发送输入框内容并收起键盘 |
| **清空** | 点击 | `lv_obj_clean()` 清空气泡列表，并**立即重建首条提示**（否则 `s_ready_body` 变野指针，在线状态一变就 use-after-free） |
| 首条提示气泡 | 自动 | 只改这一条标签：未连服务 `当前是离线应答（未连上 ai_agent）…`、已连 `已连上 ai_agent，消息会转给大模型`。**不整页重建**（`lv_obj_clean()` 会把用户正在读的历史一起清掉） |

### 2.4 网络 `net`

版面分三块：**左侧 300px 接口列表**、**右上「以太网」卡（eth0，182px 高）**、
**右下「无线」卡（填满剩余高度）**。

左侧接口列表：`名称 已启用/运行中/已关闭` + `IP 掩码`，有地址显示绿色，只读。
**两级节流**——`dm_ui_tick()` 只在网络页可见时每 2 秒做一次 `dm_net_refresh()`
（真机上 `getifaddrs`/WAPI ioctl 可能几百毫秒到几秒），页面自己的 `update()` 再
按 1 秒节流重建这块列表（原来每 tick 都查，把驱动 RPC 刷爆过）。

| 控件 | 操作 | 结果 |
|---|---|---|
| 以太网 · **自动 (DHCP)** | 点击 | 切到 DHCP 模式，地址/掩码/网关三个字段变灰 |
| 以太网 · **手动** | 点击 | 切到手动模式，三个字段可编辑 |
| 以太网 · 地址/掩码/网关 | 点击字段 | 弹数字键盘弹窗，`取消`/`应用` 写回草稿 |
| 以太网 · **应用** | 点击 | DHCP：`dm_net_if_set_dhcp_async("eth0")` 后台取地址，状态行 `正在获取地址…` → `自动 (DHCP) OK` 或 `获取地址失败 (<errno>)`；手动：先校验点分十进制（非法 → `地址格式不对`），成功 → 状态行 `已应用静态地址 <ip>` 并弹 5 秒确认窗 |
| 静态地址确认窗 · **保留设置** | 点击 | 关掉遮罩、停掉回滚定时器，把当前草稿记为「已应用」基准；状态行 `已应用静态地址` |
| 静态地址确认窗 | 超时 5 秒 | 自动关遮罩，把**上一份已应用**的地址/掩码/网关写回，状态行 `5 秒内未确认，已回滚`，并打一条 `net: static config rolled back` |
| 无线 · **扫描** | 点击 | 起 worker 线程异步扫描（UI 不阻塞），列表显示 `SSID + RSSI + 是否加密`；扫描中状态行 `扫描中…`；驱动侧失败显示 `扫描失败 (errno)`；AP 多时列表竖向可滚 |
| 无线 · AP 列表 · **连接** | 点击 | 弹密码输入框（`取消`/`应用`）→ `dm_net_wifi_connect()`。驱动接受只是「请求已收下」，状态行先 `连接中…`；之后每秒查一次关联，成功 → `已连接到`，15 秒没关联上 → `未关联（请重试或检查密码）`；驱动直接拒绝 → `连接失败 (errno=N)` |
| 无线 · **断开** | 点击 | `dm_net_wifi_disconnect()`：成功状态行 `已关闭`，失败 `断开失败 (errno=N)` —— 返回值以前被 `(void)` 丢掉，界面上什么都看不到 |
| 无线 · **热点** | 点击 | 显示 `本机从机固件为 STA 版，不支持热点`（如实降级，不假装能开） |

> 网络状态由 `dm_ui_tick()` 统一刷新并节流（网络页可见时 2 秒一次、其它页只在开机补一次），
> 页面只读缓存。**关联状态**是例外：点「连接」后页面自己每秒查一次（最多 15 秒），
> 因为 `SIOCGIWAP` 会走 CP 的 RPC（超时 3 秒），再快就会把 UI 拖停。

### 2.5 设置 `settings`

| 控件 | 操作 | 结果 |
|---|---|---|
| 智能体地址 | — | `智能体地址: <off/mock/online/offline> (chat id "deskmate-ui")` |
| **保存快照** | 点击 | 先写 `/host/deskmate/dm_settings.bin`（模拟器 hostfs），失败退回 `/tmp/deskmate/dm_settings.bin`；结果写进状态行 + `DM_LOG` |
| **主动播报演练** | 点击 | `dm_agent_push_proactive("你让我主动说句话。")`，非对话页会弹提示条 → `主动消息已入队` |
| 关于 | — | 三行：`deskmate 0.1.0` / `显示: 1024x600 16bpp mem` / `图形库: <lv_version_info()>`，下面一行四路能力说明（摄像头 V4L2、无线 WAPI/wlan0、有线 eth0、触摸 `/dev/input0`） |

---

## 三、命令行参数（完整）

`deskmate` 是 **NSH 内置命令**（`CONFIG_NSH_BUILTIN_APPS=y`，程序名由
`CONFIG_EXAMPLES_DESKMATE_PROGNAME` 决定，默认 `deskmate`）。`--help` / `-h` 打印下面这份
分类用法；源码里 `g_usage` 与本节一一对应。

### 3.0 默认启动行为（`deskmate` 不带参数）

产品定位是「上电就是一块能用的中控屏」，所以**交互式启动默认把三件事拉起来**：

| 默认动作 | 做什么 | 关掉 |
|---|---|---|
| **打开摄像头** | `dm_camera_create()`，相机页一进去就有画面（打不开自动退合成源） | `--no-camera` 或 `..._CAMERA_AUTOSTART=n` |
| **同进程起 ai_agent 并连 ws** | `task_create("ai_agent", …)` → 等 1.5 秒监听 → 桥以 `ws` 连 `127.0.0.1:28789`，顶栏显示「已连服务」，对话直接走大模型 | `--no-agent-spawn` 或 `..._AGENT_AUTOSPAWN=n` |
| **拉起 wlan0** | `ifup` + 可选关联 + 异步 DHCP；配置了 `..._WLAN_SSID` 就先关联，留空则只 ifup+DHCP（不动从机固件已经建好的链路） | `--no-wlan` 或 `..._WLAN_AUTOSTART=n` |

- **`--no-*` 是否决票**：和 `--camera` / `--agent-spawn` 一起给时以关为准。
- **取证模式不套用这些默认**：`--selftest`、`--tour`、`--wifi-probe`、`--ap-probe`、
  `--wifi-connect` 走各自的显式参数。理由很实际——自检要可重复、可对比，不能被
  后台采集线程、Agent 的 1.5 秒启动等待、以及抢地址的 DHCP 搅乱。
- 启动第一行日志会自报决策：
  `[dm] startup: camera=1 agent_spawn=1 wlan=1 (evidence_mode=0)`
- 没有 ai_agent 的镜像（sim / goldfish 配置）会把 `AGENT_AUTOSPAWN` 在编译期强制归零，
  所以不会每次启动都刷一条"spawn 不支持"的告警。

### 3.1 运行模式

| 参数 | 说明 | 退出 |
|---|---|---|
| *(无参数)* | 起 UI，一直跑到被杀 | 0 |
| `--selftest` | 无头自检：断言 + 每页快照 + 报告，跑完自己退出 | 全部通过 0，否则 1 |
| `--tour` | 巡览：9 步走完 5 页（相机页连拍 5 帧），每步一张快照，无断言，走完自己退出 | 0 |
| `--soak <sec>` | 每 `<sec>` 秒切一次页，每 30 秒把 `t/page/commits/cam_frames/loops/max_busy` 追加到 `/tmp/deskmate_soak.txt`。**自己不会退出**，要配 `--exit-after` 或外部 kill | 由 `--exit-after` 决定 |
| `--exit-after <sec>` | UI 循环跑满 `<sec>` 秒后退出（`CONFIG_EXAMPLES_DESKMATE_EXIT_AFTER` 的运行时版本） | 0 |
| `--page <id\|index>` | 起始页：`home`/`camera`/`chat`/`net`/`settings` 或 `0`..`4`；未知值只告警、留在首页 | 0 |

### 3.2 证据相关

| 参数 | 说明 |
|---|---|
| `--snap <path>` | 指定快照**目录**（文件名部分只用于推导目录）：每页写成 `<dir>/dm_home.bin`、`dm_camera.bin`、`dm_net.bin`，各带一个 `.txt` 旁注（`width/height/bpp/stride/backend`）。默认 `/host/deskmate/dm_selftest.bin`（hostfs 已挂）或 `/tmp/deskmate/…` |
| `--report <path>` | 把 `[dm-check]` 行**同时**写一份到文件（板上串口常被驱动日志淹没，报告文件才是可靠证据） |
| `--hostfs <dir>` | 把宿主目录挂到 `/host`（仅 `CONFIG_FS_HOSTFS=y` 的模拟器），同时创建 `/host/deskmate` |

### 3.3 智能体桥

| 参数 | 说明 |
|---|---|
| `--agent <mock\|ws\|off>` | 覆盖 `CONFIG_EXAMPLES_DESKMATE_AGENT_MODE`。`mock` 用设备真实状态离线应答；`ws` 连 `ai_agent`；`off` 关闭 |
| `--agent-endpoint <host[:port]>` | ws 后端地址，默认 `127.0.0.1:28789` |
| `--agent-spawn` | 把内置 `ai_agent` 起成**同进程任务**再连 `127.0.0.1`；隐含 `--agent ws`。**需要镜像里有 ai_agent**（`CONFIG_EXAMPLES_AI_AGENT_VELA=y`）+ 回环；缺任何一项只告警，UI 照常起 |
| `--say <text>` | 连上后发一条消息并打印回复（`DM-PROBE state/send/recv`），证明 `桥 → 后端 → UI` 整条链是通的 |

### 3.4 相机

| 参数 | 说明 |
|---|---|
| `--camera` | 打开 `/dev/video0`（CSI 1280×720 RAW8 → CPU 转 RGB565 预览）。**交互式启动本来就默认打开**（见 3.0），这个参数只在配了 `..._CAMERA_AUTOSTART=n` 或取证模式下需要显式给 |
| `--cam-fps <n>` | 预览节流到 `<n>` fps（`0` = 不限速） |
| `--freeze-preview` | 停止把画布指向新帧（诊断：验证"每帧换图源是否触发整屏重绘"） |

### 3.5 网络

| 参数 | 说明 | 输出 |
|---|---|---|
| `--wifi-scan` | 用应用自己的 WAPI 通道扫 wlan0 并打印 AP，然后退出 | `DM-SCAN count=N` / `DM-SCAN apK ssid=… rssi=… freq=… secure=…`；驱动失败 `DM-SCAN err=<errno>`，退出码 1 |
| `--wifi-probe` | WiFi 诊断报告（接口、关联、断开、扫描、AP 列表），**不开 UI 直接退出** | `[dm-wifi-probe] …` 多行 + `DM-WIFI-PROBE DONE`；恒返回 0 |
| `--wifi-connect <ssid> [<psk>]` | 关联指定 AP（`dm_net_wifi_wait_link` 最多等 20 秒），成功后立刻在 wlan0 上做 DHCP，**不开 UI 直接退出** | `DM-WIFI-CONNECT ssid='…' rc=…`、`DM-WIFI-LINK ok ssid='…' rssi=…` 或 `DM-WIFI-LINK timeout`、`DM-WIFI-IP <ip>` / `dhcp-failed`；**只有真的拿到链路才返回 0**，否则 1 |
| `--ap-probe` | 走一次 `dm_net_ap_probe()`（AP 模式 ioctl 再恢复），**不开 UI 直接退出** | `DM-AP-PROBE rc=…`；rc 为 0 才返回 0 |

### 3.6 显示 / 触摸 / 硬件诊断

| 参数 | 说明 | 需要 |
|---|---|---|
| `--disp-stall <sec>` | 在 UI 建起来**之前**故意睡两段 `sleep(<sec>+5)` + `sleep(<sec>)`，让启动过程停摆约 `2*<sec>+5` 秒，复现「显示被打停」让看门狗报出并自救 | 无 |
| `--disp-reinit` | 显示起来之后再重跑一次面板/DSI bring-up（显示自愈测试） | P4 |
| `--touch-fake x y` | 把一次"合成按下"直接写进触摸**硬件快照**（跑通 `快照 → indev → UI`） | 无 |
| `--touch-wr x y` | 把一次真实按下写进 GT911 寄存器（端到端） | P4 |
| `--wifi-start` | 试一次 hosted RPC `wifi_start`（5s 有界，防挂死），然后扫一次 | P4 |
| `--ppa-test` | 跑 PPA（2D 加速器）填充自检后退出 | `CONFIG_ESPRESSIF_PPA` |

### 3.7 关闭默认值

| 参数 | 说明 |
|---|---|
| `--no-camera` | 启动时不打开摄像头（等同 `--camera` 的相反数；与相机页/自检无关——相机页仍会按需创建） |
| `--no-agent-spawn` | 启动时不起内置 `ai_agent`；桥保持 `--agent` 或 Kconfig 指定的模式（`mock`/`ws`/`off`） |
| `--no-wlan` | 启动时不 ifup / 不 DHCP；网络页仍可手动点「扫描」「连接」 |

> 这三个是**否决票**：即使同时给了 `--camera` / `--agent-spawn`，也以关为准。
> 典型用法是省电、或在已经手工配好网络的现场做对照实验。

### 3.8 启动时的无线行为（`--no-wlan` 之外）

不带参数启动且 `..._WLAN_AUTOSTART=y` 时，`dm_wlan_autostart()` 依次做：

1. `dm_net_refresh()` → 找到第一个 `wireless` 接口（**不写死 `wlan0`**）
2. `dm_net_if_set_up(ifname, true)`
3. 配了 `..._WLAN_SSID`：**先查是否已关联**（`dm_net_wifi_link`），已关联就不动；
   否则 `dm_net_wifi_connect()` 并最多等 20 秒
   没配 SSID：扫一次（只留证据，网络页也要用），不主动关联
4. `dm_net_if_set_dhcp_async(ifname)` —— 后台线程取地址，不阻塞 UI

没有无线口时只写一行 `wlan: no wireless interface, autostart skipped` 就继续，
不会拦住启动。

### 3.9 行为细节（都是踩过的坑）

- **未知参数不致命**：打印 `ignoring unknown argument 'xxx'` 后继续，不会退出。所以
  `--wifi-start` 在模拟器上会先落后被忽略的告警，而不是报错退出。
- **`--touch-wr` 在非 P4 上匹配但不吃参数**：紧跟的两个数字会各自再报一次 unknown。
- **`--say` 只吃一个 token**；带空格要引号（NSH 支持 `"hello world"`）。
- **NSH 不会把非 ASCII 送进 argv**：`--say "现在几点了"` 到应用里只剩空/ASCII 片段。
  要验中文对话，请在**界面上**发（预设短语/输入框），或直接用 `tools/test/ws_demo.py`。
- **同一 session 里只能起一次**：第二次会打印
  `LVGL is already initialised; refusing to run twice` 并退出（LVGL 全局态无法安全重建）。
- **快照上限** 3 MB（`DM_SNAP_MAX_BYTES`），一张 1024×600 RGB565 = 1,228,800 B。

---

## 四、控制台标记（脚本断言的锚点）

所有标记都是**工程标识**，不做中英翻译（`dm_i18n.h` 里明确列了这条纪律）。

| 标记 | 何时出现 | 用途 |
|---|---|---|
| `DM-READY display=<后端> <W>x<H> bpp=<n> lvgl= agent=<状态> camera=<后端> page=<id>` | UI 起来后一次 | 判"界面到底有没有起来"；脚本缺这一行直接 FAIL |
| `[dm-check] <name> PASS/FAIL <detail>` | 自检每条判据一行 | 逐项证据；脚本统计 PASS/FAIL 计数 |
| `DM-SELFTEST RESULT: PASS (n/n)` / `FAIL (m/n failed)` | 自检收尾一行 | 唯一的判定行 |
| `DM-EXIT <selftest\|run> result=<n>` | 进程退出前 | 区分是自检还是常跑退出 |
| `DM-TOUR DONE steps=9` | 巡览收尾 | `--tour` 的完成标记 |
| `DM-SCAN count=…` / `DM-SCAN err=…` / `DM-SCAN apK …` | `--wifi-scan` | 扫描证据 |
| `DM-WIFI-PROBE DONE`、`[dm-wifi-probe] …` | `--wifi-probe` | WiFi 诊断报告 |
| `DM-WIFI-CONNECT rc=`、`DM-WIFI-LINK ok\|timeout`、`DM-WIFI-IP …` | `--wifi-connect` | 连接证据 |
| `DM-WIFI-START rc=`、`DM-WIFI-START scan=…` | `--wifi-start` | hosted RPC 结论 |
| `DM-AP-PROBE rc=` | `--ap-probe` | 热点能力结论 |
| `DM-PROBE state=`、`DM-PROBE send rc=`、`DM-PROBE recv: …` | `--say` | 对话链路证据 |
| `[dm] …` | `DM_LOG`（**按调用点每秒最多一行**，被丢的条数附在下一行） | 诊断流水；限速是为了别把 115200 控制台灌爆后冻住 UI |
| `[dm][warn] …` / `[dm][err] …` | 告警/错误（`err` 同时进 syslog） | 故障定位 |

---

## 五、自检 `--selftest`（28 条判据）

自检是一个**跑在 UI 线程上的状态机**：每轮主循环推进一步，所以不需要任何锁；点击通过
脚本 indev 注入，走的是和真人一样的事件路径。全部通过时最后一行是
`DM-SELFTEST RESULT: PASS (28/28)`。

| # | 判据 | 证明什么 |
|---|---|---|
| 1 | `display:ready` | 显示后端就绪，且给出分辨率/位深/后端名 |
| 2 | `lvgl:version` | LVGL 主版本是 9（界面按 9 写的） |
| 3 | `ui:built` | 控件树建成：控件数 ≥3 且注册页数 ≥4（实际 5 页；下限断言是为了将来删页时不误报） |
| 4 | `ui:home` | 起始页就是首页（`dm_ui_current() == 0`） |
| 5 | `font:cjk-body` | **正文档**覆盖：把全部界面文案串起来，逐个字符问 LVGL 有没有轮廓（缺一个就 FAIL 并报出首个缺字） |
| 6 | `font:cjk-title` | **标题档**覆盖：品牌名、5 个 Tab、数字与冒号 |
| 7 | `font:chat-input` | **输入框实际解析到的字体**（`lv_obj_get_style_text_font`）覆盖中文——这一条专抓"控件忘了设字体、回退到无 CJK 的默认字体" |
| 8 | `touch:target` | 向 UI 要到首页「点我」按钮**真实屏幕坐标**（x>0 且 y>0），随后按这个坐标注入一次点击 |
| 9 | `touch:delivered` | 几帧后按钮计数必须比点击前大（0→1）——证明注入走完 `indev → 事件 → 回调 → 标签更新` 整条链，不是画给眼睛看的静态图 |
| 10 | `snapshot:written` | 首页快照写入成功，字节数 == 几何（1,228,800） |
| 11 | `ui:tab-target` | 向 UI 要到「相机」Tab 的真实坐标，并按它注入点击 |
| 12 | `ui:camera-page` | **点 Tab 真的切到相机页** |
| 13 | `ui:page-rendered` | 首页与相机页的**帧指纹不同**（`dm_display_frame_hash()`）：画面真的变了，不是空转 |
| 14 | `snapshot:written` | 相机页快照 |
| 15 | `ui:net-page` | 再点一次 Tab 切到网络页（判 `dm_ui_current_id()=="net"`），detail 带上接口摘要 |
| 16 | `agent:bridge` | 桥处于五种合法状态之一（`off`/`mock`/`online`/`offline`/`ws`），并给出已发/已收计数 |
| 17 | `net:interfaces` | 接口列表可读（真机 eth0，模拟器 wlan0） |
| 18 | `net:ipv4-valid` | 点分十进制校验：接受 3 个合法地址、拒绝 4 个非法 |
| 19 | `net:eth-static` | 静态地址**真的写回驱动并读回**：写 `192.168.77.9/255.255.255.0`，读回必须一模一样。接口名不写死（优先 `eth*`，没有就用唯一的无线口） |
| 20 | `net:ap-degraded` | 热点如实降级：`dm_net_ap_start("DeskMate-AP", …, 6)` 必须返回 `-ENOTSUP` 且 `dm_net_ap_supported()==false`（从机是 STA 固件） |
| 21 | `snapshot:written` | 网络页快照 |
| 22 | `wifi:scan` | WAPI 扫描：有 AP 就断言第一个的 SSID 非空（模拟器读 `/host/wifi/bss`，板上问 ESP-Hosted）；0 个 AP 时把**驱动 errno** 一并写进证据（`no AP in range` vs `driver scan unavailable`），两种情况都要求界面正确降级，因此都算通过 |
| 23 | `snapshot:picture` | 画面是"画出来的 UI"而不是黑屏/花屏：抽样点数 >0、色数 ≥3、主色 ≤90%、平均亮度梯度 ≤8（与 `fb2png.py` 同一套判据、同一段统计代码） |
| 24 | `csi:dsi-coexist` | 相机出帧的同时**面板**还在被喂数据：优先用 DSI DMA 完成计数（无法伪造），平台没有该信息源时如实标注并只断言相机出帧 |
| 25 | `ui:responsive` | 4 秒窗口内**最大单轮忙时 ≤500ms** 且循环 ≥10 次（抓秒级卡顿；不把 GT9xx 单次 I2C 传输实测可达 419ms 的固有延迟算成应用缺陷） |
| 26 | `display:fed` | 有相机时 `dm_display_commits()` 在窗口内必须涨 ≥5 次；没有相机（纯静态界面，LVGL 本来就不重绘）如实跳过并在 detail 里写明 |
| 27 | `cam:fps` | 预览帧率 ≥4fps（三合一场景实测 6~14fps；门槛用来抓"相机死了/只剩 1~2fps"） |
| 28 | `cam:teardown` | 先切回首页再 `dm_camera_destroy()` → 200ms → `dm_camera_create()`，要求 `rc==0`（V4L2 close 真把 CSI 停了且还能再起来）；没相机则跳过 |

**没有相机 / 没有无线时**：`csi:dsi-coexist`、`cam:fps`、`cam:teardown` 会带
`(skipped)` 或 `(run with --camera …)` 通过，并在 detail 里说明跳过原因——不假装验证过。

自检产出的快照目录（默认 `/host/deskmate`，无 hostfs 时 `/tmp/deskmate`）：

```
dm_home.bin  dm_home.bin.txt
dm_camera.bin dm_camera.bin.txt
dm_net.bin   dm_net.bin.txt
```

> 三个 `.bin` 是裸 RGB565（无文件头），用 `tools/test/fb2png.py <bin> <png> 1024 600` 转 PNG。
> `.bin.txt` 里写 `width/height/bpp/stride/backend`，脚本按它取几何，不写死。

---

## 六、巡览 `--tour`（9 帧 + GIF）

自检是"证明它对"，巡览是"让人看见它"。9 步固定脚本，不做断言：

| 步 | 页面 | 快照 |
|---|---|---|
| 1 | home | `tour_01_home` |
| 2–6 | camera（连拍 5 帧） | `tour_02_camera_a` … `tour_06_camera_e` |
| 7 | net | `tour_07_net` |
| 8 | chat（巡览开始时就先问一句 `What time is it, and how is the network?`，让气泡有内容） | `tour_08_chat` |
| 9 | settings | `tour_09_settings` |

```bash
tools/test/sim_deskmate.sh --tour     # 一条命令：跑 → 9 张 PNG → deskmate_tour.gif
```

宿主侧由 `tools/test/dm_tour_gif.py` 拼 GIF：页面停留 950ms、对话页 1100ms、相机连拍 250ms，
让"切页"和"画面在动"都看得出来。

---

## 七、构建与运行（三套目标）

三套目标共用同一份源码，差异只在 Kconfig 选的后端（`fbdev` vs `mem`）与设备节点。

### 7.1 真机：ESP32-P4X-C5-Function-EV-Board

板级配置已包含本应用（`CONFIG_EXAMPLES_DESKMATE=y`）：

```bash
cd <openvela 工作区根>
./build.sh vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/configs/nsh --cmake -j8
cd contest2026_342_ezdezhandui && tools/build_flash/usb_stable.sh    # 两镜像烧录（MCUboot）
python3 tools/test/board.py reset                                    # 复位 → nsh>
python3 tools/test/board.py run "deskmate --help"
```

- 显示：`/dev/fb0` 1024×600 RGB565（MIPI-DSI EK79007）
- 触摸：`/dev/input0`（GT911，独立低优先级线程轮询，UI 线程不碰 I2C）
- 摄像头：`/dev/video0`（SC2336 → MIPI-CSI → V4L2，默认关，`--camera` 开）
- 网络：`eth0`（esp_emac）+ `wlan0`（ESP-Hosted，WAPI）
- `/tmp` 是 tmpfs（板级 bringup 挂 `CONFIG_LIBC_TMPDIR`），无 hostfs

> ⚠️ 镜像余量极小：应用槽 `CONFIG_ESPRESSIF_OTA_SLOT_SIZE=0x1C0000`（1,835,008 B），
> 当前 `nuttx.bin` 为 1,830,912 B —— **只剩 4 KB**。加功能前先想清楚要从哪儿省。
>
> 💡 全新工作树第一次构建时，CMake 会把 MCUboot 的 **git 子模组**拉到
> `nuttx/bootloader-esp32p4/mcuboot/`（含 mbedtls 等，200 MB 以上）。这一步失败会看到
> `Failed to update submodules in: .../mcuboot`，但 `deskmate` 本身没任何编译错误——
> 重跑一次 `build.sh` 即可（已下载的部分会复用）；纯离线环境请先手工
> `git submodule update --init --recursive`。

### 7.2 NuttX 模拟器（宿主二进制，无头）

```bash
tools/test/sim_deskmate.sh                 # 构建 + 运行 + 断言 + 归档（冷构建数分钟 / 复用构建约 8 秒）
tools/test/sim_deskmate.sh --no-build      # 复用已有构建
tools/test/sim_deskmate.sh --tour          # 巡览 + GIF
tools/test/sim_deskmate.sh --keep-running   # 不设超时（调试用）
tools/test/sim_deskmate.sh --out logs/sim-deskmate   # 证据入库
```

配置在 `nuttx/boards/sim/sim/sim/configs/deskmate/defconfig`，要点：

- **无 X11**：显示走**内存后端**（应用自持 1024×600 RGB565，快照即真相）
- **触摸**：`SIM_TOUCHSCREEN` 需要 X11，所以默认用**脚本注入**（`dm_touch_tap`），
  自检点击按钮也走这条路；`/dev/input0` 不存在时会打一条 err（可忽略）
- **网口**：`SIM_NETDEV` + `SIM_NETDEV_TAP`（宿主 TAP 设备，需 root 或提前 `tuntap`）
- **WiFi**：`DRIVERS_WIFI_SIM` + `SIM_WIFIDEV_PSEUDO`，wlan0 由驱动提供，
  BSS 表由应用启动时写 `/host/wifi/bss`（走 hostfs，因为 guest 根文件系统不可写）；
  扫描结果就是这张表里的 3 个隐藏 AP（`<hidden>` / -42 dBm 等）
- **快照**：经 `FS_HOSTFS` 落宿主目录，再用 `tools/test/fb2png.py` 转 PNG
- **相机**：宿主没有 `/dev/video0` → 合成源（`synthetic`）

产物在 `<out>/<时间戳>/`：`console.log`、`build.log`、`dm_home/camera/net.png`、对应
`.bin` + `.bin.txt`、`fb2png.log`（色数/主色/梯度）、`result.txt`（判定摘要）。
退出码：0 = `RESULT: PASS`。

脚本本身还做了两件宿主适配：`libunwind.so` 开发符号缺失时用私有 `LIBRARY_PATH` shim；
清掉别的配置在 `apps/builtin` 下留下的陈旧生成头（否则会链接到本配置没编的应用）。

### 7.3 goldfish 官方模拟器（验收轨）

板级配置 `board/vela/configs/goldfish-arm64-v8a-ap/defconfig`（manifest 映射到
`vendor/openvela/boards/vela/`），goldfish camera / GPU fb / goldfish events /
virtio-net / wifi_sim 都是现成的，`/tmp`、`/data` 由 `rc.sysinit` 挂成 tmpfs：

```bash
cd <openvela 工作区根>
./build.sh vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap/ --cmake -j8

# emulator.sh 从 nuttx/ 下找镜像，先软链过去（缺了会 "No initial vela_system image"）
ln -sf ../cmake_out/vela_goldfish-arm64-v8a-ap/nuttx           nuttx/nuttx
ln -sf ../cmake_out/vela_goldfish-arm64-v8a-ap/vela_system.bin nuttx/vela_system.bin
ln -sf ../cmake_out/vela_goldfish-arm64-v8a-ap/vela_data.bin   nuttx/vela_data.bin

# 无窗口运行，并把命令从管道喂进去（直接交互会因为 stdin 不是 tty 拿不到 shell）
(sleep 40; printf "deskmate --selftest\n"; sleep 90; printf "quit\n") \
  | ./emulator.sh vela -no-window -no-audio -gpu off -qemu -nographic > /tmp/emu.log 2>&1
grep -q "DM-SELFTEST RESULT: PASS" /tmp/emu.log && echo OK
```

> 这条轨**没有**一键脚本：仓库里不放 `tools/test/emulator_deskmate.sh`（早期文档引用过它，
> 但该脚本从未入库），闭环要靠上面的「管道喂 stdin + grep 判定行」。
>
> 该配置的 agent 模式是 `mock`，镜像里也没有 ai_agent，所以 `--agent-spawn` 在这是
> 明确的"不支持"告警。
>
> 快照落在镜像里的 tmpfs（`/tmp/deskmate`），**取不出来**转 PNG。要在 goldfish 上拿到
> 图像证据，只能截屏（`-no-window` 下没有窗口），或者换 NuttX sim 那条轨（`--hostfs`）。

---

## 八、Kconfig 配置项

`menuconfig EXAMPLES_DESKMATE`（`depends on GRAPHICS_LVGL`，tristate，默认 n），
下设 20 个子符号，共 21 项：

| 符号 | 默认 | 说明 |
|---|---|---|
| `EXAMPLES_DESKMATE` | n | 总开关 |
| `..._PROGNAME` | `"deskmate"` | NSH 命令名 |
| `..._PRIORITY` | 100 | 任务优先级 |
| `..._STACKSIZE` | 32768 | 主任务栈（sim 用 65536；`--agent-spawn` 另起 24 KB 任务） |
| `..._DISPLAY_FBDEV` / `..._DISPLAY_MEM` | choice，`VIDEO_FB=y` 时默认 FBDEV，否则 MEM | 显示后端二选一：fbdev `/dev/fb0` / 应用自持内存 |
| `..._WIDTH` / `..._HEIGHT` | 1024 / 600 | 内存后端几何 |
| `..._FB_PATH` | `"/dev/fb0"` | 仅 fbdev 后端 |
| `..._INPUT_DEVPATH` | `"/dev/input0"` | 触摸设备（`depends on INPUT_TOUCHSCREEN`） |
| `..._HOSTFS_PATH` | `""` | 非空时启动即把该宿主目录挂到 `/host`（模拟器用） |
| `..._EXIT_AFTER` | 0 | 开机自动退出秒数（`--exit-after` 的配置版） |
| `..._FONT_SIMSUN` | n | 退回 LVGL 内置 SimSun 16（约 +150 KB） |
| `..._AGENT_MODE` | `"ws"` | `mock` / `ws` / `off` |
| `..._WIFI_BSS` | `""` | 启动时写 wifi_sim 的 BSS 表（如 `/host/wifi/bss`） |
| `..._CAMERA_AUTOSTART` | **y** | 交互式启动就打开摄像头（`--no-camera` 或这里关） |
| `..._AGENT_AUTOSPAWN` | **y** | 交互式启动就起内置 ai_agent 并连 ws；`depends on EXAMPLES_AI_AGENT_VELA`，没有 ai_agent 的镜像由 `dm_config.h` 强制归零 |
| `..._WLAN_AUTOSTART` | **y** | 交互式启动就 ifup + DHCP（`--no-wlan` 或这里关） |
| `..._WLAN_SSID` | `""` | 非空则启动时先关联该 AP；留空只 ifup+DHCP（不动从机已建的链路） |
| `..._WLAN_PSK` | `""` | 配合 `_WLAN_SSID`；开放网络留空 |
| `..._CAMERA_DEVPATH` | `"/dev/video0"` | V4L2 设备；打不开自动退合成源 |

> `ai_agent` 的开关不在这里：`--agent-spawn` 依赖 `CONFIG_EXAMPLES_AI_AGENT_VELA=y`
> （P4 的 `nsh` 配置有，sim / goldfish 都没有）。

---

## 九、字体与文案

- **文案全部集中在 `src/ui/dm_i18n.h`**：页面与控件里不写中文字面量。改文案只动这一个文件。
- **字体是自绘子集**：当前只编 `dm_font_14`（14px / **1bpp**）——ASCII 0x20-0x7E 全量
  + 出现在字符串字面量里的非 ASCII 字形 + **GB2312 一级 3755 汉字**，共 3867 个字形。
  C 源 950 KB，编译后 `.rodata` 实测 **109 KB**（P4 构建产物）。
- 选它是因为镜像余量：原来是两档——`dm_font_32` 做标题、`dm_font_20` 做正文/小字
  （共 417 KB rodata）。合成一档后只占 109 KB，**省下约 308 KB**，正好是镜像能装下的
  前提（当前镜像 1,830,912 B / 槽 1,835,008 B）。
- **代价**：`DM_FONT_TITLE` / `DM_FONT_BODY` / `DM_FONT_SMALL` 指向同一支字体，
  标题与正文没有字号层次。
- `fonts/` 下还留着 `dm_font_16/20/24/32.c`（历史档位，**不参与编译**，编译产物里
  只能找到 `dm_font_14.c.o`）。想换档位：改 `dm_theme.h` 的 `DM_FONT_*`，再在
  `Makefile` 的 `CSRCS` 与 `CMakeLists.txt` 的 `SRCS` 里把对应 `.c` 加进去
  ——**两处都要改**，否则 Yocto/CMake 与纯 Make 两条构建路径会分叉。
- 生成：

  ```bash
  tools/engineering/gen_dm_font.sh --check          # 只清点字形与体积，不生成
  tools/engineering/gen_dm_font.sh --sizes 14       # 生成 14px（脚本默认 16 24 / 4bpp）
  ```

  脚本扫描 `deskmate_main.c` + `src/**/*.{c,h}` 里的全部字符串字面量，并给正文档并入
  GB2312 一级汉字（运行时文本如 LLM 回复、SSID、用户输入不在字面量里，不并会出现方框）。
  需 `node`+`npx`（首次拉 `lv_font_conv`）；离线回退是开 `..._FONT_SIMSUN=y`。
  注意脚本**没有 `--bpp` 开关**（`BPP=4` 写在第 31 行），生成 1bpp 档要改那一行。

---

## 十、配套宿主工具

都在仓库根的 `tools/` 下（索引见 `tools/README.md`）。

| 工具 | 用途 |
|---|---|
| `tools/test/sim_deskmate.sh` | 模拟器一键闭环（本文档最常用） |
| `tools/test/board.py` | 真机串口 harness：`reset` / `run "<cmd>"` / `script <file>` / `watch --timeout N` / `login`；`--expect <regex>` 断言、`--save` 存日志、`--no-idle` 抓满时长。console 在 `/dev/ttyUSB0`，**必须 assert DTR/RTS** |
| `tools/test/fb2png.py` | 裸 RGB565 → PNG，并打印色数/主色/梯度：`fb2png.py <bin> <png> [w=1024] [h=600]` |
| `tools/test/dm_tour_gif.py` | 巡览 PNG → GIF（`dm_tour_gif.py <run_dir> [--width 640] [--out …]`） |
| `tools/test/hw_deskmate_evidence.sh` | 真机 UI 取证：串口找 fb 地址 → OpenOCD dump → PNG + 同一套判据（`--addr` 可指定） |
| `tools/test/hw_var_read.sh` | 串口被日志占满时，用 JTAG 按符号名读应用全局变量（地址从 ELF 取）：`hw_var_read.sh g_scan_state g_scan_count` |
| `tools/test/evidence.py` | 一次真机取证（复位 + 串口 + 可选逻辑分析仪），产物落 `logs/<ts>_<tag>/` |
| `tools/test/ws_demo.py` | 直连板端 28789 发消息收 LLM 回答：`--host <ip> --port 28789 --send "…" --wait 60`（可多次 `--send`） |
| `tools/test/camera_diag.sh` | 摄像头链路一次性判定（传感器 → MIPI-CSI → 桥 → DW-GDMA） |
| `tools/test/camera_verify.sh` | 摄像头驱动一键复现（编译→烧录→枚举→抓帧→判定） |
| `tools/test/verify_aiagent.sh` | ai_agent 启动里程碑闭环断言 |
| `tools/test/ocd.py` | OpenOCD telnet（4444）现场：`pc` / `halt` / `resume` / `regs`。GDB 连 P4 会 `Target disconnected`，telnet 口才是稳的 |
| `tools/build_flash/usb_stable.sh` | MCUboot 两镜像稳定烧录（`0x2000` 引导 + `0x20000` 应用，双 hash 校验） |
| `tools/engineering/gen_dm_font.sh` | 生成中文子集字体 |
| `tools/engineering/export.py` | 把工作区改动导出到作品仓文件树（默认 dry-run） |

真机取证链路：**串口/JTAG 取帧 → `fb2png.py` → PNG + 判据**；模拟器是
**应用自写快照 → `fb2png.py` → PNG + 判据**。同一条判据、同一段代码，只是取帧手段不同。

---

## 十一、已知边界（诚实记录）

### 平台侧（不在应用层范围内）

- **WiFi**：应用层通过 WAPI 做扫描/连接/状态；板上 ESP-Hosted 的 SDIO 链路属平台侧工作。
  模拟器里 `wifi_sim` 提供伪无线设备（上游 `wifi_sim.c` 少一个 `<nuttx/mutex.h>` 引用，
  本项目顺手修了，否则 flat 构建链接不过）。
- **热点**：从机固件是 STA 版，`set_mode(AP)` 返回 0 但 `set_config(AP)` 失败（-1），
  恢复 STA 也会失败甚至卡死 RPC。所以应用**如实报"不支持"**，界面显示原因而不是假装能开。
- **WiFi 扫描**：板上 esp_hosted 的扫描 RPC 曾出现 5s 无响应；此时界面进入"无线不可用/
  无接入点"的降级态，自检把**驱动 errno** 写进证据，不把平台缺陷说成"附近没有 AP"。

### 应用层的能力边界

- **运动检测是帧差**，不是人形/人脸识别。它驱动"主动事件"叙事，文档不把它写成别的算法。
- **模拟器的相机是合成源**（画面里有移动方块），页面与自检都明确标注 `synthetic`。
- **`--say` 收不到中文**：NSH 不会把非 ASCII 送进 argv，中文对话请从界面发。
- **同一 session 只能跑一次**（LVGL 全局态不能二次初始化）。
- **模拟器根文件系统写不进文件**：这份 defconfig 没编 `FS_TMPFS`，`/` 是伪文件系统
  （`mkdir /tmp` 看起来会成功，但 `fopen("/tmp/...")` 一律 `errno=2`）。所以不给
  `--hostfs` 时默认快照路径必然失败，得到 `snapshot:written FAIL … -2`。
  **模拟器上快照必须走 `--hostfs <dir>`**；真机（`tmpfs` 挂在 `CONFIG_LIBC_TMPDIR`）和
  goldfish（`rc.sysinit` 里 `mount -t tmpfs /tmp`）都有真实可写的 `/tmp`。
- **`--snap` 只认目录**：文件路径只用来推导目录，每页固定写 `<dir>/dm_<page>.bin`；
  给 `/tmp/abc/one.bin` 不会产生 `one.bin`。
- **`--agent-spawn` 需要镜像里有 ai_agent**：sim / goldfish 配置没有
  `CONFIG_EXAMPLES_AI_AGENT_VELA`，加 `#if` 守卫后它会退化成一条明确告警（否则
  `undefined reference to ai_agent_main`，整条链接失败、连自检都跑不起来）。
- **自检不能在自己身上 sleep 测"提交是否增长"**：自检跑在 UI 线程上，`usleep` 期间
  `lv_timer_handler()` 不执行，提交计数当然不动。所以无 panel-beat 的平台只断言"相机出帧"，
  面板保活交给 `display:fed` 在不阻塞的窗口里回答。

### 镜像余量与取舍

- 应用槽 1,835,008 B，当前镜像 1,830,912 B，**余量 4 KB**。
- MCUboot `MCUBOOT_MAX_IMG_SECTORS=512` 限制镜像 ≤2 MB，所以"把槽位扩到 4 MB"这条路
  走不通；字体档位因此压到 14px/1bpp（见[九](#九字体与文案)）。

---

## 十二、架构与线程模型

### 12.1 目录结构

```
deskmate_main.c          NSH 内置命令 deskmate：参数解析 / 单线程 UI 循环 / 自检驱动
src/dm_config.h          全部可调常量（几何、节流、快照、运动阈值、路径）
src/dm_log.[ch]          分级日志 + 按调用点限速（默认每 __func__ 每秒 1 行）
src/dm_selftest.[ch]     无头自检与巡览状态机（跑在 UI 线程上）
src/hal/dm_display.[ch]  fbdev 后端（/dev/fb0 + lv_nuttx）/ 内存后端；快照 + 画面统计
src/hal/dm_touch.[ch]    GT911 轮询线程 + 脚本 indev（自检与无输入设备时的点击来源）
src/hal/dm_camera.[ch]   V4L2 后端（RAW8/RGB565/RGB888 + 2×2 去马赛克）/ 合成后端
src/hal/dm_net.[ch]      接口状态（getifaddrs + SIOCGIFFLAGS）、静态地址、DHCP、
                         WiFi（SIOCGIW* / WAPI，扫描/关联/断开/AP 探测）
src/agent/dm_agent.[ch]  agent bridge：mock（离线应答）/ ws（ai_agent 28789）/ off
src/ui/dm_theme.h        配色、字号、边距（DM_MARGIN/DM_STATUS_H/DM_TABBAR_H）
src/ui/dm_i18n.h         全部文案（字体子集就是从这里抽字形）
src/ui/dm_widgets.[ch]   卡片/按钮/徽标等基础控件
src/ui/dm_ui.[ch]        状态栏 + 页面容器 + Tab 栏 + toast + 路由 + 统一 tick
src/ui/dm_page_*.c       五个页面（页面首次进入时才构建）
fonts/dm_font_14.c       中文子集字体（唯一参与编译的档位）
```

### 12.2 线程模型

**UI 是单线程的**：LVGL、控件树、页面 update、自检、巡览、agent 轮询全在
`deskmate` 主循环里跑，因此**不存在跨线程操作 widget 的问题**——这也是自检能直接
在 UI 线程上点击/快照而不需要任何锁的原因。

主循环之外的辅助线程（全部 detach、只通过原子变量/快照与 UI 交换状态）：

| 线程 | 栈 | 职责 |
|---|---|---|
| `dm-touch` | 2 KB | 等 GT911 的 `poll()` 事件（驱动侧 LPWORK 每 8ms 采一次样并唤醒等待者，所以有手指就立刻返回；200ms 超时只是退出检查周期，不是采样周期），读到的坐标写进**硬件快照**；UI 线程只读快照，从此不碰 I2C |
| 相机采集 | 4 KB | V4L2 `DQBUF`/`QBUF` + 转 RGB565，发布带序号的帧；比 UI 低一档优先级，保证"没画面"优先于"画面不新鲜" |
| agent ws | 8 KB | 一次连接尝试，失败由下一次 poll 触发（5 秒节流） |
| WiFi 扫描 | 16 KB | 扫描 RPC（**栈给少了会静默死掉**，`g_scan_state` 永远停在 RUNNING，界面永久"扫描中…"） |
| DHCP | 8 KB | 异步取地址（DHCP 会阻塞几秒，不能放 UI 线程） |
| 显示自愈 | 8 KB | 面板停摆时重跑 bring-up（深调用链进 DSI/HAL，2 KB 会溢出到堆里，症状是 LVGL 内部莫名其妙的 load fault） |
| `ai_agent` | 24 KB | 仅 `--agent-spawn`，由 `task_create()` 起成同进程 NSH 外的任务 |

### 12.3 agent 协议（与 `tools/test/ws_demo.py` 一致）

```
-> {"type":"message", "chat_id":"deskmate-ui", "content":"..."}
<- {"type":"response","chat_id":"deskmate-ui", "content":"..."}
```

- `chat_id` 固定 `deskmate-ui`；后端既可以直接回话，也可以用
  `ws_server_send("deskmate-ui", text)` 把**主动消息**推进 UI。
- 主动消息有三个来源：设置页「主动播报演练」、相机页运动事件、以及后端自己推。
- 非对话页收到消息时弹顶栏 toast 提示，同时仍然写进对话历史（切回对话页可见）。
- 桥的五种状态：`off`（关闭）/ `mock`（离线应答）/ `ws`（已连上但未握手完成）/
  `online`（已连上）/ `offline`（连不上）。`mock` 用**设备真实状态**应答，
  所以没有网络/API key 也能把对话与主动事件跑通。

### 12.4 为什么这些坑都写进了注释

真机调试中反复出现的三类问题——**脏区合并导致整屏回写**、**同步 ioctl 拖停 UI**、
**线程栈不足静默死亡**——都不是靠"看代码顺眼"能发现的，只有量化证据（日志里的
`cost=`/`max_busy=`/`commits=`，或 JTAG 读到的全局变量）才定位得到。所以本仓库的
做法是：**每个修过的坑都留一条注释写明现象、根因、实测数字**，让下一个人不必重跑
一遍同样的实验。
