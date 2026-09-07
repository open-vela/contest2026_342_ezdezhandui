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
board/esp32p4_vela/        # ESP32-P4 平台移植（文件树 nuttx+apps + 构建/烧录说明）
docs/                      # 开发规划 V5.1 + 踩坑笔记 #01/#04/#05 + 状态交接
.claude/skills/            # openvela 官方 AI 开发技能集（17 个，AI Coding 资产）
logs/                      # AI Coding 日志（提交前持续导出）
```

## 四、运行方式

移植采用**文件树 + repo manifest copyfile 自动映射**：作品仓 `board/esp32p4_vela/` 保存所有
改动文件（nuttx 281 + apps 3），`contest2026_342_ezdezhandui.xml` 通过 284 条 `<copyfile>`
在 `repo sync` 时把改动自动覆盖到工作区 nuttx/apps 对应路径（评审零手工拷贝）。
了解实验内那 8 个被删除的上游文件见 `board/esp32p4_vela/nuttx/.deleted-files`。

```bash
# 1. 环境（repo；esptool 软链见 board/esp32p4_vela/README.md）
repo init -u https://gitee.com/open-vela/contest2026_342_ezdezhandui.git \
  -b dev-ai-contest-2026 -m contest2026_342_ezdezhandui.xml
repo sync -c -j8     # copyfile 自动应用全部改动 + 生成软链工具

# 2. 处理被删除的上游文件（deploy 脚本自动删除）
cd contest2026_342_ezdezhandui/board/esp32p4_vela
./deploy.sh ../../nuttx   # 幂等 rsync + 按 .deleted-files 删除

# 3. 构建
cd ../../..
./build.sh nuttx/boards/risc-v/esp32p4/esp32p4-function-ev-board/configs/nsh/ --cmake -j8

# 4. 启动：console 在 USB Serial/JTAG 口（ttyACM0, 115200），出现 nsh> 提示符
```

## 五、作品功能

| 功能 | 实现 | 赛题点 |
|---|---|---|
| openvela 移植 ESP32-P4 | 板级+芯片移植（补丁见 board/）→ 串口 console 启动 | 适配赛道核心 |
| LLM 对话 | ai_agent + MiMo（goldfish 验证：`ask 现在几点了` 内置工具链路工作） | ① Agent 上硬件 |
| 自定义 Skill ×2 | 中控助手、速记工单（/data/agent/skills/） | ② |
| 事件主动（摄像头） | MIPI-CSI 2MP 人形检测 → 主动问候/告警 | ③ |
| 定时主动 | cron_add + weather/daily-briefing + TTS | ③ |

## 六、AI 开发记录

- 全流程 AI Coding，日志导出至 `logs/`（contest-log-collector）
- 沉淀：openvela 官方 17 个开发技能（.claude/skills/）+ 本仓文档（docs/）

## 七、状态

- ✅ esp32p4 移植编译通过，ram-only 镜像生成（nuttx.bin 269KB）
- ⏳ 真机 JTAG 烧录验证（详见 docs/STATUS.md）
