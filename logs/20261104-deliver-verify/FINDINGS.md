# 2026-11-04 真机验证与修复记录（NSH 进不去 → 已修复）

## 结论（最终）

**根因：`up_exit()` 的换出 ECALL 被通用层 `riscv_doirq()` 的"无当前任务就丢弃"守卫
吃掉**，导致换出永不发生、ECALL 被无限重放（实测 ~9e5–1.6e6 次/秒），tick/网络/控制台
全部饿死 → 进不去 NSH、整机失聪。

修复在芯片层 `nuttx/arch/risc-v/src/esp32p4/espressif/esp_irq.c` 的
`riscv_dispatch_irq()`：进通用层前把 `g_running_task` 补回成就绪队列首任务
（等价于非 SMP 的 `this_task()` 语义）。通用层零写入。

完整诊断链与可复用方法论见 `docs/50_排障经验/11_ECALL换出风暴与异常被丢弃.md`。

## 修复前后（真机实测）

| 判据 | 修复前 | 修复后 |
|---|---|---|
| JTAG PC 采样 8 次（每次 halt→读→resume） | 8/8 在 trap 路径 | **0/8**（`esp_cpu_wait_for_intr`） |
| `g_dbg_exc_cnt` | 3 秒 +479 万（~1.6e6/s） | 不再增长 |
| `g_dbg_demux_cnt` | 冻结（外设中断零派发） | 正常 |
| NSH | 进不去 | `NuttShell (NSH)` / `nsh>` 正常 |
| 应用层 | 跑不了 | `DM-SELFTEST RESULT: PASS (27/27)` |

## 关键现场（供复核）

```
mcause = 0x3000000b   （ECALL）
mepc   = 0x40030b64   （up_exit 的 ecall 指令）
ra     = 0x40030b5a

40030b5e: sw   zero,652(a5)   ; g_running_task = NULL（up_exit 的合法动作）
40030b62: li   a0,1           ; SYS_restore_context
40030b64: ecall               ; ← 被反复重放
40030b6a: j    40030b62
```

## 端口口径（本轮实测修正）

- `/dev/ttyUSB0`（CP2102）= UART0 镜像：**日志可读；系统健康时写入也会被 NSH 执行**
  （系统卡死时写入无效——本轮一度误判为"写不进去"）。
- `/dev/ttyACM0`（USB-Serial/JTAG）= 烧录（esptool）与 JTAG（OpenOCD）；固件处于风暴时
  打开会阻塞（可作"已风暴"的旁证）。
- 结论：**看日志/下发命令用 USB0，烧录/调试用 ACM0**（文档 §31 的"控制台在 ACM0"
  只对下载/调试成立）。

## 顺带修掉 / 发现

1. `esp_sdmmc.c`：HLE（电平型企业错误）在 ISR 里未屏蔽 → 控制器会以中断风暴形式重入。
   已加"ISR 屏蔽 + 下个传输重新武装"（43 行，含理由注释）。它不是本次卡点，但是真实缺陷。
2. 显示看门狗会周期性打印 `display: 停摆 6s（commits=N），尝试重建` —— 属既有行为
   （docs/20 系列已记录），应用会自愈，未列入本次修复范围。
3. C6 侧固件为 station 示例（自带射频），主机 `wifi_set_mode` 必然返回
   `12289 = ESP_ERR_WIFI_STATE`；这是**配对**问题，与本次风暴无关（风暴在 hosted
   关闭的对照里同样出现——尽管那次对照因旧 `.config` 复用而无效，最终由"异常全部是
   ECALL、且与 SDMMC/hosted 无关"的证据定性）。

## 排查过程里的两次自我纠正（保留教训）

1. **无效对照**：第一次"关掉 hosted"的对照实验因构建复用旧 `.config` 而未生效
   （镜像里 `CONFIG_ESPRESSIF_HOSTED_OFFICIAL=y` 仍在），结论作废。
   → 教训：改 defconfig 后必须核对 `.config` 与 ELF 符号，不能只看"构建成功"。
2. **无效采样**：`halt` 后连续读多次 PC / 计数器，读到的是同一个冻结值。
   → 教训：每次采样之间必须 `resume`。

## 追加修复（同一轮）

1. `apps/examples/deskmate/src/hal/dm_display.c`：看门狗改为**代际语义**
   （`g_wd_gen`）—— 解决"自检退出后立刻再启动"时旧线程未被回收/被复位复活，
   导致两个 `dm-disp-wd` 同时重建显示的问题。
2. `dm_touch.c` / `dm_touch.h` / `deskmate_main.c`：触摸轮询线程加
   `dm_touch_shutdown()`（同样用代际语义），退出时调用 —— 解决 `dm-touch` 泄漏与
   两个触摸线程抢同一设备。
3. 已知限制（重启后不重启板子直接再跑 UI 会崩在 LVGL 悬垂对象、`&` 不可用、
   wlan0 不可用、`/mnt/spif` 未挂载、OpenOCD reset 不可靠）见
   `docs/50_排障经验/11_ECALL换出风暴与异常被丢弃.md` 末节。
