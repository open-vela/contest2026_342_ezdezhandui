# 踩坑笔记 #07：摄像头 MIPI-CSI / SC2336 接收链路

> 时间范围：2026-09-15 ~ 09-16。目标：把 SC2336（1920×1080 RAW10，2-lane MIPI CSI-2）
> 接到 `/dev/video0`，让帧进到 PSRAM。
>
> **结论（诚实版）**：
> - **驱动软件侧已逐级验证为正确**（下面有完整证据链）；
> - **但一帧都没有出来**，最终判定位**摄像头模组 / 其 MIPI 高速链路**的硬件故障。
>
> 这一篇的重点不是"怎么修好了"，而是**"怎么把范围收敛到硬件"**——
> 里面有 6 个我实际踩过、并且**一度据此得出错误结论**的坑。

硬件：`SC2336（SCCB @ I2C0 0x30）→ 2-lane MIPI CSI-2 → ESP32-P4 CSI Host → CSI Bridge → DW-GDMA ch0 → PSRAM`。

---

## 〇、先说结论：验证到了哪一步

| 环节 | 状态 | 证据 |
|---|---|---|
| 传感器身份 | ✅ | chip ID `0xcb3a`，与 `SC2336_PID` 完全一致 |
| 传感器寄存器表 | ✅ | 与 Espressif 官方参考表**逐条一致**（149/149，无缺失/多余/取值差异） |
| 开流时序 | ✅ | 与官方驱动一致，`0x0100` 写 1 回读 1 |
| CSI Host | ✅ | `csi2_resetn=1`、`phy_shutdownz=1`、`dphy_rstz=1`、2 lane、`PLL_F20M` 参考时钟已使能 |
| CSI Bridge | ✅ | 桥时钟 `host_ctrl=0x3`、`csi_en=1`、1920×1080、RAW10、burst 512 |
| DW-GDMA | ✅ | `dmac_en=1`、通道已武装（`block_ts=324000` 恰好一帧 RAW10）；**同一控制器被 DSI 通道证明可用** |
| **数据是否到达** | ❌ | `dar` 完全静止、桥 FIFO 为空、Host **零 packet 零错误** |

**最终判据**：打开传感器**内部测试图案**（`0x4501` bit3，绕过像素阵列与镜头）后，
D-PHY 报告 `phy_rx=0x00030000`（bit17 `rxclkactivehs=1`，clock lane 处于 HS）、
`stopstate=0`（各 lane 均未停）——**传感器确实在发**；
但 CSI Host **既不产生任何 packet、也不报任何错误**。
「模拟状态正常 + 数字侧零 packet 零错误」= **D-PHY 收到的数据没有送达 Host 的包解析器**，属物理层。

> 上游 `espressif/esp-video-components` issue **#53** 记录了**同一块板 + 同一颗 SC2336 的完全相同症状**，
> 其最终处理结果是**更换摄像头模组后恢复正常**（原模组硬件故障）。

---

## 坑 1：缓冲尺寸没跟着像素格式改

从 RGB565 切到 RAW10 后，缓冲仍是 QVGA RGB565 的 `320×240×2 = 153600`，而实际需要
`1920×1080×10/8 = 2592000`。驱动直接拒绝：

```
esp_csi_data_set_buf: ERROR: buffer too small: 0x4ff6eda0, 153600 < 2592000
```

**修复**：`camera_main.c` 里按格式算 `IMAGE_RAW10_SIZE = H_RES*V_RES*10/8`。

---

## 坑 2：缓冲只按 32 字节对齐，而 cache line 是 64 字节

```
esp_cache_msync: start address: 0x483a4da0 ... not aligned with cache line size (0x40)B
```

`0x...a0` **对 32 字节对齐、对 64 字节不对齐**。`esp_cache_msync()` 会拒绝这种地址，
于是 `s_ctlr_csi_start()` 在 `cache msync(M2C)` 这一步就失败。

**修复**：`memalign(32, ...)` → `memalign(64, ...)`。

---

## 坑 3：DSI 与摄像头抢同一个 DW-GDMA 通道 → `Store/AMO access fault`

**现象**：跑 `camera` 直接 panic：

```
Store/AMO access fault, EPC in dw_gdma_channel_config_transfer, MTVAL=0x50081100
```

`0x50081100` 正是 **DW-GDMA ch0 的寄存器块**。
（顺带：DW-GDMA 的真实基址是 **`0x50081000`**，不是 `0x500a0000`——我前期按错的基址读了一大批寄存器，
那批数据全是垃圾，浪费了不少时间。）

**机理（三层叠加）**：

1. **DSI 用裸 LL 直接占用通道 0**（`ESP_MIPI_DSI_DMA_CHAN 0`），**完全不经过 `dw_gdma` 分配器**；
2. 摄像头的 `channel_register_to_group()` 取 `channels[]` **第一个空槽** → ch0 没被登记，**摄像头也拿到 ch0**；
3. DSI 随后的 `dw_gdma_ll_reset_register(0)` + `dw_gdma_hal_init()` 对**整个控制器做全局复位**，
   抹掉摄像头的 ch0 配置 → `esp_cam_ctlr_start` 失败 → 后续寄存器访问直接 fault。

**修复**：DSI 改**通道 1**，把 ch0 完整让给摄像头；
并且当总线时钟已被别人打开时**不再做全局复位**。

**实测**（修复后通道彻底分离）：

| 通道 | sar | dar | 归属 |
|---|---|---|---|
| ch[0] | `0x50104000`（CSI 桥 FIFO） | `0x483a4dc0`（PSRAM 帧缓冲） | **摄像头** |
| ch[1] | `0x483a4d80`（LCD 帧缓冲） | `0x50105000`（DSI 桥 FIFO） | **DSI**（`isr=15 rst=16` ≈80Hz） |

---

## 坑 4：**寄存器偏移绝对不能猜**——两次误读都出在这里

这是本次排查里**代价最大**的一类错误。教训：**任何寄存器偏移，先用 host 编译的 `offsetof()` 程序核实**。

```c
/* /tmp/brgoff.c —— 用 host gcc 编译，直接打印结构体布局 */
#include "soc/mipi_csi_bridge_struct.h"
printf("host_ctrl @0x%02zx\n", offsetof(csi_brg_dev_t, host_ctrl));
```

实测布局（`sizeof(csi_brg_dev_t) == 0x50`，与头文件 `_Static_assert` 一致）：

| 寄存器 | 偏移 |
|---|---|
| `clk_en` | `0x00` |
| `csi_en` | `0x04` |
| `buf_flow_ctl` | `0x0c` |
| `data_type_cfg` | `0x10` |
| `frame_cfg` | `0x14` |
| `int_raw` | `0x1c` |
| `int_ena` | `0x28` |
| `dmablk_size` | `0x30` |
| **`host_ctrl`（`csi_enableclk` bit0）** | **`0x40`** |

**误读 1**：我把 `+0x00` 的 `clk_en` 当成"C 桥时钟使能位"，读到 0 就断言"桥时钟没开"，并据此改了一轮代码。
实际上 `clk_en` **默认就是 0**，含义只是**"时钟门控开关"**（0 = 允许门控），
真正的"时钟 lane 模块使能"是 `host_ctrl` bit0，在 **`0x40`**，实测一直是 1。
**这个错误结论差点让我去改一段本来正确的时钟代码。**

**误读 2（更隐蔽）**：`n_lanes` 读到 **1**，我一度以为"Host 只配了 1 lane"。
实际上 HAL 写的是：

```c
dev->n_lanes.n_lanes = lanes_num - 1;     /* 读到 1 表示 2 lane，完全正确 */
```

**两个教训**：
- 寄存器**偏移**要用 `offsetof()` 验；
- 寄存器**取值语义**（尤其是"编码过的值"和"默认值"）要读 HAL 的**写入侧**代码，不能只看读回值。

---

## 坑 5：`bridge int_raw` **不是**"收到数据"标志

我曾看到 `bridge int_raw = 0`，据此断言"桥一个字节都没收到"。
实际上它的位定义**全是错误位**：

| bit | 含义 |
|---|---|
| 0 | `vadr_num_gt` — 行数比配置多 |
| 1 | `vadr_num_lt` — 行数比配置少 |
| 2 | `discard` — **收到过不完整的帧** |
| 3 | `csi_buf_overrun` |
| 4 | `csi_async_fifo_ovf` |
| 5 | `dma_cfg_has_updated` |

**全 0 只代表"无错误、无丢弃"，不代表"没有数据"。**
而且这些位是 `R/WTC/SS`（写 1 清零的粘滞位），"从没收到过任何东西"时同样是 0 ——
**两种截然不同的情况给出同一个读数**，所以它不能单独作为判据。

---

## 坑 6：**严禁用 CPU 直接读桥的数据 FIFO `0x50104000`**

我想"绕过 DMA 直接看 FIFO 里有没有像素"，于是在诊断代码里加了：

```c
*(volatile uint32_t *)0x50104000      /* ← 会挂死总线 */
```

结果**整个系统当场崩掉**：串口打出 task dump（`lpwork` 正在 Running、`nsh_main` 在等信号量），
随后板子**完全失联（ping 不通）**。

**该地址只能由 DW-GDMA 访问**，CPU load 会触发总线错误。
要判断"有没有数据"，**只能看 DMA 侧**（`dar` 是否推进 / 传输完成回调是否触发）。

---

## 坑 7：验证"有没有出帧"要用**正确的判据**

排查过程中我犯过一个**判据错误**，并因此**误报了一次成功**：

- ❌ **错的判据**：重新武装前后各采一次 `dar`。
  —— **重新武装本身就会用新缓冲区重写 `dar`**，所以 `dar` 一定变，这个判据**必然假阳性**。
  我据此报了一次 `MOVING - REARM FIXED IT`，是错的。
- ✅ **对的判据**：**武装之后**连续多次采样，看 `dar` 在**采样之间**是否移动。
  实测 4 次采样（间隔 500 ms）全部相同 → 静止。

**其他可行的判据**：
- `g_system_ticks`（`0x4ff52744`）推进 → 调度器活着；
- 帧缓冲 md5 在两次 JTAG dump 之间变化 → 帧真的在写内存
  （**注意**：不要用"非零/熵"判断，PSRAM 里的陈旧垃圾数据看起来很"像"图像，甚至含有 IRAM 指针）。

---

## 附：本次排除的假设清单（都做了实验，都证伪）

| 假设 | 实验 | 结果 |
|---|---|---|
| DSI 干扰 CSI | 完全禁用 DSI+LCD 后单独跑摄像头 | 同样失败 → 排除 |
| lane rate 不匹配 | 单次烧录内扫 9 个 `hs_freq_sel`（`0x25/06/14/15/22/19/16/0a/04`，覆盖 90–1099 Mbps） | 9 个值**全部零数据** → 排除 |
| 控制器在传感器未开流时被武装导致桥锁死 | 在传感器确认在流后 `stop`+`start` 重新武装，之后 4 次采样 `dar` | 完全静止 → 排除 |
| 传感器未真正开流 | 内部测试图案 + D-PHY 状态 | `rxclkactivehs=1`、`stopstate=0` → **传感器在发** |
| Host 缺少使能位 | 穷尽 Host 全部 **42 个寄存器**（含所有 `int_st/msk/force`、`scrambling`、`phy_ctrl`） | 无遗漏开关 → 排除 |
| 模块晶振不是 24MHz | 见上"lane rate"一行 | 排除 |

---

## 附：留给下一次复测的**诊断开关**（默认关闭，不影响正常启动）

| 文件 | 开关 | 作用 |
|---|---|---|
| `nuttx/arch/risc-v/src/common/espressif/esp_csi.c` | `#define ESP_CSI_CAPTURE_DIAG 1` | 首帧启动 3 秒后连续打印 4 组 Host/Bridge/DW-GDMA 寄存器快照 + 一行结论 |
| `nuttx/drivers/video/sc2336.c` | `#define SC2336_TEST_PATTERN 1` | 打开传感器内部测试图案，区分"采集链路坏"与"传感器不发送" |

开启后跑 `camera`，看这一行即可判定：

```
CAMDIAG[RESULT] ch0 dar 483a4dc0 -> 483a4dc0 : STATIC - no frames | brg int_raw=0 csi_en=1 buf_depth=0
```

出现 `MOVING - FRAMES ARRIVING` 即为通。

---

## 相关文件

| 文件 | 改动 |
|---|---|
| `apps/examples/camera/camera_main.c` | 坑 1/2：RAW10 缓冲尺寸、64 字节对齐 |
| `nuttx/arch/risc-v/src/common/espressif/esp_csi.c` | 诊断开关、寄存器快照、`ctlr start rc=` 日志 |
| `nuttx/arch/risc-v/src/common/espressif/esp_mipi_dsi.c` | 坑 3：DSI 让出 ch0、去掉全局复位 |
| `nuttx/drivers/video/sc2336.c` | 寄存器表校验、测试图案开关、探针回读 |
