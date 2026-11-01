# DeskMate —— openvela 中控屏应用层

一块 7″ 触控中控屏的应用层：把 **摄像头 / WiFi / 网口 / 触摸屏** 四条板载能力收进
一个 LVGL 中控界面，并接上 `ai_agent`（LLM 对话 + 主动事件）。同一套代码可以在
**ESP32-P4 真机**、**NuttX 模拟器（宿主二进制）** 与 **openvela goldfish 官方模拟器**
上运行；模拟器侧是**无头可脚本化**的，一条命令跑完 UI 与逻辑并产出 PNG 证据。

## 一、五个页面

| 页面 | 用到的板载能力 | 内容 |
|---|---|---|
| Home | 时钟 / 触摸 | 构建与显示身份（`/dev/fb0` 分辨率、后端）、四个能力状态（Camera / Network / Agent）、触摸自检按钮 |
| Camera | **摄像头** `/dev/video0`（SC2336 → MIPI-CSI → V4L2） | 实时预览（2×2 去马赛克 → RGB565）、帧计数、**帧差运动量**、抓拍落盘、「让 agent 看一下」 |
| Chat | agent bridge | 气泡对话（预设短语 + 软键盘）、LLM 回复、**主动事件**消息 |
| Network | **网口** eth0 + **WiFi** wlan0（WAPI） | 接口列表（up/running/IP/掩码）、WiFi 关联状态、**扫描列表 + 一键连接** |
| Settings | 显示 | agent 端点、手动快照（证据路径）、主动事件演练、关于 |

## 二、架构

```
deskmate_main.c        NSH 内置命令 deskmate（参数解析 / 单线程 UI 循环 / 自检驱动）
src/hal/               ← 硬件抽象层（真机与模拟器同一接口）
  dm_display.[ch]        fbdev 后端（/dev/fb0 + lv_nuttx）/ 内存后端（无头）；快照 + 画面统计
  dm_touch.[ch]          脚本指针 indev（无输入设备也能驱动 UI；也是自检的点击来源）
  dm_camera.[ch]         v4l2 后端（RAW8/RGB565/RGB888）/ 合成后端；2×2 去马赛克 + 缩放
  dm_net.[ch]            接口状态（getifaddrs + SIOCGIFFLAGS）；WiFi（SIOCGIW* / WAPI）
  dm_audio.[ch]          （预留）TTS 播报
src/core/              ← 事件与状态（时钟 / 网络 / 相机 / agent）
src/agent/dm_agent.[ch]  agent bridge：mock（离线应答）/ ws（ai_agent 28789）/ off
src/ui/                ← 主题、控件、五个页面、路由（状态栏 + 分页 + Tab 栏）
src/dm_selftest.[ch]   ← 无头自检场景（页面导航 + 触摸 + 快照 + 画面统计）
src/ui/dm_i18n.h       ← 全部文案（字体子集从这里抽字形）
```

**线程模型**：单线程。LVGL、widget 树、HAL 轮询、agent 轮询全在 `deskmate` 的
主循环里，不存在跨线程操作 widget 的问题。

**agent 协议**（与 `tools/test/ws_demo.py` 一致，`chat_id="deskmate-ui"`）：

```
-> {"type":"message","chat_id":"deskmate-ui","content":"..."}
<- {"type":"response","chat_id":"deskmate-ui","content":"..."}
```

所以 agent 侧既可以直接回话，也可以用 `ws_server_send("deskmate-ui", text)`
把**主动消息**推进 UI。

## 三、构建与运行

### 3.1 真机（ESP32-P4）

板级配置已包含本应用（`CONFIG_EXAMPLES_DESKMATE=y`）：

```bash
cd <openvela 工作区根>
./build.sh vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/configs/nsh --cmake -j8
cd contest2026_342_ezdezhandui && tools/build_flash/usb_stable.sh     # 两镜像烧录
python3 tools/test/board.py run "deskmate"
```

启动后：`/dev/fb0` 1024×600 RGB565 出界面，`/dev/input0`（GT911）响应触摸；
摄像头页走 `/dev/video0`；网络页读 eth0 / wlan0。

### 3.2 NuttX 模拟器（宿主二进制，无头）

```bash
cd contest2026_342_ezdezhandui
tools/test/sim_deskmate.sh            # 配置 + 构建 + 运行 + 断言 + 归档证据
tools/test/sim_deskmate.sh --no-build # 复用已有构建
```

产物在 `logs/sim-deskmate/<时间戳>/`：`console.log`、`dm_home.png`、`dm_camera.png`、
`dm_net.png`、`result.txt`。判定行是 `RESULT: PASS`。

配置本身在 `nuttx/boards/sim/sim/sim/configs/deskmate/defconfig`，要点：

- 无 X11：显示走**内存后端**（应用自持一块 1024×600 RGB565 帧缓冲，快照即真相）；
- 触摸：`SIM_TOUCHSCREEN` 需要 X11，所以默认用**脚本注入**（`dm_touch_tap`），
  这也是自检点击按钮的方式；
- 网口：`SIM_NETDEV`（TAP）；WiFi：`DRIVERS_WIFI_SIM` + `SIM_WIFIDEV_PSEUDO`，
  BSS 表由应用启动时写 `/host/wifi/bss`（hostfs，guest 根文件系统不可写）；
- 快照经 `FS_HOSTFS` 落到宿主目录，再用 `tools/test/fb2png.py` 转 PNG。

### 3.3 goldfish 官方模拟器（验收轨）

```bash
tools/test/emulator_deskmate.sh       # 构建 goldfish-arm64-v8a-ap 并跑 deskmate
```

配置在 `board/vela/configs/goldfish-arm64-v8a-ap/defconfig`（manifest 映射到
`vendor/openvela/boards/vela/`）。该配置里相机（goldfish camera）、显示
（goldfish GPU fb）、触摸（goldfish events）、网口（virtio-net）、WiFi
（wifi_sim）都是现成的。

## 四、自检（证据生成器）

`deskmate --selftest` 在 UI 线程上跑一个状态机，逐步断言并打印
`[dm-check] <项> PASS/FAIL`，最后一行是 `DM-SELFTEST RESULT: PASS (n/n)`：

| 检查 | 证明什么 |
|---|---|
| `display:ready` | 显示后端就绪（分辨率 / 位深 / 后端） |
| `lvgl:version` | LVGL 主版本匹配 |
| `ui:built` / `ui:home` | 页面树建起来、停在首页 |
| `touch:target` / `touch:delivered` | 注入点击命中**真实按钮**并更新了模型（不是画给眼睛看的静态图） |
| `snapshot:written` | 每页快照写入成功且字节数与几何一致 |
| `ui:tab-target` / `ui:camera-page` / `ui:net-page` | **点 Tab 切页**真的切过去了 |
| `ui:page-rendered` | 切换前后帧指纹不同（页面内容真的变了） |
| `net:interfaces` | 网口状态可读（模拟器里是 `eth0 UP <ip>`） |
| `snapshot:picture` | 画面是"画出来的 UI"而不是黑屏/噪声：色数、主色占比、平均亮度梯度（与 `fb2png.py` 同一套判据） |

## 五、已知边界（诚实记录）

- **WiFi**：应用层通过 WAPI 做扫描/连接/状态；板载 ESP-Hosted 的 SDIO 链路属平台
  侧工作，不在本应用层范围内。模拟器里 `wifi_sim` 提供伪无线设备（上游
  `wifi_sim.c` 少一个 `<nuttx/mutex.h>` 引用，本项目顺手修了，否则 flat 构建链接不过）。
- **运动检测**：是**帧差**，不是人形/人脸识别。它驱动"主动事件"叙事，文档不夸大。
- **CJK 字体**：当前 UI 用 ASCII 文案（`lv_font_montserrat_*`）。中文文案集中在
  `src/ui/dm_i18n.h`，配 `tools/engineering/gen_dm_font.sh` 生成的子集字体即可启用；
  离线回退开关 `EXAMPLES_DESKMATE_FONT_SIMSUN`（LVGL 内置 SimSun 16，约 149 KB）。
- **模拟器的相机**：宿主没有 `/dev/video0` 时用合成动态源（画面里能看到移动方块），
  页面与自检都明确标注 `synthetic`。
