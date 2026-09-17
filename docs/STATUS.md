# 项目状态（STATUS）

> 最后更新：**2026-09-16** · 新增《openvela 系统框架解读》（`docs/10`）；
> §五 外设状态按 9/14–9/15 的**平台级驱动修复**重写（显示/触摸/DNS 已打通，详见 `docs/05` §22/§23）
> （逐项结果见 `docs/04_功能闭环测试.md` §七）；收尾计划见 `docs/01_开发规划文档.md` §十
>
> 本轮复验要点：一次 `build.sh` 即产出**双镜像**（修复 `bootloader` 目标未入 `all` +
> 内层 ninja 生成器丢失两个接线缺陷）；真机 MCUboot → XIP 映射 → NSH 全链路通过；
> 新暴露 3 个问题：**无 `/dev/fb0`**（EK79007 I2C 无应答）、**DNS 不可用**（镜像无
> `/etc/resolv.conf`）、**WebSocket 28789 主机侧不通**（板端监听已建立，主机经路由器
> 192.168.1.1 路由到 10.0.0.2，非同一 L2）。

## 一、主线达成状态（真机闭环验证）

| 里程碑 | 状态 | 证据 |
|---|---|---|
| ESP32-P4X 平台移植 | ✅ | 文件树 + 8 删除收拢作品仓，编译通过 |
| **MCUboot 二级引导 + flash XIP** | ✅ | 引导 24,640 B；text+rodata 迁入 flash XIP，**SRAM 460KB→83KB**；一次 `build.sh` 产出双镜像 |
| 烧录链路 | ✅ | `tools/usb_stable.sh` 两镜像：`0x2000` 引导 + `0x20000` 应用，均 hash 校验 |
| console（UART0/CP2102） | ✅ | **`/dev/ttyUSB0`**（须 assert DTR/RTS）；`/dev/ttyACM0` 仅烧录 |
| eth0 RJ45 联网 | ✅ | 10.0.0.2 RUNNING |
| PSRAM 32MB heap | ✅ | `free`：Umem total ≈ 33.9 MB |
| ai_agent 完整启动 | ✅ | P0→P6 全 rc=0，36 tools / **12 skills**，WebSocket 控制通道板端监听 28789（主机侧连通待打通，见 §四.5） |
| **自定义 Skill ×2（赛题②）** | ✅ | `center-assistant` / `quick-note`；`Skills system ready (12 built-in)` |
| **cron 定时主动（赛题③）** | ✅ | `cron_service`/`tool_cron` 真实编入，`[cron] Cron started` |
| AI Coding 日志 | ✅ | 18 会话 / 11,074 事件；`validate-log.py` → ALL OK |

## 二、当前交付

- **交付形态**：仓根 4 棵文件树（`nuttx/` + `board/esp32p4/` + `app/apps/` + `app/ai_agent/`）+ `contest2026_342_ezdezhandui.xml` **324 条 copyfile** 自动映射（清单由 `tools/gen_manifest_copyfiles.py` 与文件树一一对应校验/再生）
- **复现路径**：根 `README.md` §四（init → sync → build → **两镜像烧录** → console，**无部署步骤**；copyfile 在 `repo sync` 时自动落位，且移植不删除任何上游文件）
- **闭环工具**：`tools/board.py`（串口 harness/断言）、`tools/usb_stable.sh`（两镜像烧录）、`tools/verify_aiagent.sh`（启动里程碑断言）

## 三、演示脚本

```bash
tools/demo_aiagent.sh            # 烧录 + 复位 + 启动 ai_agent + 关键输出
tools/verify_aiagent.sh          # P0→P6 里程碑 + 12 skills + eth0 + cron 断言
python3 tools/board.py run "free" "ps"    # 任意 NSH 命令并断言
```

## 四、已知限制（诚实记录）

1. **ai_agent 串口交互（`vela>` CLI）不可用** —— agent 启动后 console **输入**通路失效
   （本轮复验补充：agent 启动**前** NSH 交互完全正常，`uname/free/ls/ifconfig/df/mount/uptime`
   全部有正常输出；agent 一启动 console 就收不到任何响应）。演示以 NSH 命令 + 服务日志为主。
   已定位排除项：线程已创建并进入读循环（fstat 证实 stdin/stdout 均为 `/dev/console`），
   故非 fd 或线程调度问题，根因待继续排查（方向：agent CLI 与 NSH 对同一
   `/dev/console` 的抢占/阻塞）。
2. **console 输出突发会被截断** —— `esp_lowputc_send_byte()` 原先未等 TX FIFO 空间即写，
   超长突发丢字节（启动日志常见 ~1.5KB 丢失）。已在 `esp_lowputc.c` 修复（等待 FIFO 空间）
   并将 `CONFIG_UART0_TXBUFSIZE` 提到 2048。
3. **MiMo API key 无效（HTTP 401 Invalid API Key）** —— LLM 真实对话需有效 key/额度。
4. **屏/摄像头驱动**：DSI(EK79007)+GT911+LVGL 与 MIPI-CSI(SC2336) 已实现并编译通过，
   硬件验证见 §五。
5. **WebSocket 28789 主机侧不通**（本轮新发现）—— 板端日志确认 `WebSocket server started on
   port 28789`，但从主机 TCP 连接被拒/超时：主机 `ip route get 10.0.0.2` 显示经路由器
   `192.168.1.1` 转发（非同一二层），ICMP ping 通（0% 丢包）而 TCP 不通。需在同一网段的
   主机上复测，或在板端加一个本机自连用例。
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
| 🔴 ai_agent 启动后整板冻结 | **当前最大阻塞** | `All network services started!` 之后约 15–27s 整板冻死（未复位）。已排除：空闲、纯网络压测、显示、MTU；嫌疑收窄到网络服务启动后跑起来的组件（与 cron 10s tick 时间吻合）。**注意：`docs/09` §七 记的 "agent 启动后 console 输入无响应" 与本冻结是同一个问题**（不是输入通道问题） |
| 运维通道 | ⚠️ 注意 | CP2102（`/dev/ttyUSB0`）不在时可用 USJ 口：`python3 tools/board.py run "…" --port /dev/ttyACM0`；但 USJ 的 **DTR/RTS 直接控制复位/下载模式**，被串口软件占用会造成"板子假死"的假象（`docs/05` §23.6） |

## 六、安全提醒

- ⚠️ MiMo API key 曾误入公开仓历史（commit 4d7f318）——**必须轮换**；
  新 key 一律经本地 `ai_agent/include/agent_secrets.h`（.gitignore 忽略）注入。
- ⚠️ 本地环境坑：`openocd` 运行期间占用 USB，用完务必 `pkill -x openocd`；
  `pkill -f openocd` 会连自己的 shell 一起杀掉（模式自匹配）。
