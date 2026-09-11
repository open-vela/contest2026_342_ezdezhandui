# 演示证据包（真机实测，2026-09-11）

> 本目录是**真机串口原始日志**，可直接作为演示与评审证据。
> 采集方式：`tools/board.py`（console = `/dev/ttyUSB0` @115200，已 assert DTR/RTS）。

## 文件说明

| 文件 | 内容 | 看点 |
|---|---|---|
| `01_boot_mcuboot.log` | 冷启动日志 | **MCUboot 二级引导** → `Mapped IROM vaddr=0x40000000`（flash XIP 执行）→ `NuttShell (NSH)`；随后各外设 bringup |
| `02_verify.log` | `tools/verify_aiagent.sh` 多轮断言 | ai_agent 启动里程碑 + 12 skills + eth0 + cron 全部 ✓ |
| `03_runtime.log` | NSH 运行时查询 | `free`（PSRAM 33.9MB）、`ls /dev`（video0/input0）、`ifconfig eth0`（10.0.0.2 RUNNING）、`uname -a` |

## 复现命令

```bash
cd contest2026_342_ezdezhandui
tools/usb_stable.sh                                  # 烧录（0x2000 引导 + 0x20000 应用）
python3 tools/board.py reset --save docs/demo/01_boot_mcuboot.log
tools/verify_aiagent.sh | tee docs/demo/02_verify.log
python3 tools/board.py run "uname -a" "free" "ls /dev" "ifconfig eth0" \
        --save docs/demo/03_runtime.log
```

## 关键证据摘录

### 1. MCUboot 二级引导（flash XIP 生效）

```
[esp32p4] [INF] br_image_off = 0x20000
[esp32p4] [INF] Loading image 0 - slot 0 from flash, area id: 1
[esp32p4] [INF] IRAM segment: start=0x20080, size=0x9488, vaddr=0x4ff40000
[esp32p4] [INF] Mapped IROM: vaddr=0x40000000 paddr=0x30000 len=0xe0000
NuttShell (NSH)
nsh>
```
> `Mapped IROM ... paddr=0x30000` 即 16MB flash 的 XIP 映射 —— 这是 SRAM 占用
> 从 460KB 降到 83KB、从而解锁 LVGL/摄像头等大功能的关键。

### 2. 自定义 Skill ×2（赛题②）

```
[skills] Installed built-in skill: /data/ai_agent/skills/center-assistant.md
[skills] Installed built-in skill: /data/ai_agent/skills/quick-note.md
[skills] Skills system ready (12 built-in)
```

### 3. 定时主动服务（赛题③）

```
[cron] No cron file, starting fresh
[cron] Cron started (0 jobs, interval 10s)
```

### 4. 摄像头（MIPI-CSI + SC2336）

```
SC2336 chip ID: 0xcb3a                ← SCCB 通信成功，模块已识别
Camera registered on /dev/video0
```

### 5. 运行时环境

```
nsh> free
      total       used       free    maxused    maxfree  nused  nfree name
   33986336    2631504   31354832    2631880   30962360     89      4 Umem
nsh> ls /dev
 console  input0  null  random  ttyS0  video0  zero
nsh> ifconfig eth0
eth0  HWaddr e8:f6:0a:e3:a6:a5 at RUNNING mtu 576
      inet addr:10.0.0.2 DRaddr:10.0.0.1 Mask:255.255.255.0
```

## 本机采集限制（诚实说明）

开发机是 VMware 虚拟机 + USB-CP2102，串口在**日志突发期会丢失字节**（约每秒千字节级输出时），
表现为个别行缺失或行内字符被 ANSI 片段劈开（如 `Cron start[ed`）。
`tools/verify_aiagent.sh` 已针对该现象做归一化处理，并把位于突发中段的里程碑降级为 WARN。
**这是采集链路的限制，不是固件行为**：同一现象在多轮采集中出现位置随机，且固件自身的
里程碑计数与工具输出一致。
