# 踩坑笔记 #05：MIPI-DSI 显示与 LVGL 移植

> 时间范围：2026-09-15 ~ 09-16。目标：点亮 EK79007 面板（1024×600，MIPI-DSI 2 lane @900Mbps），
> 并在其上跑通 LVGL。
>
> **结论：已真机闭环** —— 面板渲染出 LVGL 控件界面（按钮/滑条/标签），
> 帧缓冲实测为背景色 + 控件像素。本文记录路径上的 5 个坑。
>
> 实机截图见 [`docs/验证截图_LVGL界面_1024x600.png`](验证截图_LVGL界面_1024x600.png)
> （1024×600，与 EK79007 面板分辨率一致，由真机帧缓冲抓取）。

硬件链路：`ESP32-P4 → MIPI-DSI(2 lane) → EK79007 驱动 IC → 1024×600 面板`，
帧缓冲在 PSRAM `0x48278d80`（1228800 B = 1024×600×2）。

---

## 坑 1：先武装 DMA、后开 DPI 输出 → 通道永久关闭（"冻屏"的真根因）

`esp_mipi_dsi_video_start()` 原来的顺序是「武装 DW-GDMA → 再开 video mode / DPI 输出」。

问题在于 **DW-GDMA 通道的目的端握手对象是桥的 FIFO**，而该 FIFO **只有在 `dpi_en` 置位后才排空**。
于是：

1. 通道武装后开始搬运，但 FIFO 不排空 → 传输 **stall**，永远不产生 `TFR_DONE`；
2. 本驱动的 LLI 是**一次性**的（`llp=0x1` 终结哨兵），**只能靠该 ISR 再武装**；
3. ISR 永不触发 → 通道永久关闭 → 面板停在"无信号"色（蓝）。

**修复**：把「开 video mode + DPI 输出 + underrun 中断」提到武装 DMA **之前**。

**实测证据**（埋点 `DSI-DMA[...]`）：

| 时刻 | isr | rst | sar | dar |
|---|---|---|---|---|
| before-arm | 0 | 0 | `00000000` | `00000000` |
| after-arm | 0 | 1 | `48278f80` | `50105000`（DSI FIFO） |
| t200ms | **15** | **16** | `48278f80` | `50105000` |

`isr=15 / rst=16` ≈ 80 Hz，即**每帧一次重启**，通道持续存活。修复前面板是纯蓝。

---

## 坑 2：`int_st_ena0` / `int_sig_ena0` 复位默认全 1，`|=` 是空操作 → 中断风暴

驱动里用 `|=` 去"使能"通道中断，但这两个寄存器**硬件复位默认值就是全 1**，
所以 `|=` 什么也没改变 —— **每一个桥 FIFO burst 都在打断 CPU**。

**实测**：200 ms 内 **13433 次 ISR**，而只对应 **23 次真实传输**（约每帧 860 次无效中断），
并且造成 **25% 丢包**。

**修复**：按 ESP-IDF 的方式用「通道状态寄存器 + 通道掩码」注册
（`esp_setup_irq_with_flags_intrstatus(..., &dev->int_st0, DW_GDMA_LL_CHANNEL_EVENT_MASK(chan), ...)`），
并把生成/传播掩码**先清零、再只置 `DMA_TFR_DONE`**。

**实测**：13433 → **15 次/200 ms**（约 900 倍）。

> 附带教训：DW-GDMA 是**共享中断源**，多条通道共用一根中断线，
> 所以不能用"独占中断"的方式注册，否则第二条通道会 `No free interrupt inputs`。

---

## 坑 3：DSI 与摄像头抢同一个 DW-GDMA 通道（跨子系统）

DSI 走**裸 LL** 直接占用通道 0，**完全不经过 `dw_gdma` 分配器**；
而摄像头的 `channel_register_to_group()` 取 `channels[]` **第一个空槽** —— 因为 ch0 没被登记，
**摄像头也拿到 ch0**。再加上 DSI 随后做的 `dw_gdma_ll_reset_register(0)` + `dw_gdma_hal_init()`
是**对整个控制器做全局复位**，会把摄像头的 ch0 配置抹掉。

**修复**：DSI 改用**通道 1**；并且当总线时钟已被别人打开时**不再做全局复位**。

详见 `踩坑笔记_07_摄像头_MIPI_CSI.md`（那里有完整证据链与 `Store/AMO access fault` 现场）。

---

## 坑 4：LVGL 输入回调无限循环 → `lv_timer_handler()` 永不返回 → 一帧都不画

这是"LVGL 界面不显示"的**元凶**。

`lv_indev_read_timer_cb` 的结构是：

```c
do { read_cb(...); } while (data.continue_reading);
```

而 NuttX 的 gt9xx 是**轮询式**驱动，`read()` **每次都返回一个完整样本**（哪怕 `npoints=0`），
移植层却写成"只要 read 成功就 `continue_reading = true`" → **LVGL 永远困在读触摸屏**
→ `lv_timer_handler()` 永不返回 → 显示刷新定时器永不执行。

**定位手段**（层层埋点，一击命中）：

```
before handler #1 tick=11890
timer cb #1 fn=0x4006e8f0 period=33      ← 解析符号 = lv_indev_read_timer_cb
```

**修复**：`lv_nuttx_touchscreen.c` 里改为**只在样本真的含触摸点时才继续读**：

```c
if (touchscreen_read_sample(touchscreen) && touchscreen->sample->npoints > 0)
```

> 这也解释了当时"CPU 空闲"的怪现象：每次 I2C 读都在等中断，看起来像空闲。

---

## 坑 5：`display_refr_timer_cb()` 用 `poll(POLLOUT)` 门控渲染

单缓冲 fb 上，NuttX 的 `fb_poll` **只在内部 panbuf 未满时**才报 `POLLOUT`，
这里**永远不成立** → `_lv_display_refr_timer()` 从不被调用。

**修复**：仅对**真正的双缓冲**（`pinfo.yres_virtual == vinfo.yres*2`）才做 `POLLOUT` 门控。

---

## 附：两个"搬运数据"层面的坑（显示与摄像头都会踩）

### 附 1：PSRAM 的 cache line 是 **64 字节**，不是 32

`esp_cache_msync()` 会拒绝任何**起始地址不是 cache line 整数倍**的缓冲。实测报错：

```
esp_cache_msync: start address: 0x483a4da0 ... not aligned with cache line size (0x40)B
```

`0x...a0` 对 32 字节对齐、对 **64 字节不对齐**。分配缓冲一律用 `memalign(64, ...)`。

### 附 2：缓冲尺寸要按**实际像素格式**算

从 RGB565 切到 RAW10 后尺寸没跟着改，驱动直接拒绝：

```
esp_csi_data_set_buf: ERROR: buffer too small: 0x4ff6eda0, 153600 < 2592000
```

`1920×1080×10/8 = 2592000`，而旧值 `320×240×2 = 153600`。
**改格式必须同步改缓冲尺寸**。

---

## 相关文件

| 文件 | 改动 |
|---|---|
| `nuttx/arch/risc-v/src/common/espressif/esp_mipi_dsi.c` | 坑 1/2/3：启动顺序、中断掩码、通道改 1 |
| `apps/graphics/lvgl/lvgl/src/drivers/nuttx/lv_nuttx_touchscreen.c` | 坑 4：输入回调条件 |
| `apps/graphics/lvgl/lvgl/src/drivers/nuttx/lv_nuttx_fbdev.c` | 坑 5：渲染门控 |
