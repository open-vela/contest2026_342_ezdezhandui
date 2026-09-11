# 项目状态（STATUS）

> 最后更新：**2026-09-11** · MCUboot 二级引导打通 + 赛题②自定义 Skill 上机；收尾计划见 `docs/01_开发规划文档.md` §十

## 一、主线达成状态（真机闭环验证）

| 里程碑 | 状态 | 证据 |
|---|---|---|
| ESP32-P4X 平台移植 | ✅ | 文件树 + 8 删除收拢作品仓，编译通过 |
| **MCUboot 二级引导 + flash XIP** | ✅ | 引导 24,640 B；text+rodata 迁入 flash XIP，**SRAM 460KB→83KB** |
| 烧录链路 | ✅ | `tools/usb_stable.sh` 两镜像：`0x2000` 引导 + `0x20000` 应用，均 hash 校验 |
| console（UART0/CP2102） | ✅ | **`/dev/ttyUSB0`**（须 assert DTR/RTS）；`/dev/ttyACM0` 仅烧录 |
| eth0 RJ45 联网 | ✅ | 10.0.0.2 RUNNING |
| PSRAM 32MB heap | ✅ | `free`：Umem total ≈ 33.9 MB |
| ai_agent 完整启动 | ✅ | P0→P6 全 rc=0，36 tools / **12 skills**，WebSocket 控制通道 28789 |
| **自定义 Skill ×2（赛题②）** | ✅ | `center-assistant` / `quick-note`；`Skills system ready (12 built-in)` |
| **cron 定时主动（赛题③）** | ✅ | `cron_service`/`tool_cron` 真实编入，`[cron] Cron started` |
| AI Coding 日志 | ✅ | 18 会话 / 11,074 事件；`validate-log.py` → ALL OK |

## 二、当前交付

- **交付形态**：`board/esp32p4_vela/` 文件树（nuttx + apps + ai_agent）+ `contest2026_342_ezdezhandui.xml` **300 条 copyfile** 自动映射
- **复现路径**：根 `README.md` §四（init → sync → deploy → build → **两镜像烧录** → console）
- **闭环工具**：`tools/board.py`（串口 harness/断言）、`tools/usb_stable.sh`（两镜像烧录）、`tools/verify_aiagent.sh`（启动里程碑断言）

## 三、演示脚本

```bash
tools/demo_aiagent.sh            # 烧录 + 复位 + 启动 ai_agent + 关键输出
tools/verify_aiagent.sh          # P0→P6 里程碑 + 12 skills + eth0 + cron 断言
python3 tools/board.py run "free" "ps"    # 任意 NSH 命令并断言
```

## 四、已知限制（诚实记录）

1. **ai_agent 串口交互（`vela>` CLI）不可用** —— 应用运行后 console **输入**通路失效
   （NSH 也收不到输入）；输出侧另有细节见下。演示以 NSH 命令 + 服务日志 + WebSocket/NET 通道为主。
   已定位排除项：线程已创建并进入读循环（fstat 证实 stdin/stdout 均为 `/dev/console`），
   故非 fd 或线程调度问题，根因待继续排查。
2. **console 输出突发会被截断** —— `esp_lowputc_send_byte()` 原先未等 TX FIFO 空间即写，
   超长突发丢字节（启动日志常见 ~1.5KB 丢失）。已在 `esp_lowputc.c` 修复（等待 FIFO 空间）
   并将 `CONFIG_UART0_TXBUFSIZE` 提到 2048。
3. **MiMo API key 无效（HTTP 401 Invalid API Key）** —— LLM 真实对话需有效 key/额度。
4. **屏/摄像头驱动**：DSI(EK79007)+GT911+LVGL 与 MIPI-CSI(SC2336) 已实现并编译通过，
   硬件验证见 §五。

## 五、硬件外设进展

| 外设 | 状态 |
|---|---|
| MIPI-DSI 7" EK79007 + GT911 触摸 + LVGL | 🔶 代码完成、编译通过（`/dev/fb0` / `/dev/input0`），真机待验 |
| MIPI-CSI SC2336 → `/dev/video0` | 🔶 代码完成、编译+链接通过，真机待验（首发看点：`SC2336 chip ID: 0xcb3a`） |

## 六、安全提醒

- ⚠️ MiMo API key 曾误入公开仓历史（commit 4d7f318）——**必须轮换**；
  新 key 一律经本地 `ai_agent/include/agent_secrets.h`（.gitignore 忽略）注入。
- ⚠️ 本地环境坑：`openocd` 运行期间占用 USB，用完务必 `pkill -x openocd`；
  `pkill -f openocd` 会连自己的 shell 一起杀掉（模式自匹配）。
