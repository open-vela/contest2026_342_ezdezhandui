# 桌伴 DeskMate —— openvela × ESP32-P4X 智能视觉中控屏

## 一、作品简介

一块 7 英寸的 AI 视觉中控屏——它「能看见、会主动、会执行」：摄像头看到你进门就主动问候，
早上 8 点自动播报天气和日程，说"记一下明早 9 点开会"就记下并到点提醒，设备异常主动告警。
全程离线也能响应本地指令，联网后 LLM 加持。

基于 openvela（大赛分支 dev-ai-contest-2026）+ ESP32-P4X-C5-Function-EV-Board 构建，
双线并打：**新硬件平台适配**（ESP32-P4 官方待适配板，RISC-V 双核 400MHz + 32MB PSRAM +
RJ45 以太网 + 板载 MIC/SPK + MIPI-CSI/DSI）+ **AI 硬件产品创新**（ai_agent「能主动、会执行」）。

## 二、选题方向

AI 硬件产品创新 + 新硬件平台适配（双赛道）。

## 三、目录结构

```text
nuttx/                     # 移植文件树①：NuttX 内核 / arch / 驱动 / 链接脚本（评审直接可读）
board/esp32p4/             # 移植文件树②：板级源码 + defconfig（openvela vendor 风格）
app/apps/                  # 移植文件树③：apps 侧改动（camera 示例 / lvgl 端口 / netinit / mbedtls）
app/ai_agent/              # 移植文件树④：ai_agent 应用（含自定义 Skill 定义）
tools/                     # 同步与真机工具（export / manifest 校验 / revision 锁定 / 烧录 / console）
docs/                      # 开发规划 + 框架解读 + 复盘/测试 + 项目状态 + 踩坑笔记 #01~#07
  ├── 00_openvela系统框架解读.md   # ★ openvela 整体架构（repo/分层/配置/构建/启动）
  ├── 03_工程框架开发.md           # 工程形态 / manifest 机制 / 日常开发循环
  ├── 04_框架模块设计.md           # 本项目的模块设计
  ├── 05_功能闭环测试.md           # 五层测试体系与真机结果
  ├── 踩坑笔记_04_MCUboot与FlashXIP.md  # ★ RAM 执行 → flash XIP 迁移全过程
  └── 踩坑笔记_05~07               # 显示 / 触摸 GT911 / 摄像头 MIPI-CSI
logs/                      # AI Coding 日志（提交前持续导出）
```

## 四、运行方式

交付形态是**文件树 + repo manifest copyfile 自动映射**，评审侧**零手工步骤**：
仓根下 4 棵文件树保存全部改动，`contest2026_342_ezdezhandui.xml` 用 **325 条 `<copyfile>`**
在 `repo sync` 时把它们自动覆盖到工作区对应位置 ——

| 作品仓 | 工作区 |
|---|---|
| `nuttx/**` | `nuttx/**` |
| `board/esp32p4/**` | `vendor/espressif/boards/esp32p4/**` |
| `app/apps/**` | `apps/**` |
| `app/ai_agent/**` | `packages/ai_agent/**` |

清单与文件树的**严格一一对应**由 `tools/engineering/gen_manifest_copyfiles.py` 保证
（`--check` 校验，直接运行则再生）。校验会**直接拒绝**两类"manifest 表达不了、
只能靠部署脚本补"的东西：需要删除的上游文件清单、指向目录的软链。

> **本移植不删除任何上游文件**：`manifest copyfile` 只能复制、不能删除，一旦依赖删除
> 就必须让评审再跑一次部署脚本。凡遇上游旧文件与新驱动冲突，一律在移植源码里消化 ——
> 例如 `esp_timer_adapter.c` 用 `#include <esp_timer.h>`（尖括号）而不是引号形式，
> 避开上游遗留的同名旧头 `espressif/esp_timer.h`。
>
> 同理，交付物里**不放任何软链**：`<copyfile>` 的 src 会经 repo `_SafeExpandPath()` 逐段校验，
> 只要路径上出现软链就抛 `ManifestInvalidPathError: traversing symlinks not allow`
> （实测目录软链与文件软链**都**失败）；`<linkfile>` 也只能链到作品仓内的路径，
> 链不回工作区内的目标。原先 `vendor/…/esp32p4/common/board → …/esp32p4-function-ev-board/src`
> 那个软链只有 make 构建路径在用（该路径本身已不通，评审走 `--cmake`），已随本次改动删除。
>
> 因此评审只需 `repo sync` + `build.sh`，**不需要 deploy/rsync 之类的部署步骤**
> （详见 `docs/06` §二十四）。

```bash
# 1. 拉取（repo sync 自动应用全部改动）
#    ⚠️ 必须用 **GitHub** 官方仓：大赛仅在 GitHub 进行，Gitee 镜像停在旧版（无本作品改动）。
repo init -u https://github.com/open-vela/contest2026_342_ezdezhandui.git \
  -b dev-ai-contest-2026 -m contest2026_342_ezdezhandui.xml
repo sync -c -j8
# 可选自检：清单与文件树一致性（应输出"✅ 清单与文件树一致"）
python3 contest2026_342_ezdezhandui/tools/engineering/gen_manifest_copyfiles.py --check

# 2. 构建（**一条命令即产出两个镜像**；引导构建已挂进默认构建图）
cd <openvela 工作区根>
#   ⚠️ 参数必须是「配置目录的路径」，**不能**用 `esp32p4-function-ev-board:nsh` 这种短名：
#   短名要求板级目录位于 nuttx/boards/*/*/ 之下，而本移植按 openvela vendor 风格把板级
#   放在 vendor/espressif/boards/esp32p4/ 下（manifest 不含 <linkfile>，repo sync 不会生成软链）；
#   用短名会停在 `CMake Error: No config file found at`（2026-09-17 干净树实测）。
./build.sh vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/configs/nsh --cmake -j8
#   cmake_out/esp32p4-function-ev-board_nsh/nuttx.bin  ← 应用（MCUboot 签名，1,835,008 B）
#   nuttx/mcuboot-esp32p4.bin                         ← MCUboot 二级引导（24,672 B）
#   耗时实测（2026-09-18 干净树，本机）：
#     · 依赖首次拉取占大头：esp-hal-3rdparty ≈594MB + MCUboot 及其子模块 ≈1.6GB，视网络 30~60 分钟
#     · 依赖就绪后的 cmake/ninja 编译（2,426 步）：**06:32 (mm:ss)**
#   （旧文档写的"约 11 分钟"是依赖已缓存时的增量构建，不代表评审首次构建）

# 3. 烧录（⚠️ MCUboot 两镜像；旧的"单镜像写 0x2000"已失效）
cd contest2026_342_ezdezhandui
tools/build_flash/usb_stable.sh
#   0x2000  ← MCUboot 引导；0x20000 ← 应用（OTA_0 主槽）；两镜像均做 hash 校验

# 4. 看 console：/dev/ttyUSB0（CP2102，115200；打开时须 assert DTR/RTS）→ 出现 nsh>
python3 tools/test/board.py reset                 # 复位并抓启动日志
python3 tools/test/board.py run "free" "ai_agent"
```


## 五、作品功能

| 功能 | 实现 | 赛题点 |
|---|---|---|
| openvela 移植 ESP32-P4X-C5 | 板级+芯片移植（MCUboot 二级引导 + flash XIP：text+rodata 迁入 flash，**SRAM 静态占用 460KB → 120.3KB**/512KB）→ console/ETH/PSRAM | 适配赛道核心 |
| LLM 对话 | ai_agent + MiMo（`llm_router` 多后端）；**端到端已实测**：WS 28789 → 消息 → LLM → 真实回答 | ① Agent 上硬件 |
| 自定义 Skill ×2 | 中控助手 `center-assistant`、速记工单 `quick-note`（内置 Skill 表，开机写入 /data/ai_agent/skills/） | ② |
| 定时主动 | cron 真实运行（`cron_service`/`tool_cron` 编入，作业表移 PSRAM） | ③ |
| LVGL 触控 UI | 7" 1024×600 MIPI-DSI（EK79007）+ GT911 触摸，真机渲染已核实 | ① + 加分 |
| 事件主动（摄像头）| 🔶 **驱动链路已打通**（SC2336 识别 / CSI 2 lane + DMA 武装 / 零错误），但 MIPI 数据 lane 物理通路无数据（模组或排线），主动场景待硬件修复后闭环 —— 复测一条命令：`tools/test/camera_diag.sh` | ③ |

## 六、AI 开发记录

- 全流程 AI Coding，日志导出至 `logs/`（contest-log-collector）
- 开发过程使用 openvela 官方 AI 开发技能集（17 个，位于**工作区** `.claude/skills/`，属上游资产、不入本仓）；本项目自身的沉淀在 `docs/`

## 七、状态（2026-09-18 提交前 复验更新）

- ✅ **移植与引导**：`repo sync` 即得完整工作树（325 条 `<copyfile>` 自动落位，**无部署步骤**）；一次 `build.sh` 产出**双镜像**（应用 1,835,008 B + MCUboot 24,672 B，命令见 §四）
- ✅ **真机闭环**：`tools/build_flash/usb_stable.sh` 两镜像烧录 → MCUboot → `Mapped IROM` XIP 映射 → NSH → `ai_agent` P0→P6 全 rc=0
- ✅ **联网**：eth0 DHCP `192.168.1.105` RUNNING（主机同网段，ICMP 通）；DNS 可用
- ✅ **显示 + 触摸 + LVGL**：`/dev/fb0 1024x600 RGB565`（EK79007）、`/dev/input0`（GT911）；`lvgldemo` 真机渲染并 JTAG 帧缓冲导出核实（色数 1265/1266、梯度 <4）→ `docs/验证截图_LVGL界面_1024x600.png`
- ✅ **ai_agent 端到端对话**：WS 28789 → 消息 → LLM → 真实回答（证据 `logs/verify-2026-09-17/agent_llm_ws.txt`）；「启动后整机失聪」已定位并修复（根因见 `docs/06` §26.7）
- ✅ **自定义 Skill ×2 已上机**（`Skills system ready (12 built-in)`，含 center-assistant / quick-note）；cron 真实启动；PSRAM 约 33.9 MB 可用
- 🔶 **摄像头**：驱动与软件链路零错误（SC2336 识别 `0xcb3a`、CSI 2 lane + DMA 武装），但 MIPI 数据 lane 物理通路无数据（模组/排线），出帧待硬件修复 —— 复测一条命令 `tools/test/camera_diag.sh`
- ✅ **随提交材料**（`提交材料/`）：演示视频（`6360b48….mp4`，27.7s）+ 9/18 真机复现证据日志 5 份（boot_nsh / aiagent_milestones / aiagent_live / lvgldemo / ws_llm_endtoend）；专属仓 `contest2026_342_ezdezhandui`
- 📌 详细状态与逐项证据见 `docs/STATUS.md`、`docs/05_功能闭环测试.md` §七、`docs/06_开发过程复盘与改进清单.md`

