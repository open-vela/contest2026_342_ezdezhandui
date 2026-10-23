# 项目状态（STATUS）

> 最后更新：**2026-10-23** · **摄像头出帧已修复（10/22）**：根因 = ESP32-P4 的 **CSI bridge 与 ISP 共享**
> （`SOC_ISP_SHARE_CSI_BRG=1`）而 openvela 从未初始化 ISP 输入模块；修复见
> `nuttx/arch/risc-v/src/common/espressif/esp_csi.c`（`esp_csi_isp_bypass_init()`），
> 验收 `tools/test/camera_verify.sh` **PASS 13/13**、抓帧 15/15（逐段结论修正史见 §四.4）。
> 新增板端 V4L2 工具 `v4l2ctl`（`app/apps/system/v4l2ctl/`，manifest → `apps/system/v4l2ctl/`）。
> 工程整理（10/23）：删除改造期文档 `docs/08`、`docs/09`（结论与证据已收进 §四.4 本文）、
> 过期交接文档 `docs/CSI_INTEGRATOR_HANDOFF.md`、废弃脚本 `tools/engineering/export-legacy.sh`
> 与模板样例 `quickapp/`；AI 日志补导至 **29 会话 / 37,968 事件**（8/21–10/23）。
> 2026-09-18 提交前基线（保留）：**「agent 一启动整机失聪」已定位并修复**：`riscv_doirq()` 的早期引导保护
> 吞掉了 `up_exit()` 的 `SYS_restore_context` ECALL（任何任务退出都会死机），修复后 agent 启动完成
> 板子照常在线（ping/TCP 28789 均通）—— 根因、证据、验证见 `docs/06` §26.7 与 `logs/verify-2026-09-17/wedge_fix.txt`；
> LVGL 截图在最终固件上复采（同一工具 `tools/test/fb2png.py`）；
> §二/§三 收拢到**无部署步骤**的交付路径（manifest copyfile 自动映射，见 `docs/06` §24）。
> §五 外设状态按 9/14–9/15 的**平台级驱动修复**（显示/触摸/DNS 已打通，见 `docs/06` §22/§23）；
> 演示视频（`提交材料/6360b48….mp4`，27.7s）+ 真机复现证据日志 5 份在 `提交材料/`；
> 逐项结果见 `docs/05_功能闭环测试.md` §七；收尾计划见 `docs/01_开发规划文档.md` §十。

## 一、主线达成状态（真机闭环验证）

| 里程碑 | 状态 | 证据 |
|---|---|---|
| ESP32-P4X 平台移植 | ✅ | 4 棵文件树 + **329 条 `<copyfile>`** 自动落位（不删除任何上游文件），编译通过 |
| **MCUboot 二级引导 + flash XIP** | ✅ | 引导 24,672 B；text+rodata 迁入 flash XIP，**`sram_seg` 静态占用 120.3 KB / 512 KB（23.5%）**（9/17 干净树重建实测；9/11 首测为 83 KB）；一次 `build.sh` 产出双镜像 |
| 烧录链路 | ✅ | `tools/build_flash/usb_stable.sh` 两镜像：`0x2000` 引导 + `0x20000` 应用，均 hash 校验 |
| console（UART0/CP2102） | ✅ | **`/dev/ttyUSB0`**（须 assert DTR/RTS）；`/dev/ttyACM0` 仅烧录 |
| eth0 RJ45 联网 | ✅ | DHCP `192.168.1.105` RUNNING（主机同网段 192.168.1.119，ICMP 通）|
| PSRAM 32MB heap | ✅ | `free`：Umem total ≈ 33.9 MB |
| ai_agent 完整启动 | ✅ | P0→P6 全 rc=0，36 tools / **12 skills**，WebSocket 板端监听 28789、`All network services started!` |
| **ai_agent 端到端对话** | ✅ | WS 28789 → 消息 → LLM → 回答（真机实测 `你好`；证据 `logs/verify-2026-09-17/agent_llm_ws.txt`）。链路修复见 `docs/06` §27（端点 401 / TCPBACKLOG RST / 计时被 TLS 改时钟）|
| LVGL demo（显示+触摸）| ✅ | 最终固件上运行 `lvgldemo`（GT911 上电 + `/dev/fb0 1024x600 RGB565`），JTAG 帧缓冲导出核实（色数 1265/1266、梯度 <4，与首采一致；存档 `logs/verify-2026-09-17/lvgl_demo_final.png`）|
| **摄像头出帧（赛题③前置）** | ✅ | SC2336 RAW8 1280×720@30（2 lane/336 Mbps）经 V4L2 稳定出帧：**PASS 13/13**、抓帧 15/15、连跑两轮一致（`tools/test/camera_verify.sh`）。根因 = CSI bridge 与 ISP 共享（`SOC_ISP_SHARE_CSI_BRG=1`）而 openvela 未初始化 ISP 输入模块 → 修复 `esp_csi_isp_bypass_init()`（`nuttx/arch/risc-v/src/common/espressif/esp_csi.c`）；结论三次修正的全过程见 §四.4 |
| **自定义 Skill ×2（赛题②）** | ✅ | `center-assistant` / `quick-note`；`Skills system ready (12 built-in)` |
| **cron 定时主动（赛题③）** | ✅ | `cron_service`/`tool_cron` 真实编入，`[cron] Cron started` |
| AI Coding 日志 | ✅ | **29 会话 / 37,968 事件**（`logs/ez-xu/`，汇总见 `logs/ez-xu/manifest.json`；claude-code 18 + dsh 11，覆盖 8/21–10/23）|

## 二、当前交付

- **交付形态**：仓根 4 棵文件树（`nuttx/` + `board/esp32p4/` + `app/apps/` + `app/ai_agent/`）+ `contest2026_342_ezdezhandui.xml` **329 条 copyfile** 自动映射（清单由 `tools/engineering/gen_manifest_copyfiles.py` 与文件树一一对应校验/再生）
- **复现路径**：根 `README.md` §四（init → sync → build → **两镜像烧录** → console，**无部署步骤**；copyfile 在 `repo sync` 时自动落位，且移植不删除任何上游文件）
- **闭环工具**：`tools/test/board.py`（串口 harness/断言）、`tools/build_flash/usb_stable.sh`（两镜像烧录）、`tools/test/verify_aiagent.sh`（ai_agent 启动里程碑断言）、`tools/test/camera_verify.sh`（**摄像头出帧验收断言 13/13**）、`tools/test/camera_diag.sh`（摄像头链路一次性判定）、`tools/test/fb2png.py`（JTAG 帧缓冲→PNG 截图）、`tools/test/wedge_diag.sh`（agent 失聪现场 JTAG 取证）
- **板端工具**：`v4l2ctl`（`app/apps/system/v4l2ctl/` → manifest 落位 `apps/system/v4l2ctl/`）—— NuttX 无 `v4l2-ctl`，用它做枚举/开流/抓帧（`v4l2ctl -d /dev/video0 --capture 3 --timeout 20`）
- **真机证据存档**：`docs/验证截图_LVGL界面_1024x600.png`（LVGL 帧缓冲首采）与 `logs/verify-2026-09-17/`（`lvgl_demo_final.png` 最终固件复采、`camera_final.log` 摄像头实录、`wedge_jtag.txt` 失聪现场、`wedge_fix.txt` 修复前后、`agent_llm_ws.txt` 端到端对话）
- **随提交材料**：`提交材料/` —— 演示视频 `6360b48….mp4`（27.7s）+ 9/18 真机复现证据日志 5 份（`boot_nsh.log` / `aiagent_milestones.log` / `aiagent_live.log` / `lvgldemo.log` / `ws_llm_endtoend.log`）；AI Coding 日志 **29 会话 / 37,968 事件**（`logs/ez-xu/manifest.json`）

## 三、演示脚本

```bash
tools/build_flash/demo_aiagent.sh            # 烧录 + 复位 + 启动 ai_agent + 关键输出
tools/test/verify_aiagent.sh          # P0→P6 里程碑 + 12 skills + eth0 + cron 断言
python3 tools/test/board.py run "free" "ps"    # 任意 NSH 命令并断言
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
   证据 `logs/verify-2026-09-17/wedge_fix.txt`、复测工具 `tools/test/wedge_diag.sh`。
   仍待办（独立问题）：对 28789 的 WS/HTTP 请求会在 ~10ms 内被 RST（TCP 通、端口在听，
   REST 接口未编入），见 `docs/06` §26.7 末。
2. **console 输出突发会被截断** —— `esp_lowputc_send_byte()` 原先未等 TX FIFO 空间即写，
   超长突发丢字节（启动日志常见 ~1.5KB 丢失）。已在 `esp_lowputc.c` 修复（等待 FIFO 空间）
   并将 `CONFIG_UART0_TXBUFSIZE` 提到 2048。
3. ~~**MiMo API key 无效（HTTP 401）**~~ —— **2026-09-17 晚更正**：key 本身有效，401 是**端点错了**（key 属于 Token Plan 网关 `token-plan-cn.xiaomimimo.com`，默认的 `api.xiaomimimo.com` 会拒绝）。改端点后 **LLM 端到端对话已真机实测成功**（WS 28789 → 消息 → 真实回答；证据 `logs/verify-2026-09-17/agent_llm_ws.txt`，链路三处修复见 `docs/06` §27）。
   ⚠️ **但该 key 已随 AI 日志公开泄露过一次（见 §六），必须轮换**；轮换后 key 是编译期注入的，需重新构建烧录。
4. **屏 ✅ / 摄像头 ✅（2026-10-22 修复；改造期文档 `docs/08`、`docs/09` 已于 10/23 删除，结论与证据全部收进本节）**：显示（DSI EK79007 + LVGL）、
   触摸（GT911）已真机闭环；**摄像头结论是「驱动链路通、物理链路无数据」**：最终固件
   `camera 3` 实测 —— `esp_csi_data_start_capture: ctlr start rc=0 (DMA+bridge enabled)`、
   `SC2336 chip ID: 0xcb3a`、`CSI: 1920x1080, 2 lane(s) @ 405 Mbps, fb=2592000 bytes`、
   `start_capture -> 0x0100=0x01, read back 0x01 (ret=0)`，三帧全部等不到数据
   （应用阻塞在 DQBUF 不会自己退出：`CONFIG_TTY_SIGINT=n`，要复位才能回 NSH）。
   诊断固件（`ESP_CSI_CAPTURE_DIAG=1`）直读硬件：CSI 桥 `csi_en=1 dtype=0x2f12 flow=0x3c0`、
   DMA `sar` 静止、`buf_depth=0`、D-PHY 仅时钟 lane 活（`rxclkactivehs=1`，数据 lane
   `stopstate=0`）→ 故障收敛到 **MIPI 数据 lane 物理通路**（模组/排线/连接器）。
   一条命令可复测：`tools/test/camera_diag.sh 35`（见 `docs/06` §25.3）。
   - **2026-10-20 复核（重要修正）**：原 `sc2336.c` 的 1080p30 表缺少 `0xffff`
     终止符，`sc2336_write_table()` 会越界读表并把后续 `.rodata` 当寄存器写入传感器；
     该缺陷已修复（改为显式表长），并补上 `ENUM_FRAMESIZES`、互斥、回读校验与
     `v4l2ctl` 目标侧工具。修复后实测：编译 0 告警、枚举全通过、`table verify 8/8`、
     `0x0100` 回读 1，但抓帧仍 0/2；诊断构建下时钟 lane HS 有效、数据 lane 非 stop、
     零 packet 零错误、DMA `dar` 静止 → 结论仍指向 MIPI 数据 lane 物理通路，
     但“硬件损坏”尚缺换模组/示波器证据。
   - **2026-10-22 复核（结论再次修正）**：换用一块**原厂 ESP-IDF 固件下相机正常**的板子，
     烧同一 openvela 镜像后仍 0 帧（枚举/开流/表回读全通过，host 有时钟 lane HS、
     零 packet 零错误、DMA 静止）。因此“摄像头/模组硬件故障”的结论**撤回**：
     硬件可用，故障在 openvela 的 CSI 接收集成。原厂固件平台为
     ESP-IDF v5.5.2 + esp_brookesia_demo 1.2.0 + esp_video 1.4.1（启动日志存档
     `docs/ref_stock_boot_esp-idf_v5.5.2.log`）。
   - **2026-10-22 参考固件实测（方案一）**：安装 IDF 5.5.6（release/v5.5）并构建
     esp_video 的 `capture_stream` 例程，在同一块板上 **30 fps 出帧**（SC2336 PID
     0xcb3a，RAW8 1280×720，2 lane/336 Mbps）。由此确认原厂运行时用的是 **RAW8
     720p**（1080p 表只是编译进固件的备选）。openvela 已切到该模式并逐项对齐 CSI
     寄存器（`frame=0x015002d0`、`dtype=0x2f12`、`hctrl=3`），但抓帧仍 0；
     差异已收敛到 D-PHY 模拟/PHY 层（或 lane 映射），需 JTAG/示波器继续。
   - **2026-10-22 16:10 ✅ 修复完成**：根因 = ESP32-P4 的 **CSI bridge 与 ISP 共享**
     （`SOC_ISP_SHARE_CSI_BRG=1`）；参考 esp_video 无条件初始化 ISP 输入模块（RAW8 走 bypass），
     openvela 从未启动 ISP → 共享桥数据通路未建立（表现为 D-PHY 有活动、host 零 packet 零错误、
     桥 FIFO 恒空、DMA 空转，而 CSI host/bridge/GDMA 寄存器与参考逐位一致）。
     修复：`esp_csi.c` 新增 `esp_csi_isp_bypass_init()`（模块时钟/复位/时钟源+分频、`isp_ll_init`、
     时钟使能、输入源=CSI、行/帧尺寸按 bpp、`isp_en=0` bypass、`mipi_csi_brg_claim(SHARE)`，
     整机只初始化一次）；另保留“删除模式表重复软复位”“CSI 控制器延迟到 STREAMON”两处修复。
     验收：`camera_verify.sh` **PASS 13/13**、抓帧 15/15、连跑两轮一致；`--preview` 可见真实画面。
   - **2026-10-22 源码级彻底比对**：运行期抓取参考侧 167 次 SCCB 写，与 openvela
     驱动序列逐条一致（已修掉我们多写的一次软复位）；`esp_cam_ctlr_csi.c` / `mipi_csi_hal.c`
     全文件 diff 仅颜色 API、日志与参考新增的错误 ISR 差异；CSI host 全窗口、bridge 窗口、
     DW-GDMA 通道（`sar/ts/ctl0/chen0`）、PHY 内部 `0x44=0x0a`、LDO 2500 mV、CPU 400 MHz /
     PSRAM 200 MHz 全部逐位一致；控制器创建已延迟到 STREAMON 与参考完全对齐。JTAG 在本 SoC
     无法读 HP 外设区（sysbus/abstract/progbuf 均失败，RAM 正常）。彩条测试图案同样 0 帧。
     下一步：把 IDF `esp_driver_cam`/`esp_video` 的启动路径原样搬进 openvela 验证。
5. ~~**WebSocket 28789 主机侧不通**~~ —— 2026-09-17 更新：改 DHCP 后主机与板子已同网段
   （192.168.1.119 ↔ 192.168.1.105），**agent 未启动时 TCP/ICMP 均通**；agent 一启动即随
   §四.1 的「外部输入全哑」一起失联。故本条不是独立问题，归入 §四.1。
6. ~~**DNS 不可用**~~ —— **2026-09-14 已修复**（真因三项：`CONFIG_NET_UDP` 未开、
   `CONFIG_NETINIT_DHCPC` 未开、netinit 不等链路；详见 §五 与 `docs/06` §23.2），
   现状为 **DHCP 全自动**：`eth0 192.168.1.105`、`nslookup → 202.69.4.22`。
7. **构建健壮性**（2026-09-14 已修）—— 干净树连续构建会因 HAL 兼容补丁重复应用/顺序
   问题 `config fail`；已改为"应用前 `git reset --hard` 钉定版本 + 幂等 apply + 固定顺序
   0002→0001"，并删除补丁中两段过时 hunk（详见 `docs/05_功能闭环测试.md` §八.3）。

8. **本地 manifest 仓副本与官方 gitee 分叉** —— `.repo/manifests` 本地领先 36 / 落后 28（非快进），
   直接 `repo sync` 会停在 `manifests rebase … 冲突`。绕开方式：`repo sync --no-manifest-update`（`--nmu`）。
   已同时修正 `tools/engineering/lock-revision.sh`：它原来只 `sed revision`，会让 `.repo/manifests` 副本在其它条目上
   悄悄漂移（2026-09-18 实测副本仍是 326 条 + 两个废弃条目）。详见 `docs/03` §3.2。
   评审侧不受影响：评审是全新 clone，`.repo/manifests` 直接来自所 init 的分支。

## 五、硬件外设进展（2026-09-16 更新；本节已按 9/14–9/15 的修复结果重写）

> ⚠️ 9/14 那份"显示/触摸失败、疑似模组硬件问题"的判断**已被推翻**：
> 真因是三个**平台级驱动缺陷**（见 `docs/06_开发过程复盘与改进清单.md` §22、§23）。

| 外设 | 状态 | 根因 / 证据 |
|---|---|---|
| MIPI-DSI 7" EK79007 + LVGL | ✅ **已点亮** | 真因：`drivers/video/mipidsi/mipi_dsi_device.c` 8 处 `struct mipi_dsi_msg` **未零初始化**（`rx_len`/`rx_buf` 残留 → 一条 DCS **写**被当成**读**发出去 → 等 BTA 应答 → `-110`）。修复后真机：`/dev/fb0 ready 1024x600 RGB565 @ 0x48000040`、`/dev/fb0 registered (EK79007)` |
| GT911 触摸 | ✅ **已修** | 真因：**产品 ID 校验吃掉 NUL 补齐字节**，控制器一直被误判为"未检测到"（commit `6499860`）。显示修好后需复测触摸是否随之恢复（`docs/06` §23.5(c)） |
| MIPI-CSI SC2336 → `/dev/video0` | ✅ **已出帧（2026-10-22）** | 早期两个坏因已修：`VIDIOC_S_FMT: errno=22` 真因是 `v4l2_cap.c` 在驱动未声明 `frmintervals` 时**回退写死 15 fps**，而 SC2336 只接受 1/30 → 已显式声明 `g_sc2336_frmintervals[]`；随后暴露的 DW_GDMA 中断分配失败也已修（改 `ESP_INTR_FLAG_SHARED` + 补 `up_enable_irq` 的 CLIC IE，commit `3d7b1f25`）。最终出帧根因与修复见 §四.4（CSI bridge 与 ISP 共享 → `esp_csi_isp_bypass_init()`）|
| 网络 / DNS | ✅ **已打通且开机全自动** | 三个叠加原因：① `CONFIG_NET_UDP` 未开（`dns_*` 符号数为 0）② `CONFIG_NETINIT_DHCPC` 未开（注意 `NETUTILS_DHCPC` 只是"编进来"、`NETINIT_DHCPC` 才是"去跑它"）③ netinit 不等链路就发 DHCP。修复后真机：`eth0 192.168.1.105 DRaddr 192.168.1.1`、`nslookup api.xiaomimimo.com → 202.69.4.22`；顺带把 MTU 从 576 对齐到 1500 |
| ✅ ~~ai_agent 启动后整机冻结~~ **已修复（9/17 晚）** | 真因：`riscv_doirq()` 的早期引导保护吞掉了 `up_exit()` 的 `SYS_restore_context` ECALL —— **任何任务退出都会死机**（子 shell `exit` 可复现）。修复：保护只对硬件中断生效；实测 agent 启动完成后 ping/TCP 28789 均正常。证据 `logs/verify-2026-09-17/wedge_fix.txt`，详见 §四.1 与 `docs/06` §26.7 |
| 运维通道 | ⚠️ 注意 | CP2102（`/dev/ttyUSB0`）不在时可用 USJ 口：`python3 tools/test/board.py run "…" --port /dev/ttyACM0`；但 USJ 的 **DTR/RTS 直接控制复位/下载模式**，被串口软件占用会造成"板子假死"的假象（`docs/06` §23.6） |

## 六、安全提醒

- ⚠️ **MiMo API key 两次误入公开仓历史 —— 必须轮换、且只当它已作废**：
  1) 早先一次：commit `4d7f318`；
  2) **2026-09-18 发现**：导出的 AI Coding 日志把会话里贴过的 key 原样带出，
     `logs/ez-xu/2026-09-11/dsh__96802734-….jsonl` 里有 4 处完整 key（该文件由 commit `85c17a6` 引入）。
     已于 commit `5663907` 全部脱敏为 `tp-***REDACTED…***`（JSONL 结构校验通过：6948 行可解析）。
  **仅从仓库改写/删除不能使其失效** —— 它已经在公开仓出现并被 fork/缓存，唯一处置是**在平台侧吊销/轮换**。
  根因是导出链路对流式会话的凭据掩码不完整（同一条日志里有的位置被掩码、有的没有）→
  **提交前必须对 `logs/` 做一次凭据扫描**（示例：`grep -rEo 'tp-[A-Za-z0-9]{40,}|sk-[A-Za-z0-9]{20,}' logs/`）。
  新 key 一律经本地 `ai_agent/include/agent_secrets.h`（.gitignore 忽略）注入，不入仓。
- ⚠️ 本地环境坑：`openocd` 运行期间占用 USB，用完务必 `pkill -x openocd`；
  `pkill -f openocd` 会连自己的 shell 一起杀掉（模式自匹配）。
