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
board/esp32p4_vela/        # ESP32-P4 平台移植（文件树 nuttx+apps+ai_agent + 构建/烧录说明）
docs/                      # 开发规划 V5.1 + 踩坑笔记 #01/#04/#05 + 状态交接
  ├── 10_openvela系统框架解读.md   # ★ openvela 整体架构（repo/分层/配置/构建/启动）
  ├── 03_框架模块设计.md           # 本项目的模块设计
  ├── 04_功能闭环测试.md           # 五层测试体系与真机结果
  └── 09_问题处理方案.md           # 未决问题的处理方案
.claude/skills/            # openvela 官方 AI 开发技能集（17 个，AI Coding 资产）
logs/                      # AI Coding 日志（提交前持续导出）
```

## 四、运行方式

移植采用**文件树 + repo manifest copyfile 自动映射**：作品仓 `board/esp32p4_vela/` 保存所有
改动文件，`contest2026_342_ezdezhandui.xml` 通过 **324 条 `<copyfile>`**
（nuttx 305 + apps 4 + packages/ai_agent 14 + 仓根文档 1）在 `repo sync` 时把改动自动覆盖到
工作区对应路径（评审零手工拷贝）。清单与文件树的一一对应由
`tools/gen_manifest_copyfiles.py` 保证（`--check` 校验，直接运行则再生）。
被删除的 8 个上游文件见 `board/esp32p4_vela/nuttx/.deleted-files`。

```bash
# 1. 环境（repo；esptool 软链见 board/esp32p4_vela/README.md）
#    注意：本仓的 PR 需先合入（或评审从含 PR 的 fork 分支拉取），否则 gitee 官方分支是旧 manifest。
repo init -u https://gitee.com/open-vela/contest2026_342_ezdezhandui.git \
  -b dev-ai-contest-2026 -m contest2026_342_ezdezhandui.xml
repo sync -c -j8     # copyfile 自动应用全部改动 + 生成软链工具

# 2. 处理被删除的上游文件（deploy 脚本按 .deleted-files 删除）
cd contest2026_342_ezdezhandui/board/esp32p4_vela
./deploy.sh <openvela 工作区根>   # 幂等 rsync + 按 .deleted-files 删除；不带参数默认 ./(pwd) 为其根
#   示例：若工作区根是 /path/to/openvela，则 ./deploy.sh /path/to/openvela

# 3. 构建（**一条命令即产出两个镜像**，2026-09-11 夜起；引导构建已挂进默认构建图）
cd <openvela 工作区根>
./build.sh nuttx/boards/risc-v/esp32p4/esp32p4-function-ev-board/configs/nsh/ --cmake -j8
#   cmake_out/esp32p4-function-ev-board_nsh/nuttx.bin  ← 应用（MCUboot 签名，1,835,008 B）
#   nuttx/mcuboot-esp32p4.bin                         ← MCUboot 二级引导（24,640 B）
#   干净树实测：10 分 39 秒（含 HAL clone + MCUboot ExternalProject + 2410 编译步）

# 4. 烧录（⚠️ MCUboot 两镜像；旧的"单镜像写 0x2000"已失效）
cd contest2026_342_ezdezhandui
tools/usb_stable.sh
#   0x2000  ← MCUboot 引导；0x20000 ← 应用（OTA_0 主槽）；两镜像均做 hash 校验

# 5. 看 console：/dev/ttyUSB0（CP2102，115200；打开时须 assert DTR/RTS）→ 出现 nsh>
python3 tools/board.py reset                 # 复位并抓启动日志
python3 tools/board.py run "free" "ai_agent"
```


## 五、作品功能

| 功能 | 实现 | 赛题点 |
|---|---|---|
| openvela 移植 ESP32-P4X-C5 | 板级+芯片移植（MCUboot 二级引导 + flash XIP，SRAM 460KB→83KB）→ console/ETH/PSRAM | 适配赛道核心 |
| LLM 对话 | ai_agent + MiMo（`llm_router` 多后端；链路已验证，**key 需有效额度**） | ① Agent 上硬件 |
| 自定义 Skill ×2 | 中控助手 `center-assistant`、速记工单 `quick-note`（内置 Skill 表，开机写入 /data/ai_agent/skills/） | ② |
| 定时主动 | cron 真实运行（`cron_service`/`tool_cron` 编入，作业表移 PSRAM） | ③ |
| 事件主动（摄像头） | MIPI-CSI + SC2336 → 视觉检测 → 主动问候/告警 | ③ |
| LVGL 触控 UI | 7" 1024×600 MIPI-DSI（EK79007）+ GT911 触摸 | ① + 加分 |

## 六、AI 开发记录

- 全流程 AI Coding，日志导出至 `logs/`（contest-log-collector）
- 沉淀：openvela 官方 17 个开发技能（.claude/skills/）+ 本仓文档（docs/）

## 七、状态（2026-09-11 夜 复验更新）

- ✅ esp32p4 移植编译通过；**MCUboot 二级引导 + flash XIP 打通**（SRAM 占用 460KB→83KB）
- ✅ 干净树一次 `build.sh` 产出**双镜像**（本轮修复 `bootloader` 目标未入 `all` + 内层 ninja 生成器丢失）
- ✅ 真机闭环：`tools/usb_stable.sh`（两镜像烧录）→ MCUboot → `Mapped IROM` XIP 映射 → NSH → `ai_agent` P0→P6 全 rc=0
- ✅ 真机验证：eth0 10.0.0.2（主机 ping 0% 丢包）、PSRAM 33.9MB、cron 真实启动、12 skills
- ✅ **自定义 Skill ×2 已上机**（`Skills system ready (12 built-in)`，含 center-assistant / quick-note）
- 🔶 摄像头：SC2336 识别通过（`chip ID: 0xcb3a` → `/dev/video0`），出帧待接 ISP（`VIDIOC_S_FMT` EINVAL）
- ❌ 显示：EK79007 无应答 → **无 `/dev/fb0`**（I2C0 上 SC2336 正常，疑模组供电/FPC/J1→J6 跳线）
- ❌ DNS 不可用（无 `/etc/resolv.conf`）；WebSocket 28789 板端已监听、主机侧未打通（非同一 L2）
- ⏳ 演示视频 + 《作品介绍》随提交材料
- 📌 详细状态与差距见 `docs/STATUS.md`、`docs/04_功能闭环测试.md` §七、`docs/05_开发过程复盘与改进清单.md`

