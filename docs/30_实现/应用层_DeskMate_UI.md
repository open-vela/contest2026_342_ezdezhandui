# DeskMate 应用层设计与模拟器闭环

> 2026-11-01 · openvela（NuttX）+ ESP32-P4X-C5-Function-EV-Board
>
> 本文讲应用层**怎么设计、怎么验证、证据在哪**。平台移植本身见
> `docs/01_开发流程/STATUS.md` 与 `docs/02_驱动实现/`。

## 一、为什么需要应用层

平台侧交付的是「板子能跑 openvela」：串口、以太网、显示、触摸、摄像头、音频都有真机证据。
但此前的「LVGL UI」只有公版 `lvgldemo`（一个控件演示），**不是产品**：

| 应用层要回答的问题 | 现状（本轮之前） |
|---|---|
| 这块屏上跑的是什么界面？ | 公版控件演示，与作品无关 |
| 摄像头画面进得来 UI 吗？ | 只有 `camera` / `v4l2ctl` 命令行工具 |
| WiFi / 网口在界面上看得见、能操作吗？ | 只有 NSH 命令，界面无感知 |
| Agent 对话能在屏上完成吗？ | 只能通过串口 / `ws_demo.py` |
| 没有真机时能验证 UI 与逻辑吗？ | 不能 |

本轮把应用层补齐：**一个应用、五个页面、四条外设能力、两种模拟器验证路径**。

## 二、产品形态

```
┌───────────────────────────────────────────────────────────────┐
│ DeskMate              [ Network ]            agent: mock 12:34│  ← 状态栏
├───────────────────────────────────────────────────────────────┤
│                                                               │
│   Home 页：构建身份 / 显示后端 / 四个能力状态 / 触摸自检按钮     │
│                                                               │
├───────────────────────────────────────────────────────────────┤
│  [ Home ] [ Camera ] [ Chat ] [ Network ] [ Settings ]        │  ← Tab 栏
└───────────────────────────────────────────────────────────────┘
```

| 页面 | 外设 | 关键点 |
|---|---|---|
| **Home** | 时钟、触摸 | 显示身份（分辨率/位深/后端）、能力状态（Camera/Network/Agent）、触摸演示按钮（自检也点它） |
| **Camera** | 摄像头 `/dev/video0` | 实时预览、帧计数、帧差运动量、抓拍落盘、「让 agent 看一下」 |
| **Chat** | agent | 气泡对话、预设短语（对应自定义 Skill）、软键盘输入、主动事件入库 |
| **Network** | eth0 + wlan0 | 接口列表（up/running/IP/掩码/是否无线）、WiFi 关联状态、扫描列表 + 连接按钮 |
| **Settings** | 显示 | agent 端点、手动快照（证据路径）、主动事件演练、关于 |

## 三、架构与关键设计

### 3.1 单线程 + HAL 抽象

```
deskmate_main.c   一个循环：lv_timer_handler → dm_ui_tick（时钟/当前页刷新/agent 轮询/提示条）
src/hal/          硬件抽象层：真机与模拟器**同一套接口**，差异只在后端实现
src/ui/           五页 + 路由（状态栏 / 内容区 / Tab 栏）
src/agent/        agent bridge：mock（离线应答）| ws（ai_agent 28789）| off
src/dm_selftest.c 无头自检状态机（证据生成器）+ 巡览演示（截图/GIF）
```

**为什么单线程**：LVGL 的 widget 树不是线程安全的。把 HAL 轮询、agent 轮询、页面刷新全放在
同一个循环里，就不存在跨线程操作 widget 这一类最难查的问题；代价是网络/相机采样必须非阻塞
（`poll(0)` / 每轮取一帧），这也是 `dm_camera_poll()` 与 `dm_agent_poll()` 的设计约束。

### 3.2 HAL 的四个后端选择（这是"能在模拟器里跑"的关键）

| HAL | 真机后端 | 模拟器后端 | 为什么要抽象 |
|---|---|---|---|
| `dm_display` | `fbdev`：`/dev/fb0` + `lv_nuttx_fbdev_create` | `mem`：LVGL 直接渲染进应用自持的 1024×600 RGB565 缓冲 | 无头宿主没有 `/dev/fb0`；自持缓冲让"快照=真相" |
| `dm_touch` | `/dev/input0`（GT911，经 lv_nuttx） | 脚本注入 indev（`dm_touch_tap`） | `SIM_TOUCHSCREEN` 依赖 X11，本机无 DISPLAY |
| `dm_camera` | `v4l2`：`/dev/video0`（SC2336 RAW8 → 2×2 去马赛克 → RGB565） | 合成动态源 | 宿主没有摄像头，但页面与事件链必须能跑 |
| `dm_net` | `getifaddrs` + `SIOCGIFFLAGS`；WiFi 走 WAPI（`SIOCGIW*`） | 同一套代码 + `SIM_NETDEV`（TAP）与 `wifi_sim`（伪无线） | 让"网口/WiFi 页"在模拟器里也有真实数据 |

> 关键取舍：**画面统计与快照判据与真机完全一致**。真机用 JTAG dump `/dev/fb0` +
> `tools/test/fb2png.py`；模拟器用 `dm_display_snapshot()` 写同一格式的 RAW RGB565，
> 仍用 `fb2png.py` 转换——同一条证据链，只是取帧手段不同。

### 3.3 Agent bridge：把 LLM 变成可替换的后端

```
UI ──dm_agent_send()──► mock（离线规则应答：时钟/网络/相机/速记）
                    └► ws  ──► ai_agent 内置 WebSocket 服务 (127.0.0.1:28789)
                                 -> {"type":"message","chat_id":"deskmate-ui","content":"…"}
                                 <- {"type":"response","chat_id":"deskmate-ui","content":"…"}
```

- 协议与既有 `tools/test/ws_demo.py` **完全一致**，不引入第二种约定；
- `chat_id` 固定为 `deskmate-ui`，于是 agent 侧一条
  `ws_server_send("deskmate-ui", text)` 就能把**主动消息**推进 UI（顶栏提示条 + Chat 页入库）；
- `mock` 后端不是玩具：它读**真实设备状态**（时钟、接口列表、相机后端、帧计数）再回答，
  所以离线自检覆盖的是"输入 → 逻辑 → UI"整条链，而不是打印常量。

### 3.4 主动事件（赛题③）

```
dm_camera_poll 每轮取一帧 → 与上一帧做采样帧差（阈值 1200/像素）→ 变化比例 ≥ 6%
   → dm_agent_push_proactive("Motion detected in front of the camera.")
      → UI 顶栏提示条（非 Chat 页时）+ Chat 页气泡
```

**诚实标注**：这是**帧差**，不是人形/人脸识别。它足以驱动"看见有人进门就主动说话"的产品
叙事，文档不把它写成别的算法。

## 四、模拟器闭环（本轮的核心交付手段）

### 4.1 双轨

| 轨道 | 配置 | 用途 | 命令 |
|---|---|---|---|
| **NuttX sim**（宿主二进制） | `nuttx/boards/sim/sim/sim/configs/deskmate/defconfig` | 日常迭代：秒级构建、无头、可断言 | `tools/test/sim_deskmate.sh` |
| **goldfish**（官方模拟器） | `board/vela/configs/goldfish-arm64-v8a-ap/defconfig`（→ `vendor/openvela/boards/vela/`） | 验收：与赛事官方模拟器环境一致（goldfish camera / GPU fb / goldfish events / virtio-net / wifi_sim） | `tools/test/emulator_deskmate.sh` |

### 4.2 sim 配置踩到的三处坑（都已在配置里注释清楚）

1. **图形设备选择项被关在 `if SIM_X11FB` 内** → 无 X11 时 `SIM_FRAMEBUFFER` 取不到值，
   `/dev/fb0` 不会注册，`CONFIG_VIDEO_FB=y` 反而会链接失败（`up_fbinitialize` 未定义）。
   本应用因此提供 `mem` 显示后端：**没有 framebuffer 也能渲染与取证**。
2. **无线链路的根开关是 `DRIVERS_WIRELESS`**：少了它，`NETDEV_WIRELESS_IOCTL →
   NETDEV_WIRELESS_HANDLER → DRIVERS_WIFI_SIM / SIM_WIFIDEV_NUMBER` 会被 Kconfig
   **静默丢弃**（`SIM_NETDEV=y` 照样生效，于是只看到 eth0，看不到 wlan0）。
3. **guest 根文件系统不可写**（`SIM_NETDEV` 的配置也验证了这点）：`wifi_sim` 需要的
   BSS 表必须经 `FS_HOSTFS` 落到宿主（`/host/wifi/bss`），应用启动时自己创建目录并写入。

另外顺手修了一个上游缺陷：`nuttx/drivers/net/wifi_sim.c` 调用 `nxmutex_*` 却没有
`#include <nuttx/mutex.h>`，在 flat 构建下变成隐式声明 → 链接期
`undefined reference to nxmutex_lock`。加一行头文件即可（与项目里其它平台级修复同类）。

### 4.3 一键脚本做什么

`tools/test/sim_deskmate.sh`：

```
宿主适配（libunwind dev 符号链接缺失时用私有 LIBRARY_PATH shim；
         清掉别的配置在 apps/builtin 下留下的陈旧生成头）
→ ./build.sh nuttx/boards/sim/sim/sim/configs/deskmate --cmake -j8
→ 以短路径 hostfs 运行：printf 'deskmate --selftest|--tour --hostfs /tmp/dm-sim-<ts>\n' | <nuttx>
→ 轮询日志里的收尾标记（DM-SELFTEST RESULT / DM-TOUR DONE）即收掉模拟器
→ 断言：DM-READY / 自检 PASS / 0 个 FAIL / 快照存在 / hostfs 挂在预期目录
→ fb2png.py 把所有快照转 PNG 并打印色数·主色占比·梯度（tour 模式再拼 GIF）
→ 归档 console.log + 每页快照 + PNG + result.txt，最后打印 RESULT: PASS|FAIL
```

- 一条命令**约 3 秒**（增量构建后；等收尾标记而不是死等超时）；
- 证据默认落 `/tmp/deskmate-runs/<时间戳>/`：本仓 `logs/` 是未跟踪目录，
  共享工作区里其它会话的 `git clean -xdf` 会把它清掉（实测踩过）；要入库用 `--out`；
- 无需 X11、无需摄像头、无需 root。

### 4.4 巡览演示（`--tour`）

自检是"证明它对"，巡览是"让人看见它"：`deskmate --tour` 依次走完五个页面并各拍一张
快照，相机页额外连拍 5 帧；宿主侧把它们转 PNG 并拼成 `deskmate_tour.gif`。

```bash
tools/test/sim_deskmate.sh --tour          # 一条命令：跑 → 9 张 PNG → GIF
```

产出的 9 帧与 GIF 已归档在 `docs/40_验证与证据/截图与对照日志/应用层_模拟器快照/tour/`。

### 4.5 自检断言表（证据 = 控制台 PASS 行 + 每页 PNG）

| 断言 | 证明 |
|---|---|
| `display:ready` / `lvgl:version` | 显示后端与 LVGL 版本就绪 |
| `ui:built` / `ui:home` | 5 个页面注册、页面树建成、停在首页 |
| `touch:target` / `touch:delivered` | 注入点击命中**真实按钮**并使计数从 0→1（不是静态图） |
| `ui:tab-target` / `ui:camera-page` / `ui:net-page` | 点 Tab **真的切页** |
| `ui:page-rendered` | 切页前后**帧指纹不同**（首页 ≠ 相机页） |
| `net:interfaces` | 接口状态可读（模拟器：`wlan0 UP <ip>`） |
| `wifi:scan` | WAPI 扫描真的返回 AP（模拟器：3 个 AP + RSSI，来自应用写入的 BSS 表） |
| `snapshot:written` | 每页快照写入成功且字节数 == 几何（1024×600×2 = 1,228,800） |
| `snapshot:picture` | 画面是"画出来的 UI"：色数 ≥3、主色 ≤90%、平均亮度梯度 ≤8（与 `fb2png.py` 同判据） |

## 五、验证记录（本轮实测）

| 项 | 结果 |
|---|---|
| sim 构建 | `build completed successfully`（干净构建 02:2x，增量 5–9s） |
| sim 自检 | `DM-SELFTEST RESULT: PASS (16/16)`；`RESULT: PASS`；一次运行约 3 秒 |
| 网口 | `wlan0 UP 10.0.0.2`（`SIM_NETDEV` TAP；无线优先显示） |
| WiFi | `wifi_sim` 伪无线设备注册成功；WAPI 扫描返回 **3 个 AP**（`DeskMate-Lab -42 dBm` 等） |
| 摄像头 | 合成后端出帧；运动事件 `motion 98% → proactive event` |
| 页面 | Home / Camera / Network 三页各出一张 1024×600 PNG（`docs/40_验证与证据/截图与对照日志/应用层_模拟器快照/`）：Home 131 色 / 主色 56.9% / 梯度 2.90、Network 172 色 / 51.6% / 1.94、Camera 13,715 色 / 44.7% / 2.66（相机页是连续色调预览，色数天然高）；`--tour` 另有 9 帧 + GIF |
| risc-v 编译 | 应用 14/14 源文件用 P4 配置的**真实编译参数**编译通过（含 V4L2 路径） |
| 真机 P4 | 板级 defconfig 已并入应用（`CONFIG_EXAMPLES_DESKMATE=y`）；**整镜像构建/上板未在本轮完成** |

## 六、真机侧

板级配置 `board/esp32p4/esp32p4-function-ev-board/configs/nsh/defconfig` 追加：

```
CONFIG_EXAMPLES_DESKMATE=y
CONFIG_EXAMPLES_DESKMATE_AGENT_MODE="ws"
CONFIG_EXAMPLES_DESKMATE_CAMERA_DEVPATH="/dev/video0"
CONFIG_LV_FONT_MONTSERRAT_28=y
```

四个外设的配置**本来就在**：`/dev/fb0`（MIPI-DSI EK79007 1024×600 RGB565）、
`/dev/input0`（GT911）、`/dev/video0`（SC2336 → MIPI-CSI → V4L2）、`eth0`（esp_emac）
与 wlan0（ESP-Hosted）。真机运行：

```
nsh> deskmate                      # 起 UI
nsh> deskmate --selftest           # 同一份自检（快照 → /host 或 /tmp）
```

真机取证沿用既有链路：`tools/test/board.py`（串口断言）+ JTAG dump `/dev/fb0` +
`tools/test/fb2png.py`（同判据）。

## 七、边界与后续

- **真机 P4 整镜像构建/上板运行未完成**：本轮构建时同一工作区的另一会话正在改
  `esp_hosted` 并占用同一个 `cmake_out` 构建目录，整镜像构建卡住；应用源码已用 P4 真实
  参数逐个编译通过，但"编译出 nuttx.bin → 烧录 → 跑 `deskmate`"这一段还没有真机证据。
- **goldfish 验收轨未执行**：配置已交付（含 manifest 分组与 export 映射），未跑起来验证。
- **WiFi 真机链路**（ESP-Hosted SDIO 枚举）属平台侧工作，不在本应用层范围内；
  应用层按"wlan0 存在即用、不存在即在网络页显示不可用"设计，并有 `wifi:scan` 断言兜底。
- **CJK 文案**：当前 UI 为 ASCII（`lv_font_montserrat_*`）；中文文案已集中到
  `src/ui/dm_i18n.h`，`tools/engineering/gen_dm_font.sh` 可按该文件抽取字形生成子集字体
  （离线回退 `EXAMPLES_DESKMATE_FONT_SIMSUN`，LVGL 内置 SimSun 16，约 149 KB）。
- **TTS 播报**：应用侧**尚未**实现 `dm_audio`（平台侧 ES8311 播放链已打通）；主动事件目前只"上屏"，
  接上 `dm_audio` + TTS 即可"出声"——这是明确的下一步，不是已完成项。
