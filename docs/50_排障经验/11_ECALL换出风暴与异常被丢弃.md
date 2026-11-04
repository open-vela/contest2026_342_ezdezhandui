# 11 · ECALL 换出风暴：异常被通用层守卫丢弃（2026-11-04）

## 症状

hosted（SDIO 到板载 C6）初始化跑完之后，**永远进不到 NSH**：控制台最后一行是
`eh_hosted_nuttx: skipping RPC wifi_start ...`，之后完全静默；JTAG 采样 cpu0 的 PC
（每次都 `halt → reg pc → resume`）8/8 全落在 `_interrupt_handler` /
`return_from_exception` / `riscv_dispatch_irq` / `riscv_doirq`。

线程版（hosted 初始化放独立线程）则表现为"先能进 NSH，**过一会儿**整机失聪"——
同一个故障，只是发生时机的差别。

## 定量（仓库自带 `ESP_WEDGE_DIAG` 诊断全局，JTAG 只读）

| 观测量 | 读数 | 判读 |
|---|---|---|
| `g_dbg_exc_cnt` | 87,965,610 → 92,755,742（3 s） | **~1.6e6 次异常/秒** |
| `g_dbg_demux_cnt` | 3651 → 3651 | 外设中断**零派发** |
| `g_dbg_irq_cnt[]` | 511 / 2 / 23 全部不增 | 不是中断风暴 |
| `g_dbg_exc_frozen_*`（本次新增的冻结记录） | 4 条全同 | `mcause=0x3000000b`（ECALL）、`mepc=0x40030b64`、`ra=0x40030b5a` |

`mepc=0x40030b64` 反汇编落在 `up_exit()`：

```asm
40030b5e: sw   zero,652(a5)   ; g_running_task = NULL
40030b62: li   a0,1           ; SYS_restore_context
40030b64: ecall               ; ← 风暴就是这条被反复重放
40030b6a: j    40030b62       ; 若 ECALL 返回就再来一次
```

## 根因链（三层，缺一不成）

1. **`up_exit()` 的合法语义**（`arch/risc-v/src/common/riscv_exit.c`，上游原生、本移植未改）：
   先 `nxtask_exit()` 销毁当前任务，再**故意** `g_running_task = NULL`，然后发
   `SYS_restore_context` ECALL —— 期望异常处理把 CPU 换到就绪队列里的下一个任务。
2. **`riscv_dispatch_irq()`**（`arch/risc-v/src/esp32p4/espressif/esp_irq.c`，本移植）里
   那条早期启动守卫**已经**区分异常与硬件中断（`irq > RISCV_MAX_EXCEPTION` 才丢弃），
   所以 ECALL 被正确放行到通用层。
3. **但通用层 `riscv_doirq()` 里 openvela 的早期启动守卫没有区分**：

   ```c
   if (g_running_task == NULL)
     {
       return regs;          /* 把这条换出 ECALL 一起丢了 */
     }
   ```

   → 换出永不发生 → `up_exit()` 的重试循环 → 9e5/s 的 ECALL 风暴，tick/网络/控制台
   全部饿死。

> 关键认知：**`g_running_task == NULL` 在任务退出路径上是合法状态**，不是错误。
> 任何"没有当前任务就丢弃"的守卫都必须放行异常/系统调用。

## 修复（芯片层，通用层零写入）

`esp_irq.c` 的 `riscv_dispatch_irq()`，在调用通用层 `riscv_doirq()` **之前**把
"当前任务"补回成就绪队列首任务（这正是非 SMP 构建里 `this_task()` 的取值语义，
也是 `up_exit()` 期望换入的那个任务）；队列真的为空时保持 NULL，让 `up_exit()`
末尾的 `PANIC()` 把失败显式暴露出来，而不是退化成静默风暴。

```c
  if (!is_irq && g_running_task == NULL)
    {
      FAR struct tcb_s *next = (FAR struct tcb_s *)g_readytorun.head;

      if (next != NULL)
        {
          g_running_task = next;
        }
    }
```

## 验证（真机，2026-11-04 20:30 前后）

| 判据 | 修复前 | 修复后 |
|---|---|---|
| JTAG PC 采样（8 次，每次 resume） | **8/8 在 trap 路径** | **0/8**（全部 `esp_cpu_wait_for_intr`） |
| 异常速率 | ~1.6e6/s | 不再增长 |
| NSH | 进不去 | `NuttShell (NSH)` + `nsh>` 正常 |
| 应用层 | 跑不了 | `DM-SELFTEST RESULT: PASS (27/27)`（V4L2 相机 10 fps、显示 11/s、UI 40 ms、WiFi 降级处理） |
| 5 分钟长跑 | — | 采样无 TRAP（见 `logs/20261104-deliver-verify/stability_5min.log`） |

## 排查方法论（可复用）

1. **先否定"中断风暴"**：`g_dbg_irq_cnt[]` 与 `g_dbg_demux_cnt` 冻结 ⇒ 中断根本没派发，
   问题在异常侧。
2. **`mcause` 会被 JTAG halt 自己的 ECALL 覆盖**：所以要"冻结记录前 N 次异常"
   （本次加了 `g_dbg_exc_frozen_*`，见 `esp_irq.c` 的 `ESP_WEDGE_DIAG` 段）。
   过滤条件不能按"非 ECALL"——本次风暴里**全部**是 ECALL，必须按"计数器越过阈值后
   的前 4 次"取样。
3. **JTAG 读内存必须"halt → 读 → resume"**：halt 状态下隔几秒读两次，读到的是同一个
   冻结值，会得出"计数器不增长"的错误结论（本次踩过两次）。
4. 反汇编定位：`riscv-none-elf-objdump -d --start-address=... --stop-address=...`，
   比只靠 `addr2line`（这里只有函数名、没有行号）信息量大得多。

## 已知限制（本轮未修，如实记录）

1. **同一启动内"跑过 `--selftest` 之后再启动常驻模式"会崩**：崩溃点
   `lv_obj_get_screen()`，沿 parent 链解引用到已释放的 LVGL 对象
   （`MTVAL` 是"垃圾指针 + 小偏移"，例如 0x80d）。根因是 `lv_deinit()` 之后
   应用内仍有旧对象/定时器引用；把 UI/LVGL 的重建路径做成彻底重入安全超出本轮范围。
   **workaround**：要重新起 UI 先复位板子（`tools/build_flash/usb_stable.sh "" "" reset`），
   再 `deskmate --agent mock`。全新启动下稳定。
2. **不要用 `&` 后台运行**：本配置没开 `CONFIG_NSH_JOB_CONTROL`，
   `deskmate ... &` 会让 shell 在作业控制路径上空指针崩溃
   （`Load access fault, EPC` 落在不可反汇编区域，MTVAL=0）。
   UI 应用应当前台运行（它会自己占住 NSH 任务，符合预期）。
3. **wlan0 不可用**（与本次 P0 无关）：C6 跑的是 station 示例固件，自带射频，
   主机 `wifi_set_mode(STA)` 返回 `12289 = ESP_ERR_WIFI_STATE`，且 C6 从不拉
   SDIO 卡中断（主机侧每 2s 轮询一次）。要通 WiFi 需要换成"纯网络适配器"从机固件
   并配套主机初始化（docs/30_实现/连接_WiFi_SDIO.md 的 §5 线）。
4. **`camera` 落盘失败**：`/mnt/spif` 不存在（`df` 只有 `/proc`、`/tmp`），
   `fopen error : 2`；采集链路本身正常（CSI/SC2336/DMA/stream-on 全通）。
5. **OpenOCD `reset run` + 立刻 shutdown 不可靠**：会把芯片留在 `0x4fc00f42`
   附近的死循环里（MCUboot 之后无输出）。复位请用
   `tools/build_flash/usb_stable.sh "" "" reset`（esptool）。
