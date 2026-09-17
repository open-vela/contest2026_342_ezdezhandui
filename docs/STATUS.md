# 项目状态（STATUS）

> 最后更新：**2026-09-17** · §四.1 用 JTAG 现场取证给出「agent 一启动整机失聪」的定论
> （节拍中断投递失效，非忙等/非输入通道问题，证据 `logs/verify-2026-09-17/wedge_jtag.txt`）；
> §四.4 摄像头按最终固件复测写入定论；LVGL 截图在最终固件上复采（同一工具 `tools/fb2png.py`）；
> §二/§三 收拢到**无部署步骤**的交付路径（manifest 324 条 copyfile，见 `docs/05` §24）。
> §五 外设状态仍按 9/14–9/15 的**平台级驱动修复**（显示/触摸/DNS 已打通，见 `docs/05` §22/§23）；
> 逐项结果见 `docs/04_功能闭环测试.md` §七；收尾计划见 `docs/01_开发规划文档.md` §十。

## 一、主线达成状态（真机闭环验证）

| 里程碑 | 状态 | 证据 |
|---|---|---|
| ESP32-P4X 平台移植 | ✅ | 文件树 + 8 删除收拢作品仓，编译通过 |
| **MCUboot 二级引导 + flash XIP** | ✅ | 引导 24,640 B；text+rodata 迁入 flash XIP，**SRAM 460KB→83KB**；一次 `build.sh` 产出双镜像 |
| 烧录链路 | ✅ | `tools/usb_stable.sh` 两镜像：`0x2000` 引导 + `0x20000` 应用，均 hash 校验 |
| console（UART0/CP2102） | ✅ | **`/dev/ttyUSB0`**（须 assert DTR/RTS）；`/dev/ttyACM0` 仅烧录 |
| eth0 RJ45 联网 | ✅ | DHCP `192.168.1.105` RUNNING（主机同网段 192.168.1.119，ICMP 通）|
| PSRAM 32MB heap | ✅ | `free`：Umem total ≈ 33.9 MB |
| ai_agent 完整启动 | ✅ | P0→P6 全 rc=0，36 tools / **12 skills**，WebSocket 板端监听 28789、`All network services started!`（**交互链路见 §四.1**）|
| LVGL demo（显示+触摸）| ✅ | 最终固件上运行 `lvgldemo`（GT911 上电 + `/dev/fb0 1024x600 RGB565`），JTAG 帧缓冲导出核实（色数 1265/1266、梯度 <4，与首采一致；存档 `logs/verify-2026-09-17/lvgl_demo_final.png`）|
| 摄像头驱动链路 | ⚠️ 有帧→无 | 传感器 I2C/寄存器/stream-on ✅、CSI 2 lane + RAW10 + DMA 武装 ✅、零错误；但桥 FIFO 零字节（数据未跨 MIPI 物理链路），最终固件复测同结论，**一条命令可复测**：`tools/camera_diag.sh` |
| **自定义 Skill ×2（赛题②）** | ✅ | `center-assistant` / `quick-note`；`Skills system ready (12 built-in)` |
| **cron 定时主动（赛题③）** | ✅ | `cron_service`/`tool_cron` 真实编入，`[cron] Cron started` |
| AI Coding 日志 | ✅ | 18 会话 / 11,074 事件；`validate-log.py` → ALL OK |

## 二、当前交付

- **交付形态**：仓根 4 棵文件树（`nuttx/` + `board/esp32p4/` + `app/apps/` + `app/ai_agent/`）+ `contest2026_342_ezdezhandui.xml` **324 条 copyfile** 自动映射（清单由 `tools/gen_manifest_copyfiles.py` 与文件树一一对应校验/再生）
- **复现路径**：根 `README.md` §四（init → sync → build → **两镜像烧录** → console，**无部署步骤**；copyfile 在 `repo sync` 时自动落位，且移植不删除任何上游文件）
- **闭环工具**：`tools/board.py`（串口 harness/断言）、`tools/usb_stable.sh`（两镜像烧录）、`tools/verify_aiagent.sh`（ai_agent 启动里程碑断言）、`tools/camera_diag.sh`（摄像头链路一次性判定）、`tools/fb2png.py`（JTAG 帧缓冲→PNG 截图）、`tools/wedge_diag.sh`（agent 失聪现场 JTAG 取证）
- **真机证据存档**：`logs/verify-2026-09-17/`（`lvgl_demo.png` / `lvgl_demo_final.png` 帧缓冲截图、`camera_final.log` 摄像头实录、`wedge_jtag.txt` 失聪现场）

## 三、演示脚本

```bash
tools/demo_aiagent.sh            # 烧录 + 复位 + 启动 ai_agent + 关键输出
tools/verify_aiagent.sh          # P0→P6 里程碑 + 12 skills + eth0 + cron 断言
python3 tools/board.py run "free" "ps"    # 任意 NSH 命令并断言
```

## 四、已知限制（诚实记录）

1. **ai_agent 启动后「外部输入全哑」= 节拍中断投递失效（2026-09-17，JTAG 现场定论）**
   —— 现象：`ai_agent` 打印到 `All network services started!` 之后即失联：主机侧 ICMP
   **100% 丢包**、TCP 28789 超时、串口输入无回显，且**不会自愈**（当日 3/3 次复现）。
   现场取证（`tools/wedge_diag.sh`；原始记录 `logs/verify-2026-09-17/wedge_jtag.txt`）：
   * **CPU 在跑**：两次 halt 采样间隔 4s，`mcycle`/`minstret` 大幅前进（不是停机/死等）；
   * **节拍死了**：`g_system_ticks` 两次采样**完全相同**（100Hz 定时器 ISR 不再执行）；
     对照组（复位后未启 agent，同法 halt）：4s 内 `g_system_ticks` +410 ≈ 102.5Hz ✅；
   * **外设仍在请求中断**：`SYSTIMER INT_ENA=5 INT_RAW=5 INT_ST=5`
     （TARGET0 节拍闹钟 `TARGET0_CONF=0xc0027100`，period=160000＝10ms@16MHz；TARGET2＝IDF
     esp_timer）；手动写 `INT_CLR=0x5` 清掉后 3s 又回到 `INT_RAW=0x1`（周期闹钟照常来），
     系统**仍不恢复** ⇒ 挂起位是症状不是病因；
   * **PC 落在 trap 进出路径**：`exception_common`(0x4ff40100)/`riscv_dispatch_irq`(0x4ff405c4)/
     `return_from_exception`；对照组的 PC 稳定停在 `esp_cpu_wait_for_intr` 的 WFI 上 ⇒
     wedge 态 CPU 绝大多数时间在中断进出（风暴特征）；
   * **不是开关/阈值/向量表被改**：`mintthresh=0x1f`、`mtvec=0x4ff40003`、`mtvt=0x4ff40040`、
     `mie=mip=0` 在健康态与 wedge 态**完全一致**；halt 伪迹 `mstatus=0x1801`（MPIE=1）说明
     进 trap 之前 CPU 中断是**开着**的；
   * **处理器还在注册**：`s_intr_handlers[core0]` 两态**逐字节相同**，intno0 仍是
     `systimer_irq_handler`（即 NuttX 节拍 ISR，`g_irqvector` 也一致）。
   已排除（真机 A/B 对照）：CPU 忙等、UART 硬件、包缓冲耗尽（`IOB_NBUFFERS 8→128`、
   `IOB_NCHAINS 4→32`、`EMAC NRXDESC/NTXDESC 2→8` 恢复后现象不变）、UART RX 挂起
   （wedge 时 RX FIFO 已空、`INT_RAW` 无 RX 位）。
   线索：`board_emac_init() → esp_hr_timer_init() → esp_timer_early_init()+esp_timer_init()`
   （ROM 的 IDF esp_timer，走 `SYSTIMER_ALARM_ESPTIMER` + `ETS_SYSTIMER_TARGET2_INTR_SOURCE`）
   与 NuttX 节拍（`SYSTIMER_ALARM_OS_TICK_CORE0` + `SYSTIMER_TARGET0_INTR_SOURCE`，见
   `esp_timerisr.c`）**共用同一颗 systimer、同一套 `esp_irq.c` 的 CLIC/共享中断线分配**，
   现场正是同时看到 TARGET0/TARGET2 两个 pending 位。
   **结论：故障在网络服务启动后收敛到 CPU 侧「中断投递」路径（CLIC／共享线记账），
   而不是 agent 逻辑本身** —— agent 侧日志已完整走完。下一步在 `esp_irq.c` 的
   `up_disable_irq()`/`esp_intr_enable_source()` 记账点上打点（记录最后一次改动该线的
   调用者），或让节拍与 IDF esp_timer 不再共用 systimer。
   影响面：**不阻碍「agent 能在板上完整跑起来」的演示**，阻碍「与 agent 交互（`ask`/WS）」。
   详见 `docs/05` §26。
2. **console 输出突发会被截断** —— `esp_lowputc_send_byte()` 原先未等 TX FIFO 空间即写，
   超长突发丢字节（启动日志常见 ~1.5KB 丢失）。已在 `esp_lowputc.c` 修复（等待 FIFO 空间）
   并将 `CONFIG_UART0_TXBUFSIZE` 提到 2048。
3. **MiMo API key 无效（HTTP 401 Invalid API Key）** —— LLM 真实对话需有效 key/额度。
4. **屏 ✅ / 摄像头 ⚠️（2026-09-17 定论，最终固件复测）**：显示（DSI EK79007 + LVGL）、
   触摸（GT911）已真机闭环；**摄像头结论是「驱动链路通、物理链路无数据」**：最终固件
   `camera 3` 实测 —— `esp_csi_data_start_capture: ctlr start rc=0 (DMA+bridge enabled)`、
   `SC2336 chip ID: 0xcb3a`、`CSI: 1920x1080, 2 lane(s) @ 405 Mbps, fb=2592000 bytes`、
   `start_capture -> 0x0100=0x01, read back 0x01 (ret=0)`，三帧全部等不到数据
   （应用阻塞在 DQBUF 不会自己退出：`CONFIG_TTY_SIGINT=n`，要复位才能回 NSH）。
   诊断固件（`ESP_CSI_CAPTURE_DIAG=1`）直读硬件：CSI 桥 `csi_en=1 dtype=0x2f12 flow=0x3c0`、
   DMA `sar` 静止、`buf_depth=0`、D-PHY 仅时钟 lane 活（`rxclkactivehs=1`，数据 lane
   `stopstate=0`）→ 故障收敛到 **MIPI 数据 lane 物理通路**（模组/排线/连接器）。
   一条命令可复测：`tools/camera_diag.sh 35`（见 `docs/05` §25.3）。
5. ~~**WebSocket 28789 主机侧不通**~~ —— 2026-09-17 更新：改 DHCP 后主机与板子已同网段
   （192.168.1.119 ↔ 192.168.1.105），**agent 未启动时 TCP/ICMP 均通**；agent 一启动即随
   §四.1 的「外部输入全哑」一起失联。故本条不是独立问题，归入 §四.1。
6. ~~**DNS 不可用**~~ —— **2026-09-14 已修复**（真因三项：`CONFIG_NET_UDP` 未开、
   `CONFIG_NETINIT_DHCPC` 未开、netinit 不等链路；详见 §五 与 `docs/05` §23.2），
   现状为 **DHCP 全自动**：`eth0 192.168.1.105`、`nslookup → 202.69.4.22`。
7. **构建健壮性**（2026-09-14 已修）—— 干净树连续构建会因 HAL 兼容补丁重复应用/顺序
   问题 `config fail`；已改为"应用前 `git reset --hard` 钉定版本 + 幂等 apply + 固定顺序
   0002→0001"，并删除补丁中两段过时 hunk（详见 `docs/04_功能闭环测试.md` §八.3）。

## 五、硬件外设进展（2026-09-16 更新；本节已按 9/14–9/15 的修复结果重写）

> ⚠️ 9/14 那份"显示/触摸失败、疑似模组硬件问题"的判断**已被推翻**：
> 真因是三个**平台级驱动缺陷**（见 `docs/05_开发过程复盘与改进清单.md` §22、§23）。

| 外设 | 状态 | 根因 / 证据 |
|---|---|---|
| MIPI-DSI 7" EK79007 + LVGL | ✅ **已点亮** | 真因：`drivers/video/mipidsi/mipi_dsi_device.c` 8 处 `struct mipi_dsi_msg` **未零初始化**（`rx_len`/`rx_buf` 残留 → 一条 DCS **写**被当成**读**发出去 → 等 BTA 应答 → `-110`）。修复后真机：`/dev/fb0 ready 1024x600 RGB565 @ 0x48000040`、`/dev/fb0 registered (EK79007)` |
| GT911 触摸 | ✅ **已修** | 真因：**产品 ID 校验吃掉 NUL 补齐字节**，控制器一直被误判为"未检测到"（commit `6499860`）。显示修好后需复测触摸是否随之恢复（`docs/05` §23.5(c)） |
| MIPI-CSI SC2336 → `/dev/video0` | 🔶 识别 ✅；出帧链路修复已就位 | `VIDIOC_S_FMT: errno=22` 的真因是 `v4l2_cap.c` 在驱动未声明 `frmintervals` 时**回退写死 15 fps**，而 SC2336 只接受 1/30 → 已显式声明 `g_sc2336_frmintervals[]`；随后暴露的 DW_GDMA 中断分配失败也已修（改 `ESP_INTR_FLAG_SHARED` + 补 `up_enable_irq` 的 CLIC IE，commit `3d7b1f25`）。出帧最终验证以 `docs/05` 为准 |
| 网络 / DNS | ✅ **已打通且开机全自动** | 三个叠加原因：① `CONFIG_NET_UDP` 未开（`dns_*` 符号数为 0）② `CONFIG_NETINIT_DHCPC` 未开（注意 `NETUTILS_DHCPC` 只是"编进来"、`NETINIT_DHCPC` 才是"去跑它"）③ netinit 不等链路就发 DHCP。修复后真机：`eth0 192.168.1.105 DRaddr 192.168.1.1`、`nslookup api.xiaomimimo.com → 202.69.4.22`；顺带把 MTU 从 576 对齐到 1500 |
| 🔴 ai_agent 启动后整机冻结 | **当前最大阻塞** | 定论（9/17 JTAG）：**节拍中断投递失效** —— `g_system_ticks` 冻住、`SYSTIMER INT_ST=5` 外设仍在请求、CPU 在 trap 进出路径上打转；handler 表两态一致。已排除忙等/包缓冲/UART/阈值。线索：IDF esp_timer(TARGET2) 与 NuttX 节拍(TARGET0) 共用 systimer + `esp_irq.c` 共享线。详见 §四.1 与 `docs/05` §26。**注意：`docs/09` §七 记的 "agent 启动后 console 输入无响应" 与本案是同一问题**（不是输入通道问题） |
| 运维通道 | ⚠️ 注意 | CP2102（`/dev/ttyUSB0`）不在时可用 USJ 口：`python3 tools/board.py run "…" --port /dev/ttyACM0`；但 USJ 的 **DTR/RTS 直接控制复位/下载模式**，被串口软件占用会造成"板子假死"的假象（`docs/05` §23.6） |

## 六、安全提醒

- ⚠️ MiMo API key 曾误入公开仓历史（commit 4d7f318）——**必须轮换**；
  新 key 一律经本地 `ai_agent/include/agent_secrets.h`（.gitignore 忽略）注入。
- ⚠️ 本地环境坑：`openocd` 运行期间占用 USB，用完务必 `pkill -x openocd`；
  `pkill -f openocd` 会连自己的 shell 一起杀掉（模式自匹配）。
