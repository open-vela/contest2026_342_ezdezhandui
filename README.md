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
tools/                     # 工具集 —— 按用途分三类，索引见 tools/README.md
  ├── engineering/         #   📐 工程相关：export / manifest 校验 / revision 锁定 / 日志转换
  ├── build_flash/         #   🔥 构建烧录：MCUboot 两镜像烧录 / 一键演示 / USB 说明
  └── test/                #   🔬 测试取证：串口 harness / 里程碑断言 / 摄像头·失聪诊断 / WS 对话 / 视频合成
docs/                      # 规划 + 框架 + 测试 + 复盘 + 状态 + 踩坑笔记 #01~#07 + 真机证据
  ├── 00_openvela系统框架解读.md   # ★ openvela 整体架构（repo/分层/配置/构建/启动）
  ├── 01_开发规划文档.md           # 需求 / 差距分析 / 评分对照 / 收尾计划
  ├── 03_工程框架开发.md           # 工程形态 / manifest 机制 / 日常开发循环
  ├── 04_框架模块设计.md           # 本项目的模块设计
  ├── 05_功能闭环测试.md           # 五层测试体系与真机结果
  ├── 07_作品介绍.md               # ★ 作品介绍（做什么 / 输出什么 / 问题与解法，诚实记录）
  ├── STATUS.md                   # ★ 当前状态 / 逐项证据 / 已知限制（最常看）
  └── 踩坑笔记_01~07               # 环境 / 移植 / 烧录 / MCUboot+XIP / 显示 / 触摸 / 摄像头
logs/                      # AI Coding 日志（28 会话 / 35,831 事件，提交前持续导出）
提交材料/                  # 演示视频 + 真机证据日志（随作品提交）
```

> `repo sync` 后，工作区还会出现 `.agents/`：由 manifest 登记的自有 AI 开发技能仓
> `openvela-skills`（19 个 skills）自动拉取落位（见 §六）。它位于工作区、**不入本仓文件树**。

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

# 5.（可选）一键演示与复现
tools/test/verify_aiagent.sh                                   # P0→P6 + 12 skills + eth0 + cron 断言
python3 tools/test/ws_demo.py --host <板IP> --port 28789 \
        --send "只回复：你好" --wait 280                        # LLM 端到端对话复现
tools/test/camera_diag.sh                                      # 摄像头链路一次性判定
```

## 五、作品功能

| 功能 | 实现 | 赛题点 |
|---|---|---|
| openvela 移植 ESP32-P4X-C5 | 板级+芯片移植（MCUboot 二级引导 + flash XIP：text+rodata 迁入 flash，**SRAM 静态占用 460KB → 120.3KB**/512KB）→ console/ETH/PSRAM | 适配赛道核心 |
| LLM 对话 | ai_agent + MiMo（`llm_router` 多后端）；**端到端已实测**：WS 28789 → 消息 → LLM → 真实回答（证据 `提交材料/ws_llm_endtoend.log`） | ① Agent 上硬件 |
| 自定义 Skill ×2 | 中控助手 `center-assistant`、速记工单 `quick-note`（内置 Skill 表，开机写入 /data/ai_agent/skills/） | ② |
| 定时主动 | cron 真实运行（`cron_service`/`tool_cron` 编入，作业表移 PSRAM） | ③ |
| LVGL 触控 UI | 7" 1024×600 MIPI-DSI（EK79007）+ GT911 触摸，真机渲染已核实（证据 `docs/验证截图_LVGL界面_1024x600.png`） | ① + 加分 |
| 事件主动（摄像头）| 🔶 **驱动链路已打通**（SC2336 识别 / CSI 2 lane + DMA 武装 / 零错误），但 MIPI 数据 lane 物理通路无数据（模组或排线），主动场景待硬件修复后闭环 —— 复测一条命令：`tools/test/camera_diag.sh` | ③ |

> 逐项真机证据与根因见 `docs/STATUS.md`；测试体系与结果见 `docs/05_功能闭环测试.md`。

## 六、AI 开发记录

- **全流程 AI Coding**：本作品从移植到驱动调试再到文档，均由 AI 辅助完成，
  对话日志导出至 `logs/`（claude-code + dsh 双工具，**28 会话 / 35,831 事件**，
  汇总见 `logs/ez-xu/manifest.json`）；DSH 会话经 `tools/engineering/dsh_session_to_contest_log.py`
  转换为赛事日志格式。
- **AI 开发技能**：开发期使用 openvela 官方 AI 开发技能集（17 个，位于**工作区**
  `.claude/skills/`，属上游资产、不入本仓）；同时 manifest 登记自有技能仓
  `ez-xu/openvela-skills` → **工作区 `.agents/`**（19 个 skills，`repo sync` 自动拉取，
  评审可直接阅读 AI 开发技能资产）。本项目自身的工程沉淀在 `docs/`。

## 七、状态（2026-09-18 提交前 复验更新）

- ✅ **移植与引导**：`repo sync` 即得完整工作树（325 条 `<copyfile>` 自动落位，**无部署步骤**）；一次 `build.sh` 产出**双镜像**（应用 1,835,008 B + MCUboot 24,672 B，命令见 §四）
- ✅ **真机闭环**：`tools/build_flash/usb_stable.sh` 两镜像烧录 → MCUboot → `Mapped IROM` XIP 映射 → NSH → `ai_agent` P0→P6 全 rc=0
- ✅ **联网**：eth0 DHCP `192.168.1.105` RUNNING（主机同网段，ICMP 通）；DNS 可用
- ✅ **显示 + 触摸 + LVGL**：`/dev/fb0 1024x600 RGB565`（EK79007）、`/dev/input0`（GT911）；`lvgldemo` 真机渲染并 JTAG 帧缓冲导出核实（色数 1265/1266、梯度 <4）→ `docs/验证截图_LVGL界面_1024x600.png`
- ✅ **ai_agent 端到端对话**：WS 28789 → 消息 → LLM → 真实回答（证据 `提交材料/ws_llm_endtoend.log`）；「启动后整机失聪」已定位并修复（根因见 `docs/06` §26.7）
- ✅ **自定义 Skill ×2 已上机**（`Skills system ready (12 built-in)`，含 center-assistant / quick-note）；cron 真实启动；PSRAM 约 33.9 MB 可用
- 🔶 **摄像头**：驱动与软件链路零错误（SC2336 识别 `0xcb3a`、CSI 2 lane + DMA 武装），但 MIPI 数据 lane 物理通路无数据（模组/排线），出帧待硬件修复 —— 复测一条命令 `tools/test/camera_diag.sh`
- ✅ **提交与仓库**：全部提交经 fork 汇入 **PR #12**（https://github.com/open-vela/contest2026_342_ezdezhandui/pull/12 ，MERGEABLE）；提交材料清单见 §八
- 📌 详细状态与逐项证据见 `docs/STATUS.md`、`docs/05_功能闭环测试.md` §七、`docs/06_开发过程复盘与改进清单.md`

## 八、提交材料

| 交付项 | 位置 | 说明 |
|---|---|---|
| 作品代码 | 4 棵文件树 + manifest **325 条 copyfile** | 评审零手工步骤复现（§四） |
| 演示视频 | `提交材料/6360b48f….mp4` | **27.7s · HEVC(H.265) · 720×1280 竖屏 · 含音轨**，板端真机演示录像（启动 / 对话 / 显示触控等实机画面） |
| 真机证据日志 ×5 | `提交材料/*.log` | `boot_nsh` / `aiagent_milestones` / `aiagent_live` / `lvgldemo` / `ws_llm_endtoend` |
| AI Coding 日志 | `logs/ez-xu/` | 28 会话 / 35,831 事件（claude-code + dsh） |
| 文档 | `docs/` + `README.md` + `tools/README.md` | 规划 / 框架 / 测试 / 复盘 / 状态 / 踩坑笔记 |
| AI 开发技能资产 | manifest → 工作区 `.agents/` | `openvela-skills`（19 个 skills），`repo sync` 自动拉取 |
| 专属仓地址 | `https://github.com/open-vela/contest2026_342_ezdezhandui` | 大赛仅在 GitHub，提交经 fork + PR（当前 #12） |
| 《作品介绍》 | `docs/07_作品介绍.md` | 源稿已入仓（做了什么 / 输出什么 / 问题与解法，含未完成项如实记录）；docx/pdf 由本稿导出 |

**评分对照**（评分规则 30/20/20/10/10/10，详见 `docs/01` §四）：

| 评分项 | 本作品对应 |
|---|---|
| 技术难度 30 | ESP32-P4X 新平台移植 + MCUboot 二级引导 + flash XIP + **5 个平台级驱动缺陷修复**（板级 Kconfig 静默失效 / UART 丢字节 / GT911 / V4L2 / risc-v 任务退出失聪，见 `docs/06`） |
| 产品创新性 20 | 桌伴「能主动、会执行」产品叙事：ai_agent + 自定义 Skill ×2 + cron 定时主动 |
| 项目完整度 20 | 代码 + 可复现 Demo + 文档齐备（本表）；免部署复现路径；真机证据逐项归档 |
| AI 开发 10 | 全流程 AI Coding，28 会话日志 + `.agents/` 技能资产 |
| 商业潜力 10 | 桌面陪伴/中控真实场景；**离线可用**设计（断网仍有表情与本地响应） |
| 展示效果 10 | 演示视频 + 《作品介绍》 + 答辩材料（§四复现路径可现场走一遍） |

## 九、已知限制与安全（诚实记录）

1. **摄像头出帧**：驱动与软件链路零错误（SC2336 识别 / CSI 2 lane + DMA / 零错误），
   但 MIPI 数据 lane **物理通路无数据**（模组或排线，JTAG 诊断定论，见 `docs/06` §25 与
   `docs/STATUS.md` §四.4）。更换/重插模组后一条命令复测：`tools/test/camera_diag.sh`（输出 `MOVING` 即出帧）。
2. **LLM 问答延迟**：一次端到端问答实测约 **261s**（推理模型 + 15KB 工具 schema 的固有开销），
   体验偏慢但非功能缺陷；已在 `docs/06` §27 如实记录。
3. ⚠️ **MiMo API key 安全**：该 key 曾两次误入公开仓历史（commit `4d7f318`、日志文件），
   已在 `logs/` 中脱敏并移除，但**必须到平台侧轮换/吊销**才算处置完成（仅从仓库删除不能使其失效）。
   新 key 一律经**工作区** `packages/ai_agent/include/agent_secrets.h`（`.gitignore` 忽略、不入仓）注入，
   注入后需重新构建烧录。提交前对 `logs/` 做凭据扫描：
   `grep -rEo 'tp-[A-Za-z0-9]{40,}|sk-[A-Za-z0-9]{20,}' logs/`
4. **本机开发环境的两处坑**（评审全新 clone 不受影响，详见 `docs/03` §3.2、`docs/STATUS.md` §四.8）：
   ① 本地 `.repo/manifests` 与 gitee 分叉，`repo sync` 需带 `--no-manifest-update`；
   ② 本地曾遗留一个交互式 rebase 半成品，与交付物无关。
