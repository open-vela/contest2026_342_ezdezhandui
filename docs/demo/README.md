# demo —— 2026-09-11 夜的串口原始日志快照（真机实测）

> 本目录是**可直接作为演示与评审证据的真机串口原始日志**，采集工具 `tools/test/board.py`
> （console = `/dev/ttyUSB0` @115200，已 assert DTR/RTS）。
> ⚠️ 这是 **9/11 的快照**（当时显示/触摸尚未打通、eth0 为静态 10.0.0.2）；
> **最新结果与证据**在仓库 `logs/verify-2026-09-17/`（LVGL 帧缓冲截图、摄像头实录、
> 「启动后失聪」现场与修复对照、端到端 LLM 对话）与 `docs/01_开发流程/STATUS.md`。
> 本目录说明重写于 2026-10-31。

## 本目录做什么

放**一次完整采集的原始日志快照**（引导 / 应用启动 / 运行时查询三段），保留原样不加工，供评审复现核对。
**不放**结论性分析（那在 `docs/01_开发流程/` 与 `docs/02_驱动实现/`）、不放最新证据（在 `logs/verify-2026-09-17/`）。

## 文件说明

| 文件 | 内容 | 什么时候读它 |
|---|---|---|
| [01_boot_mcuboot.log](01_boot_mcuboot.log) | 冷启动日志：MCUboot 二级引导 → flash XIP → NuttShell | 核验引导链与 XIP 是否生效；确认「双镜像 0x2000 + 0x20000」在真机上跑通 |
| [02_verify.log](02_verify.log) | ai_agent 启动全量日志（本轮采集，`rc=0` 里程碑 15 处） | 核验赛题②③的证据：Skill 装载、cron、WebSocket、联网 |
| [03_runtime.log](03_runtime.log) | NSH 运行时查询 13 条（uname/free/ls/ifconfig/lvgldemo/df/mount…） | 查 9/11 当时的资源与设备状态（PSRAM、`/dev` 节点、eth0 地址） |

## 复现命令

```bash
cd contest2026_342_ezdezhandui
tools/build_flash/usb_stable.sh                                  # 烧录（0x2000 引导 + 0x20000 应用）
python3 tools/test/board.py reset --save docs/demo/01_boot_mcuboot.log
tools/test/verify_aiagent.sh | tee docs/demo/02_verify.log
python3 tools/test/board.py run "uname -a" "free" "ls /dev" "ifconfig eth0" \
        --save docs/demo/03_runtime.log
```

## 关键证据摘录

### 1. MCUboot 二级引导（flash XIP 生效）

```
[esp32p4] [INF] br_image_off = 0x20000
[esp32p4] [INF] IRAM segment: start=0x20080, size=0x9488, vaddr=0x4ff40000
[esp32p4] [INF] Mapped IROM: vaddr=0x40000000 paddr=0x30000 len=0xe0000
NuttShell (NSH)
```

> `Mapped IROM ... paddr=0x30000` 即 16MB flash 的 XIP 映射 —— 这是 SRAM 静态占用
> 从约 460KB 降到 83KB（9/11 首测；9/17 复测 120.3KB）的关键，也是解锁 LVGL/摄像头的前提。

### 2. 自定义 Skill ×2（赛题②）

```
[skills] Installed built-in skill: /data/ai_agent/skills/center-assistant.md
[skills] Installed built-in skill: /data/ai_agent/skills/quick-note.md
[skills] Skills system ready (12 built-in)
```

### 3. 定时主动 + 服务启动（赛题③）

```
[cron] Cron started (0 jobs, interval 10s)
[ws] WebSocket server started on port 28789
[agent] Network connected: 10.0.0.2
```

### 4. 运行时环境

```
nsh> free
      total       used       free    maxused    maxfree  nused  nfree name
   33986336    2631504   31354832    2631880   30962360     89      4 Umem
nsh> ifconfig eth0
eth0  HWaddr e8:f6:0a:e3:a6:a5 at RUNNING mtu 576
      inet addr:10.0.0.2 DRaddr:10.0.0.1 Mask:255.255.255.0
```

## 本机采集限制（诚实说明）

开发机是 VMware 虚拟机 + USB-CP2102，串口在**日志突发期会丢失字节**（每秒千字节级输出时），
表现为个别行缺失或行内字符被 ANSI 片段劈开（如 `Cron start[ed`）。
`tools/test/verify_aiagent.sh` 已针对该现象做归一化处理，并把位于突发中段的里程碑降级为 WARN。
**这是采集链路的限制，不是固件行为**：同一现象在多轮采集中出现位置随机，且固件自身的里程碑计数与工具输出一致。

## 维护约定

- 本目录**只存原始日志**，不改写、不截断；需要解读请在 `docs/01_开发流程/` 里另开文档并引用本目录文件。
- 新的采集快照按日期归入 `logs/verify-<日期>/`；本目录作为 9/11 历史快照保留，不再追加文件。
- 跨目录引用一律写全路径，如 `docs/04_证据/验证截图_LVGL界面_1024x600.png`。
