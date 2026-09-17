# 项目状态（STATUS）

> 最后更新：**2026-09-17（晚）** · **「agent 一启动整机失聪」已定位并修复**：`riscv_doirq()` 的早期引导保护
> 吞掉了 `up_exit()` 的 `SYS_restore_context` ECALL（任何任务退出都会死机），修复后 agent 启动完成
> 板子照常在线（ping/TCP 28789 均通）—— 根因、证据、验证见 `docs/06` §26.7 与 `logs/verify-2026-09-17/wedge_fix.txt`；
> §四.4 摄像头按最终固件复测写入定论；LVGL 截图在最终固件上复采（同一工具 `tools/fb2png.py`）；
> §二/§三 收拢到**无部署步骤**的交付路径（manifest 324 条 copyfile，见 `docs/06` §24）。
> §五 外设状态仍按 9/14–9/15 的**平台级驱动修复**（显示/触摸/DNS 已打通，见 `docs/06` §22/§23）；
> 逐项结果见 `docs/05_功能闭环测试.md` §七；收尾计划见 `docs/01_开发规划文档.md` §十。

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

1. ~~**ai_agent 启动后「外部输入全哑」**~~ —— **2026-09-17 已修复**。
   真因（JTAG 埋点一轮定位）：`nuttx/arch/risc-v/src/common/riscv_doirq.c` 的早期引导保护
   `if (g_running_task == NULL) return regs;` 把**所有** trap 都吞掉，而 `up_exit()` 正是
   **故意**先置 `g_running_task = NULL`、再发 `ECALL(SYS_restore_context)` 去切换上下文 →
   该 ECALL 进不到 `riscv_swint()`，被 `.S` 里的 `j` 无限重发（实测 ≈49 万次/秒），
   CPU 100% 卡在 trap 进出路径，节拍中断不再被服务 → 网络/串口一起"哑"。
   **与 agent 无关：子 shell 里敲 `exit` 同样死机**（判别实验，已复现）。
   修复：保护只对硬件中断生效（`irq > RISCV_MAX_EXCEPTION`），异常/系统调用照常派发。
   修复后实测：`ai_agent` 启动完成 → ping 5/6（16.7%，与空闲基线一致；修复前 **0/5 = 100% 丢**）、
   TCP 28789 连接成功；子 shell `exit` 后系统照常运行。详见 `docs/06` §26.7、
   证据 `logs/verify-2026-09-17/wedge_fix.txt`、复测工具 `tools/wedge_diag.sh`。
   仍待办（独立问题）：对 28789 的 WS/HTTP 请求会在 ~10ms 内被 RST（TCP 通、端口在听，
   REST 接口未编入），见 `docs/06` §26.7 末。
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
   一条命令可复测：`tools/camera_diag.sh 35`（见 `docs/06` §25.3）。
5. ~~**WebSocket 28789 主机侧不通**~~ —— 2026-09-17 更新：改 DHCP 后主机与板子已同网段
   （192.168.1.119 ↔ 192.168.1.105），**agent 未启动时 TCP/ICMP 均通**；agent 一启动即随
   §四.1 的「外部输入全哑」一起失联。故本条不是独立问题，归入 §四.1。
6. ~~**DNS 不可用**~~ —— **2026-09-14 已修复**（真因三项：`CONFIG_NET_UDP` 未开、
   `CONFIG_NETINIT_DHCPC` 未开、netinit 不等链路；详见 §五 与 `docs/06` §23.2），
   现状为 **DHCP 全自动**：`eth0 192.168.1.105`、`nslookup → 202.69.4.22`。
7. **构建健壮性**（2026-09-14 已修）—— 干净树连续构建会因 HAL 兼容补丁重复应用/顺序
   问题 `config fail`；已改为"应用前 `git reset --hard` 钉定版本 + 幂等 apply + 固定顺序
   0002→0001"，并删除补丁中两段过时 hunk（详见 `docs/05_功能闭环测试.md` §八.3）。

## 五、硬件外设进展（2026-09-16 更新；本节已按 9/14–9/15 的修复结果重写）

> ⚠️ 9/14 那份"显示/触摸失败、疑似模组硬件问题"的判断**已被推翻**：
> 真因是三个**平台级驱动缺陷**（见 `docs/06_开发过程复盘与改进清单.md` §22、§23）。

| 外设 | 状态 | 根因 / 证据 |
|---|---|---|
| MIPI-DSI 7" EK79007 + LVGL | ✅ **已点亮** | 真因：`drivers/video/mipidsi/mipi_dsi_device.c` 8 处 `struct mipi_dsi_msg` **未零初始化**（`rx_len`/`rx_buf` 残留 → 一条 DCS **写**被当成**读**发出去 → 等 BTA 应答 → `-110`）。修复后真机：`/dev/fb0 ready 1024x600 RGB565 @ 0x48000040`、`/dev/fb0 registered (EK79007)` |
| GT911 触摸 | ✅ **已修** | 真因：**产品 ID 校验吃掉 NUL 补齐字节**，控制器一直被误判为"未检测到"（commit `6499860`）。显示修好后需复测触摸是否随之恢复（`docs/06` §23.5(c)） |
| MIPI-CSI SC2336 → `/dev/video0` | 🔶 识别 ✅；出帧链路修复已就位 | `VIDIOC_S_FMT: errno=22` 的真因是 `v4l2_cap.c` 在驱动未声明 `frmintervals` 时**回退写死 15 fps**，而 SC2336 只接受 1/30 → 已显式声明 `g_sc2336_frmintervals[]`；随后暴露的 DW_GDMA 中断分配失败也已修（改 `ESP_INTR_FLAG_SHARED` + 补 `up_enable_irq` 的 CLIC IE，commit `3d7b1f25`）。出帧最终验证以 `docs/06` 为准 |
| 网络 / DNS | ✅ **已打通且开机全自动** | 三个叠加原因：① `CONFIG_NET_UDP` 未开（`dns_*` 符号数为 0）② `CONFIG_NETINIT_DHCPC` 未开（注意 `NETUTILS_DHCPC` 只是"编进来"、`NETINIT_DHCPC` 才是"去跑它"）③ netinit 不等链路就发 DHCP。修复后真机：`eth0 192.168.1.105 DRaddr 192.168.1.1`、`nslookup api.xiaomimimo.com → 202.69.4.22`；顺带把 MTU 从 576 对齐到 1500 |
| ✅ ~~ai_agent 启动后整机冻结~~ **已修复（9/17 晚）** | 真因：`riscv_doirq()` 的早期引导保护吞掉了 `up_exit()` 的 `SYS_restore_context` ECALL —— **任何任务退出都会死机**（子 shell `exit` 可复现）。修复：保护只对硬件中断生效；实测 agent 启动完成后 ping/TCP 28789 均正常。证据 `logs/verify-2026-09-17/wedge_fix.txt`，详见 §四.1 与 `docs/06` §26.7 |
| 运维通道 | ⚠️ 注意 | CP2102（`/dev/ttyUSB0`）不在时可用 USJ 口：`python3 tools/board.py run "…" --port /dev/ttyACM0`；但 USJ 的 **DTR/RTS 直接控制复位/下载模式**，被串口软件占用会造成"板子假死"的假象（`docs/06` §23.6） |

## 六、安全提醒

- ⚠️ MiMo API key 曾误入公开仓历史（commit 4d7f318）——**必须轮换**；
  新 key 一律经本地 `ai_agent/include/agent_secrets.h`（.gitignore 忽略）注入。
- ⚠️ 本地环境坑：`openocd` 运行期间占用 USB，用完务必 `pkill -x openocd`；
  `pkill -f openocd` 会连自己的 shell 一起杀掉（模式自匹配）。
