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

## 八、中文 UI（2026-11-02）

界面语言从英文切到**中文**，并且不依赖内置大字体：

| 项 | 做法 | 实测 |
|---|---|---|
| 文案 | `src/ui/dm_i18n.h` 全中文（80 个键）；页面/agent 里面向用户的字面量全部收进来 | — |
| 不翻译 | `[dm-check]` 键名、`DM-READY/DM-EXIT/DM-TOUR DONE/DM-SELFTEST RESULT:`、`DM_LOG` 诊断、NSH 命令与参数 | 脚本断言不受影响 |
| 字体 | 自绘子集：`fonts/dm_font_16.c`（正文）+ `fonts/dm_font_24.c`（标题），4bpp，`--no-compress` | 字形数据 **15.4 KB + 29.2 KB**；P4 镜像余量 **491 KB** |
| 生成 | `tools/engineering/gen_dm_font.sh` 扫描应用全部字符串字面量取字形 | 324 字形（229 非 ASCII） |
| 拉丁字形 | **拉丁取自 DejaVuSans、CJK 取自 DroidSansFallback（两字体 merge）** | 见下"踩坑" |
| 断言 | `font:cjk-body` / `font:cjk-title`：`lv_font_get_glyph_dsc()` 逐字查轮廓 | 真机与模拟器均 `0 字缺` |

### 8.1 两个踩过的坑

1. **`DroidSansFallbackFull.ttf` 是纯 CJK 回退字体，不含 ASCII**。只用它生成时，
   所有字母/数字都没有轮廓（UI 上会画方框）。`lv_font_conv` 支持多个 `--font`，
   正解是 `--font DejaVuSans --range 0x20-0x7e --font DroidSansFallback --symbols <中日韩字符>`。
2. **`--symbols` 里塞 ASCII 不可靠**：含 `\ " $` 等字符时会被工具当转义吞掉，症状是
   "个别字母缺字"。ASCII 一律走 `--range`。
3. 顺手修掉一个断言实现陷阱：LVGL 的 `lv_text_encoded_next` 是 `lv_text.c` 里的
   **文件内常量函数指针**（不是导出符号），直接调用会跳进数据段把 UI 线程挂死；
   断言里自带一个 20 行的 UTF-8 解码器。

## 九、真机移植记录（ESP32-P4，2026-11-02）

板子：`/dev/ttyACM0`（烧录/JTAG）+ `/dev/ttyUSB0`（console，115200，需 assert DTR/RTS）。
启动日志确认四路外设都在位：

```
SC2336 chip ID: 0xcb3a            -> Camera registered on /dev/video0
/dev/fb0 ready 1024x600 RGB565 @ 0x48000040 (1228800 bytes)
GT911: cfg x_max=1024 y_max=600   -> GT911 touchscreen registered at /dev/input0
eh_hosted_netdev: wlan0 registered (STA channel bound) / wlan0 up
NuttShell (NSH) -> nsh>
```

### 9.1 第一次上板暴露的两个真问题（都已修）

| 现象 | 根因 | 修法 |
|---|---|---|
| `snapshot:written FAIL -2 bytes` | 默认快照目录写死 `/host/deskmate`，真机没有 hostfs → `-ENOENT` | 按 hostfs 是否真的挂上决定目录：挂上 `/host/deskmate`，否则 `/tmp/deskmate`（tmpfs） |
| 点 Tab 切到相机页后 **断言死机**（`nsh_main`） | 首帧未就绪时 `dm_camera_poll()` 早退（`-EAGAIN`），`g_frame.pixels` 仍是 NULL，页面把它交给 `lv_canvas_set_buffer()` | ① `dm_camera_create()` 先填好 frame；② `poll()` 早退前也填；③ 页面再加一块黑底兜底缓冲 |
| 相机 `esp_cam_ctlr_start failed: 258` | P4 上请求 `RGB565` 能过 `S_FMT` 但 `start_capture` 失败 | 格式优先级改成 **RAW8(SBGGR8) 优先**，应用侧做 2×2 去马赛克 |
| `wifi:scan FAIL rc=-19 (ENODEV)` | 板级配置缺无线 ioctl 层 → 应用侧 `SIOCGIWNAME` 探不到 wlan0 | defconfig 补 `DRIVERS_WIRELESS` / `NETDEV_WIRELESS_IOCTL` / `NETDEV_WIRELESS_HANDLER`（esp_hosted 的 netdev 实现了 `SIOCGIWSCAN`） |

### 9.2 真机自检：**PASS 18/18**

```
[dm-check] display:ready                PASS 1024x600 16bpp fbdev
[dm-check] font:cjk-body                PASS 0 字缺（首个：-）
[dm-check] font:cjk-title               PASS 0 字缺（首个：-）
[dm-check] touch:delivered              PASS 0 -> 1
[dm-check] snapshot:written             PASS dm_home / dm_camera / dm_net -> 1228800 bytes
[dm-check] ui:page-rendered             PASS home=0xd941215b camera=0xd0cd54d2
[dm-check] net:interfaces               PASS eth0 up 0.0.0.0; wlan0 UP 0.0.0.0
[dm-check] wifi:scan                    PASS 0 AP, driver rc=5 (radio scan unavailable, UI degraded)
[dm-check] snapshot:picture             PASS distinct=75 dominant=50% grad=0 n=12642
DM-SELFTEST RESULT: PASS (18/18)
```

证据文件：`docs/40_验证与证据/截图与对照日志/应用层_真机快照/selftest_report_18of18.txt`。

**WiFi 扫描的平台侧发现（重要）**：开了无线 ioctl 层后，`wlan0` 能被识别、`SIOCGIWNAME`
仍不支持（改用名字前缀兜底），而 `SIOCSIWSCAN` 会走到 esp_hosted 的 RPC，但
**从机侧 5 秒无响应**：

```
eh_host_feat_rpc: request: no response uid=23 msg_id=289 (5000 ms)
eh_hosted_netdev: ioctl cmd=0x8b19 (SIOCSIWSCAN)
[dm] wifi: scan trigger rc=5, fetching results anyway
[dm] wifi: scan found 0 AP(s) on wlan0
```

所以"扫不到 AP"不是应用层问题，而是 esp_hosted 的扫描 RPC 在板上超时（平台侧）。
应用层的行为是正确的：触发失败仍尝试取结果、取不到就把界面置为"无线不可用/无接入点"，
断言把驱动的真实 errno 打进证据（`driver rc=5`），不把平台缺陷说成"附近没有 AP"。

另外，板上串口会被 esp_hosted 的调试日志淹没（每秒数百行），应用输出会丢；
因此自检新增 `--report <path>`：把报告同时写文件，读法：
`board.py run "deskmate --agent mock --selftest --report /tmp/dm_report.txt"` 之后
`board.py run "cat /tmp/dm_report.txt"`。

### 9.3 取证链路

```bash
# 1) 烧录（MCUboot 双镜像，两镜像均 hash 校验）
tools/build_flash/usb_stable.sh
# 2) 复位并确认外设/target
python3 tools/test/board.py reset
# 3) 跑自检
python3 tools/test/board.py run "deskmate --agent mock --selftest" --timeout 120
# 4) 屏幕取证：帧缓冲 → PNG（OpenOCD dump + 与模拟器同一套判据）
tools/test/hw_deskmate_evidence.sh
```

> console 端口被 Tabby 串口页占用时，读串口会报
> `device reports readiness to read but returned no data`。本仓自带
> `tools/engineering/tabby_mcp.py`（Tabby MCP），`close_tab` 关掉那个串口页即可释放。

## 十、真机两个阻塞项的根因（2026-11-02 晚）

### 10.1 “面板整屏纯蓝、看不到界面” —— 根因是摄像头，不是显示链路

判据链（每步都有明确观测）：

| 观测 | 结论 |
|---|---|
| 复位后、应用未启动：屏幕**纯红**（板级 `BOARD_FB_TEST_COLOR=0xf800`） | 面板能显示帧缓冲，DSI/背光/缓存回写都通 |
| 运行公版 `lvgldemo widgets`：**能看到彩色控件** | LVGL + fbdev + GDMA 通路完全正常 |
| 运行 `deskmate`：**纯蓝/纯色**，但 JTAG dump 的帧缓冲里是正常中文界面（121 色、3% 亮像素） | 应用渲染没问题，是**应用启动后**把显示搞丢了 |
| `deskmate --no-camera`：**浅色界面 + 中文正常显示** | **开摄像头（MIPI-CSI）会让 DSI 失去同步** |

解释：P4 上 CSI 与 DSI **共享 MIPI PHY**；`/dev/video0` 一被打开（本板还伴随
`esp_cam_ctlr_start failed: 258`），面板就丢失同步，屏上只剩面板自身的底色。

处置（本轮）：
- **摄像头默认关闭**：新增 `--camera` 显式打开；不传就完全不碰 `/dev/video0`。
- 相机页在"没有摄像头"时**照样建页**（显示原因），不再让路由失败。
- 主题改为**浅色高对比**（白卡片 on 浅灰底、蓝强调条），字号 16px→**20px 正文 / 32px 标题**
  —— 之前的深色卡片 + 16px 字在 7″ 1024×600 上远看就是"一片深蓝"。
- 平台侧 CSI/DSI 共存问题**尚未修**（需要动 PHY/ISP 时序），已记录为后续项。

### 10.2 WiFi：扫描超时是主机侧 bug；扫描已能跑通，但 CP 报 0 个 AP

根因（有日志证据）：

```
eh_host_feat_rpc: request: no response uid=23 msg_id=289 (5000 ms)   ← WifiScanGetApNum
eh_host_feat_rpc: rx: no waiter for uid=23 (late response?) dropping
```

CP 其实回了，只是**超过主机 5s 同步超时被丢弃**（触发扫描那条已经是 30s，
取结果这两条还是默认 5s）。修法：
- `eh_host_wifi_scan_get_ap_num/get_ap_records/sta_get_ap_info` 按请求把超时提到 **15s**。
- netdev 的 `SIOCSIWSCAN` 不再传 `cfg=NULL`，改成**显式 2.4G 全信道**扫描配置
  （`ghz_2_channels=0x7ffe`：bit0=0 表示按位图扫，bit1..14 = 信道 1..14；
  注意 bit0=1 是"跳过整个 2.4G"，写 `0xffff` 会扫不到任何东西）。
- 应用侧：扫描移出 UI 线程（detached pthread + 队列），网络页显示"扫描中…"，
  进页面自动扫一次；新增 `deskmate --wifi-probe`（可配 `--report` 写文件）。

现状（**已用 JTAG 读 RAM 定论**，不依赖串口/人眼）：

```bash
# 符号地址从 ELF 取，值从板上 RAM 读（串口被驱动日志占满时的取证手段）
tools/test/hw_var_read.sh g_scan_state g_scan_count g_scan_err g_wifi_last_errno
# 0x4ff6437c: err=0  count=0  state=2(DONE)  last_errno=0
```

即：**扫描正常完成、没有任何驱动错误，但确实一个 AP 都没扫到**（`g_scan_aps` 全 0）。
所以不是应用/传输层 bug —— 是**射频层没收到任何 AP**。C6 只支持 2.4 GHz，而现场
保存的 SSID 叫 `Hello_World_5G`（5G），模块用的是**板载天线**。结论：要么附近没有
2.4G AP，要么信号弱到扫不到。要证明射频通路，需要一个 **2.4G 的 SSID+密码**做一次
关联（关联成功 + RSSI 非 0 就是铁证）；否则只能记录为"环境无 2.4G AP"。

另外修掉了一个会干扰扫描的问题：网络页原来**每 50ms** 就 `getifaddrs` + `SIOCGIWESSID`
（netdev 每个 ioctl 还打一行 INFO），把驱动 RPC 层和串口都刷爆了。现在列表刷新节流到
**1s**，netdev 的 ioctl 追踪改成编译开关。

### 10.3 取证方式的变化（重要）

ESP-Hosted 的 INFO 日志在 SDIO 数据路径上每秒数百行，会把 115200 的 console 占满
（实测 `nsh_consolewrite: ERROR: Failed to send buffer: 12`，`cat` 都发不出去）。
因此：
- 平台侧把 `esp_log.h` 默认级别压到 **WARN/ERROR**（要细节时 `-DLOG_LOCAL_LEVEL=3`）。
- 真机证据一律走 **`--report <path>` 写文件**，或直接**看屏幕**（本轮两个根因都是靠屏幕定的）。

### 10.4 WiFi 扫描为什么一直是 0 个 AP（本轮排查到的所有环节）

按"从最外层往里剥"的顺序，每一条都是真机上验证过的：

| # | 发现的真问题 | 修法 | 结果 |
|---|---|---|---|
| 1 | 扫描**取结果**的 RPC 用默认 5s 同步超时，CP 回包被丢（`no response (5000 ms)` → `late response dropping`） | scan_get_ap_num / get_ap_records / sta_get_ap_info 按请求提到 15s | 扫描不再超时，`rc=0` |
| 2 | 网络页每 50ms 一次 `getifaddrs`+`SIOCGIWESSID`，netdev 还每个 ioctl 打一行 INFO | 列表刷新节流 1s；ioctl 追踪改编译开关 | 串口不再被淹，RPC 不再被 UI 刷爆 |
| 3 | 应用拿不到"断开"：空 ESSID 在 netdev 里直接 `-EINVAL`，无法把从机从关联中拉出来 | 空 ESSID → `Req_WifiDisconnect` | 应用现在能主动断开 |
| 4 | **开机自动连硬编码测试 SSID**（`ESP_WIFI_TEST_SSID="Hello_World_5G"` / ALT `"..._2.4G"`，构建时注入）把 CP 的 STA 长期卡在"连接中"，IDF 直接拒绝扫描（`STA is connecting, scan are not allowed!`） | 该块改为需显式 `CONFIG_ESPESSIF_HOSTED_TEST_AUTOCONNECT` 才编译 | 开机不再自动连 |
| 5 | 扫描前没有把从机拉回 STA 模式；开机日志实测 `RPC wifi_set_mode(STA) -> -1`，而从机非 STA 时 IDF 不给扫描结果（BY.18 也记过"scan 前再发 set_mode 并回读"） | 扫描前 best-effort 发一次 `set_mode(STA)` | 待复测 |

加上重试（6 次 × 6s，对齐 BY.19 里"等从机空闲再扫"的做法）之后，**扫描仍然是 0 个 AP、
且驱动不报错**（JTAG 读 RAM：`g_scan_state=2(DONE) g_scan_count=0 g_scan_err=0`）。

对照结论：`docs/30_实现/连接_WiFi_SDIO.md` 的 BY.19（2026-11-01）在**同一块板**上
用**端口开机线程里的直接调用**（`eh_host_wifi_scan_start(NULL,true)`）扫到过 25 个 AP
（`KGDZ_SHARE -39dBm ch11` 等），而 BY.18 明确写了"`wlan0` 的控制面**不能靠 `SIOCSIWSCAN`**"。
所以剩下这条差异是**调用路径**：应用走 netdev ioctl → 官方栈 API，和端口线程里直接调
官方栈 API 不等价（任务上下文/模式状态不同）。

下一步（未做）：把扫描做成**板级能力**（内核态小 helper，或 boardctl/私有 ioctl），让应用
通过"和开机线程同样的调用路径"拿 AP 列表；这也是让 `wifi:scan` 断言能升级成"必须 ≥1 个 AP"
的前提。

### 10.5 收官：扫描 0 个 AP 的真正根因是**主机侧解析**（已修，真机验证）

第 10.4 节排掉四层问题后，我用 JTAG 读 netdev 新增的诊断全局，一次看清整条链：

```
g_dbg_wifi_setmode_rc = 0     # 扫描前 set_mode(STA) 成功
g_dbg_wifi_mode_rc    = 0
g_dbg_wifi_mode       = 1     # WIFI_MODE_STA
g_dbg_wifi_scan_rc    = 0     # eh_host_wifi_scan_start 成功
g_dbg_wifi_apnum_rc   = 0
g_dbg_wifi_apnum      = 30    # ← 从机其实扫到了 30 个 AP！
```

**从机扫到了 30 个 AP，而应用侧 `g_scan_count` 一直是 0** —— 问题在主机侧取值：

1. **netdev 打包 `iw_event` 时没有填 `ev.len`**（只填了 `cmd` 和联合体）。应用侧解析
   循环第一步就是 `if (ev->len < IW_EVENT_HEADER) break;`，于是**第一个事件就把整张表
   丢掉**，`found` 恒为 0。
   修：netdev 打包时 `ev.len = IW_EV_LEN(field)`；应用侧同时兜底——`ev.len` 非法时按
   `cmd` 反推长度（`dm_wifi_event_len()`）。
2. **ESSID 指针不符合 WAPI 约定**：netdev 直接塞内核地址，应用侧按"缓冲内偏移"规则解析
   → SSID 恒为空串。修：netdev 把名字字节**内联到缓冲区**、`pointer` 存偏移；应用侧同时
   兼容"偏移"与"直接地址"两种形式。
3. **没打包信号强度**：netdev 只发了 AP/ESSID/FREQ 三种事件，应用拿不到 dBm。修：补
   `IWEVQUAL`（`level = ap->rssi`）。

修完真机结果（`--report` 文件与 JTAG 双证据）：

```
[dm-check] wifi:scan                    PASS 1 AP, first='Hello_World_2.4G' 0 dBm
DM-SELFTEST RESULT: PASS (18/18)
```

JTAG 直读应用内存的等价证据（`g_selftest_scan_evidence`）：

```
wifi:scan PASS 1 AP first='Hello_World_2.4G' bssid=88:00:33:77:ba:69 rssi=0 ch=0
```

`rssi=0` 是从机那条 AP record 里本来就是 0（信道/强度字段未填），不是解析问题——留作后续。

**复盘**：这一路真正卡住的不是射频、不是 RPC 超时、也不是驱动不支持，而是**主机侧两个
"结构体没填全"的协议细节**。诊断全局（`g_dbg_wifi_*`）是把"到底是射频没收到还是我们没
解析对"一刀切开的关键手段；串口不可用时，JTAG 读 RAM 比任何日志都可靠。

## 十一、网络页重做（类 PC 设置）与热点结论

### 11.1 页面结构（1024×600，内容区 1024×456）

| 区块 | 内容 |
|---|---|
| 左：网络接口 | eth0 / wlan0 的状态、地址、掩码（1s 节流刷新） |
| 右上：以太网 | **自动 (DHCP)** / **手动** 切换；手动时三个字段（地址/掩码/网关）点击弹**数字键盘**输入；`应用` 按钮；状态行 |
| 右下：无线 | 状态行（已连接 SSID + dBm）；`扫描` / `断开` / `热点`；扫描结果列表（SSID + dBm + 加密标记），点 `连接` 弹**密码键盘** |

**5 秒未确认自动回滚**（类 PC）：应用静态地址后弹确认框，5 秒内没点 `保留设置` 就回到应用前的配置。
所有地址输入先过 `dm_net_ipv4_valid()`（四段、每段 ≤255），格式不对直接拒绝，不会把错地址写进去。

### 11.2 平台侧补了什么

- netdev 新增 `SIOCSIWENCODE / SIOCSIWENCODEEXT`：把密码暂存，`SIOCSIWESSID` 时一起
  `set_config(STA, {ssid,password}) + connect`。NuttX 对 **legacy** netdev 会把
  `_WLIOCVALID` 命令直接转给 `d_ioctl`，所以落点在驱动的 ioctl 里（不是 `iw_ops->passwd`，
  那只对 lowerhalf 驱动生效）。
- 应用新增异步 API：`dm_net_if_set_dhcp_async()`（DHCP 几秒，走后台线程）、
  `dm_net_if_set_static()`、`dm_net_if_get_config()`、`dm_net_ap_supported()`/`ap_start()`。

### 11.3 热点：**平台不支持**（有证据）

最小实验（`SIOCSIWAP` → 直接调官方栈）：

```
g_dbg_ap_setmode_rc = 0     # set_mode(WIFI_MODE_AP) 被接受
g_dbg_ap_setcfg_rc  = -1    # set_config(WIFI_IF_AP) 被从机拒绝
g_dbg_ap_restore_rc = -1    # 连"恢复 STA 模式"也失败
```

原因：镜像里的从机固件是 **STA 专用版**（`tools/build_flash/c6_hosted/eh_cp_wifi_sta_esp32c6_sdio.bin`）。
换固件需要 **C6 的 UART**，而板上只引出 `/dev/ttyACM0`、`/dev/ttyUSB0`（没有 C6 串口），
所以是硬件级动作。**更重要的是**：这个失败实验会把从机留在坏状态——之后所有 RPC 超时、
应用连 `dm_net_refresh()` 都过不去（要**断电重启**从机才能恢复）。
因此该实验已用 `CONFIG_ESPRESSIF_HOSTED_AP_PROBE` **默认关闭**，未开启时 `SIOCSIWAP` 直接返回
`-ENOTSUP`；UI 上热点按钮显示原因而不是假装能开。

### 11.4 模拟器验证（本轮证据）

```
[dm-check] net:ipv4-valid    PASS accepts 3 valid, rejects 4 invalid
[dm-check] net:eth-static    PASS if=wlan0 rc=0 ip=192.168.77.9 mask=255.255.255.0
[dm-check] net:ap-degraded   PASS supported=0 rc=-138 (CP 为 STA 版固件)
DM-SELFTEST RESULT: PASS (21/21)
```

截图：`docs/40_验证与证据/截图与对照日志/应用层_模拟器快照/net_pc_like_1024x600.png`
（浅色卡片 81% 亮像素、113 色、强调蓝 9988 采样点）。

### 11.5 密码连接：真机验收通过（并纠正上一轮的两个误判）

**纠正 1**：上一轮说"从机被 AP 实验卡死、应用连 `dm_net_refresh()` 都过不去"——**是错的**。
真正卡住的是 `dm_agent_init()` 的 **ws 连接**（默认 `AGENT_MODE="ws"`，ai_agent 没起来时
`connect()` 阻塞）。用 `--agent mock` 跑，一切都正常（`eth0 ip=192.168.1.110`、扫描 1 个 AP）。
→ 这是真实的产品缺陷：**后端服务没起来不该让整个 UI 起不来**，Agent 页实现时要把 ws 连接
改成带超时的非阻塞（本轮先记录）。

**纠正 2**：`SIOCGIWESSID` 回读的是主机**刚写进去的配置值**，未关联时也非空 —— 用它判"是否
连上"会**假阳性**（上一轮 `link_ok=1` 因此不可信）。已改为读 **`SIOCGIWAP`（已关联 AP 的
BSSID）**，未关联时驱动返回错误。

**密码发不出去的真 bug**：NuttX 的 `struct iw_encode_ext` 里 `key[]` 是**零长数组**，
应用里 `if (ext.key_len > sizeof(ext.key)) ext.key_len = 0;` 把密码长度**清成了 0**。
改成"结构体 + 尾部缓冲"整体，长度按 `sizeof(ext) + key_len` 传。

真机验收（`--agent mock --wifi-connect Hello_World_2.4G <psk>`）：

```
eh_hosted_netdev: passphrase stored (9 bytes)
eh_hosted_netdev: joining "Hello_World_2.4G"
DM-WIFI-CONNECT ssid='Hello_World_2.4G' rc=0
```

JTAG 读 RAM（串口丢行时的可靠证据）：

```
g_app_wifi_link_ok   = 1
g_app_wifi_link_rssi = -31 dBm      # 走 SIOCGIWSTATS
g_dbg_wifi_rssi      = -31 dBm      # netdev 侧同一个值
```

即：**选网 + 输密码 + 关联成功 + 拿到 RSSI**，整条链路在真机跑通。

## 十二、ws 阻塞修复（UI 不再被后端拖死）

**症状**：默认 `AGENT_MODE="ws"`、ai_agent 没起来时，应用日志停在
`[dm] agent: mode ws, endpoint 127.0.0.1:28789` 就没了，UI 根本不出来。

**根因**：`dm_agent_ws_connect()` 里是**阻塞且无超时**的 `connect()`（外加阻塞式 handshake）。

**修法**：
- connect 改**非阻塞 + poll 1.2s 上限**；收/发各加 2s `SO_SNDTIMEO/SO_RCVTIMEO`；
- ws 连接挪到 **detached 线程**，`dm_agent_init()` 立即返回；
- `dm_agent_poll()` 里按 **5s 节流**重连。

**真机验证**（默认 ws、ai_agent 未起）：

```
[dm] agent: mode ws, endpoint 127.0.0.1:28789
[dm] net: refresh begin
[dm] net: refresh step=getifaddrs
[dm] net: refresh step=summarize
[dm] net: refresh done
```

## 十三、摄像头 CSI/DSI 共存：机理进展（未完成）

已确认的三件事（代码 + 实测）：

1. **CSI 与 DSI 共享同一个 DW-GDMA 控制器**（`esp_mipi_dsi.c` 的注释与 `esp_csi.c` 的
   诊断打印都基于这一点）。DSI 已让出 ch0、用 ch1；摄像头用 ch0。
2. **DSI 侧的防误复位判据对 CSI 无效**：DSI 用
   `dw_gdma_ll_is_bus_clock_enabled(0)` 判断"控制器是不是已经被别人占用"，但 **CSI 侧完全
   不走 `dw_gdma` 框架**（`esp_csi.c` 只读寄存器做诊断，真正的通道配置交给注入的
   `upper_hal_cam` HAL 里的 `dw_gdma_channel_config_transfer()`），所以摄像头不会去
   开总线时钟/拿 group handle → DSI 初始化时判定"我是第一个用户"→ **复位整个控制器**
   → 摄像头的 ch0 配置被抹掉，后续 `esp_cam_ctlr_start` 失败。
3. `esp_cam_ctlr_start` 的失败路径：HAL 里 `s_ctlr_csi_start()` 先查
   `csi_fsm == CSI_FSM_ENABLED`（不满足返回 `INVALID_STATE`），再 `esp_cache_msync`、
   `dw_gdma_channel_config_transfer`、`dw_gdma_channel_enable_ctrl`、`mipi_csi_brg_ll_enable`；
   258 是 `INVALID_ARG`，最可能来自 `handle == NULL`（`priv->ctlr` 没建起来）或
   注入 HAL 里上一层的参数检查。**下一步**：把 `CSI: ctlr start rc=%d` 这行日志
   可靠地取回来（串口丢行时用 `--report`/JTAG 读诊断全局），据此定位到具体那一句。

**修法方向**（待实现）：
- 让摄像头路径在初始化时**登记自己对共享 GDMA 的占用**（`dw_gdma_acquire_group_handle()`，
  会打开总线时钟），使 DSI 的判据生效、不再全局复位；
- 摄像头 start 时**不得**做控制器级复位（只配置自己的通道）；
- 新增自检判据 `csi:dsi-coexist`：开摄像头后读 DSI 侧 DMA/桥寄存器仍在跑 +
  屏上画面判据仍成立。

## 十四、CSI/DSI 共存：**已修好**（真机 22/22）

### 14.1 `esp_cam_ctlr_start failed: 258` 的真根因：缓冲没按缓存行对齐

给 CSI 侧加了 JTAG 可读的逐步诊断（`g_dbg_csi_*`，串口丢行时靠它），真机读数：

```
translen = 921600 (1280x720 RAW8)   msync_rc = 258   cfg_rc = -999   en_rc = -999
```

即 `esp_cache_msync(M2C)` 直接返回 `ESP_ERR_INVALID_ARG`：它要求缓冲地址按
**cache line(64B)** 对齐，而应用交给 V4L2 的缓冲是 `malloc`（8/16B 对齐）。

**修法**：`g_bufs[i].mem = memalign(128, bytes)` → 真机复测 `start_rc = 0`、
`msync/cfg/en` 全 0。

### 14.2 开摄像头把显示搞死（蓝屏）的真根因：DW-GDMA 控制器级复位

CSI 走 `dw_gdma` 框架，DSI 走裸 LL。框架首次取 group handle 时执行：

```
dw_gdma_ll_enable_bus_clock();
dw_gdma_ll_reset_register();   ← 控制器级复位
dw_gdma_hal_init();            ← 内部还有一次 dw_gdma_ll_reset()
```

**DSI 用裸 LL 配好的 ch1 就这样被抹掉** → 面板失去同步变纯色。

**修法**（`dw_gdma.c` 的 `dw_gdma_acquire_group_handle`）：先看
`dw_gdma_ll_is_bus_clock_enabled()`——控制器已在跑时**不再复位**，只
`group->hal.dev = DW_GDMA_LL_GET_HW()` + 保持控制器/中断使能（与 DSI 自己的判据一致）。

### 14.3 真机验收（22/22）

```
[dm-check] csi:dsi-coexist   PASS camera=v4l2 frame=ok display distinct=102 grad=3
[dm-check] snapshot:picture  PASS distinct=102 dominant=54% grad=3 n=12642
DM-SELFTEST RESULT: PASS (22/22)
```

即**摄像头出帧的同时，显示仍在正常绘制** —— CSI/DSI 共存达成。
用户目视确认：面板正常中文界面 + 相机页有画面。

证据：`docs/40_验证与证据/截图与对照日志/应用层_真机快照/selftest_report_22of22_csi_dsi_coexist.txt`

## 十五、Agent 页：把 ai_agent 的能力嵌进 DeskMate（真机 25/25）

### 15.1 为什么不是"启用它的 LVGL UI"

`packages/ai_agent/src/ui/lvgl_ui_channel.c` 是一个**会自己初始化显示驱动 + 自建
lv_timer_handler 线程**的整屏 UI（圆屏聊天 + PTT）。直接启用
`CONFIG_AI_AGENT_LVGL_UI` 会和 DeskMate 的单线程 LVGL **抢屏**。

所以做法是：**只复用它的数据通道与算法，不启用它的 UI**。

### 15.2 页面内容

| 区域 | 内容 |
|---|---|
| 左 | 连接状态（`ws · 在线` / `ws · 离线` / `离线应答(mock)` / `off`）、收发计数、**最近一条真实回复**（`dm_agent_last_recv()`）、`重连` 按钮 |
| 右 | **配对二维码**（内容 `http://<本机IP>:28789/?chat=deskmate-ui`）+ 地址文本 + 使用提示 |

- 二维码用 **LVGL 的 `lv_qrcode`**（`lv_qrcode_create/set_size/update`），底层就是
  `qrcodegen`——与 ai_agent 的 `qrcode_display.c` 用的是**同一套编码算法**。
- 聊天本身仍走「对话」页（同一个 ws，`chat_id=deskmate-ui`），主动推送进顶栏 + 对话页。
- ws 连接是**非阻塞 + 后台重连**（见 §12），ai_agent 没起来也不会卡住 UI。

### 15.3 真机验收（25/25）

```
[dm-check] agent:bridge      PASS state=mock sent=0 recv=0
[dm-check] agent:page        PASS page=agent rc=0
[dm-check] agent:qr          PASS qr=1
[dm-check] csi:dsi-coexist   PASS camera=v4l2 frame=ok display distinct=256 grad=2
DM-SELFTEST RESULT: PASS (25/25)
```

说明：本轮验收用 `--agent mock` 跑（ai_agent 服务未启动，`state=mock` 是如实的）；
页面本身走的是真实 ws 桥接，启动 `ai_agent` 后 `state` 会变 `ws · 在线`，回复进
`最近回复` 与「对话」页。

### 15.4 三件事的总账

| 工作流 | 真机判据 | 状态 |
|---|---|---|
| CSI/DSI 共存 | `csi:dsi-coexist`（摄像头出帧 + 显示仍绘制） | ✅ |
| 类 PC 网络页 | `net:ipv4-valid` / `net:eth-static` / `net:ap-degraded` + WiFi 密码连接 | ✅（热点=平台限制，已降级并注明） |
| Agent 页 | `agent:page` / `agent:qr` / `agent:bridge` | ✅ |

## 十六、真机「蓝屏卡死」+ 日志噪声：根因与修复

### 16.1 根因（两个问题其实是同一个连环）

1. `src/ui/dm_page_home.c` 在**每次页面 update**（LVGL tick，30–50 Hz）里调 `dm_net_refresh()`；
   每次 refresh 都做 `getifaddrs()` + 逐接口 ioctl，并且打 3 行日志。
2. 控制台被灌满后，**应用自己的 `printf` 阻塞在 NSH 控制台上** → UI 线程停住：
   画面冻在某一帧（看着像"蓝屏"）、触摸无响应（"卡死"）。

顺带发现并修掉一个真 bug：`dm_net_refresh()` 里我用 `clock()` 做节流，编译产物在
`jal clock` 之后直接接了一条 `ebreak`（编译器把后续代码判成不可达），**一调用就
PANIC**（`riscv_exception: Breakpoint`，EPC 落在 `dm_net_refresh`）。
现在 refresh 只做**重入保护**，频率节流放在唯一调用点 `dm_ui_tick()`（1 s），不依赖 `clock()`。

### 16.2 修法

| 侧 | 改动 |
|---|---|
| 应用 | 页面不再各自 refresh，只读缓存 `dm_net_status()`；`dm_ui_tick()` 里**全局唯一**一处 1 s 节流刷新；`dm_net_refresh()` 加重入保护；`refresh step` 日志进 `DM_NET_TRACE`；`SIOCGIWNAME` 失败提示只打一次 |
| 平台 | 新增 Kconfig `CONFIG_ESPRESSIF_HOSTED_LOG_VERBOSE`（默认 **n**）：`port_trace`、`transport INT_RAW`、`read_block addr=`、`payload read`、`watchdog INT_RAW`、`write_block` 全部改为 `EH_SDIO_TRACE()`（默认不编译）；entry 的 bring-up 诊断（D1 测试、dhcp raw）改为 `EH_LOGV()`；保留版本/枚举/`transport ready`/DHCP 结果/错误 |

### 16.3 前后对比（真机实测）

| 指标 | 修前 | 修后 |
|---|---|---|
| 每事务日志（read_reg/read_block/payload/watchdog/transport） | 25 s 内 ~38 行 | **0 行** |
| 空闲 10 s 窗口 | 持续刷 | **1 行** |
| 启动+应用运行（60 s 窗口） | ~53 行 + 刷新风暴 | **18 行** |
| 应用启动 | **PANIC**（`_assert` ← `dm_net_refresh` 的 ebreak） | `DM-READY`，无 panic |

### 16.4 验收

```
DM-SELFTEST RESULT: PASS (25/25)      # 真机，--agent mock --camera
工具：deskmate --agent mock --camera --selftest --report /tmp/r2.txt
```

模拟器同样 **PASS (25/25)**。调试 SDIO 链路时打开
`CONFIG_ESPRESSIF_HOSTED_LOG_VERBOSE=y` 即可恢复全部 trace。

### 16.5 收口：DM_LOG 限速 + 扫描窗口实测

**DM_LOG 限速**（`src/dm_log.c`）：按来源（`__func__`）每秒最多一行，被丢掉的条数
附在下一行末尾（`(+N 条同源日志被限速)`）。这样即使以后某条路径真的刷屏，也只会
变成"每秒一行 + 计数"，不会再把控制台灌满把 UI 拖死；`DM_WARN`/`DM_ERR`/`DM_CHECK`
不受影响（脚本要匹配，且量小）。

**扫描窗口实测**（真机，10 秒窗口，窗口内确认发生了扫描）：

```
[dm] wifi: scan found 1 AP(s) on wlan0
[dm] wifi: scan worker done: found=1 err=0

总行数: 3     eh_host_port_sdio: 0     每事务级(read_reg/read_block/payload/watchdog/transport): 0
```

对比修前（25 秒窗口 ~38 行每事务日志）：现在是 **0**，总行数 3（目标 ≤60）。

**触摸判据**（同一份自检）：

```
[dm-check] touch:target                 PASS (133,459)
[dm-check] touch:delivered              PASS 0 -> 1
DM-SELFTEST RESULT: PASS (25/25)
```
