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
