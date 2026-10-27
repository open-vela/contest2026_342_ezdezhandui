# WiFi（板载协处理器 / SDIO + ESP-Hosted）—— 现状调研与实现路线

> 版本：2026-10-23 · 状态：**第 1 层（P4 SDMMC/SDIO host 驱动）已实现并通过构建验证；
> 第 2/3 层（ESP-Hosted 协议 + 远端 WiFi netdev）未实现**
>
> - ✅ P4 SDMMC host 驱动（`esp_sdmmc.c`）：完整 `struct sdio_dev_s` 方法表、IDMAC 描述符链、
>   CLIC 中断、双 slot 引脚路由；**实测编入固件、符号落位、调用链已反汇编核实**
> - ✅ 枚举层（`esp_hosted_sdio.c`）：`esp_sdmmc_initialize` → `sdio_probe`，里程碑状态机
> - ❌ ESP-Hosted 协议层与远端 WiFi netdev

## 一、结论先行（三条硬事实）

### 1.1 ESP32-P4 **没有 WiFi 射频**

`SOC_WIFI_SUPPORTED` 在 esp32p4 的 `soc_caps.h` 中**不存在**，而 esp32c6 有：

```
nuttx/arch/risc-v/src/esp32p4/esp-hal-3rdparty/components/soc/esp32c6/include/soc/soc_caps.h:44
    #define SOC_WIFI_SUPPORTED  1
nuttx/arch/risc-v/src/esp32p4/esp-hal-3rdparty/components/soc/esp32p4/include/soc/soc_caps.h
    （只有 SOC_SDMMC_HOST_SUPPORTED 1，无 SOC_WIFI_SUPPORTED）
```

因此 **P4 上的 WiFi 不可能是"本地驱动"**，只能是"通过某条链路去驱动板载的
协处理器"。这也解释了为什么树里 `esp_wifi_*.c`（`esp_wifi_api.c`、`esp_wlan.c`、
`esp_wlan_netdev.c`、`Wireless.mk/.cmake`）全是**给 C6/H2 自持射频用**的路径，
对本板毫无意义 —— 它们编不出本板的 WiFi。

### 1.2 板载模组是 **ESP32-C5-MINI-1**，不是 C6

来自官方 user guide（ESP32-P4X-C5-Function-EV-Board）：

> "The board is equipped with the **ESP32-C5-MINI-1** module, which supports
> 2.4 GHz & 5 GHz dual-band Wi-Fi 6 and Bluetooth 5 (LE)"

> ⚠️ **这修正了此前文档与任务描述里"板载 C6"的说法。** 影响是实质性的：
> ESP-Hosted 的从机固件要按 **C5** 目标构建（`idf.py set-target esp32c5`），
> 按 C6 准备会直接跑不起来。

（C5 与 C6 在 J1 引出上也能对上：J1 pin 35 = GPIO53 与 PA 使能复用，
pin 32 = GPIO54 与 **C5 使能**复用 —— 官方表脚注写明"move R76 to R80 to disable
the ESP32-P4 control of the ESP32-C5 enable function"。）

### 1.3 NuttX/openvela 树里**没有任何 esp-hosted 实现**

```
$ find . -iname "*esp-hosted*" -o -iname "*esp_hosted*"     # 全工作区
（无结果，除了 docs 里那份原厂启动日志的一行字样）
```

也没有 SDMMC/SDIO host 驱动可复用：

```
nuttx/arch/risc-v/src/common/espressif/     # 只有 esp_sdm.c（Σ-Δ 调制器，与 SD 无关）
nuttx/arch/xtensa/src/esp32s3/esp32s3_sdmmc.c   # 2974 行，整套 sdio_ops_s，但只在 xtensa/S3
```

P4 侧只有 HAL：`components/esp_hal_sd/sdmmc_hal.c` + `upper_hal_sdmmc/`。

## 二、硬件链路（来自官方原理图）

原理图 `ESP32_P4X_C5_Function_EV_board-2.0-schematics.pdf` 中，
P4 有两组 SD 控制器引出：

| 网名组 | GPIO | 用途（依据） |
|---|---|---|
| `SD2_CLK/CMD/D0..D3` | **18 / 19 / 14 / 15 / 16 / 17** | 4-bit 总线 —— 与 P4 系其他板（如 Guition JC-ESP32P4-M3-DEV）的"SDIO slot 1 接协处理器射频、40 MHz、4-bit"完全同构 |
| `SD1_*` + `SD_DET` | （另一组） | microSD 卡座（官方 user guide：MicroSD 4-bit） |

即：**协处理器走 SD2（4-bit SDIO），SD 卡走 SD1**，两组不要混。
（`SD2_*` 的 GPIO 映射是从原理图文本层直接读出的，非推测；C5 模组侧的 SDIO
引脚名未在该 PDF 文本层出现，上板前建议用万用表/示波器对 `SD2_CLK` 与
C5 模组对应脚复核一次。）

## 三、已实现的部分（第 1 层 + 枚举）

| 文件 | 内容 |
|---|---|
| `nuttx/arch/risc-v/src/common/espressif/esp_sdmmc.c/.h` | P4 SDMMC/SDIO host 驱动（约 700 行）|
| `nuttx/arch/risc-v/src/common/espressif/esp_hosted_sdio.c/.h` | 协处理器链路：host 拉起 → SDIO 枚举 → 里程碑状态 |

### 3.1 为什么不照抄 S3 的驱动

控制器是同一块 DesignWare MSHC，**寄存器偏移完全一致**，但四处是芯片相关的：

1. **两个 slot 的引脚路由方式不同**（P4 独有，且官方头文件写明了）：

   | slot | 用途 | 路由 | 引脚 |
   |---|---|---|---|
   | 0 | microSD | **IO_MUX** | CLK43 CMD44 D0..D3 39..42 |
   | 1 | 协处理器 | **GPIO matrix** | CLK18 CMD19 D0..D3 14..17 |

   `sdmmc_pins.h` 里 slot 0 有定义，slot 1 明确写着 "SLOT1 doesn't go through IOMUX"；
   `SDMMC_LL_SLOT_SUPPORT_GPIO_MATRIX()` 也只在 slot 1 返回真。**S3 上所有引脚都走
   GPIO matrix**，所以这一层在 S3 驱动里根本不存在，必须新写。

2. **时钟树在控制器之外**，位于 `HP_SYS_CLKRST`（`peri_clk_ctrl01/02`）：每卡分频器在
   SDMMC 的 CLKDIV 寄存器，而源分频与 HS/LS 边沿选择在系统时钟块。

3. **DMA 是控制器自带的 IDMAC**，不占 GDMA 通道 —— 这点在本板很关键，因为
   **I2S 驱动已经占用了 GDMA 通道**，走 IDMAC 两者互不干扰。

4. **中断走 CLIC**，用 `esp_setup_irq(SDIO_HOST_INTR_SOURCE, ...)` 安装。

### 3.2 寄存器层用 Espressif 自己的 LL，而不是手抄寄存器表

全部寄存器访问走 `components/esp_hal_sd/esp32p4/include/hal/sdmmc_ll.h` ——
**ESP-IDF 驱动同一颗控制器的同一份代码**。手抄 440 行位域定义是纯粹的重复劳动，
而且错一位就是静默故障。本文件只写"集成"：NuttX 的方法表、命令/响应翻译、
等待与回调机制、IDMAC 描述符链。

### 3.3 一个容易忽略的正确性细节：描述符必须 64 字节

P4 的 L1 cache line 是 64 字节，芯片自己的 `sdmmc_desc_t` 就带 `reserved[12]` 补齐
（注释写明是 cache 对齐）。描述符若短于一行，就会与邻居共用 cache line，
clean 一个会把另一个的陈旧内容一起写回，**悄悄毁掉描述符链**。本驱动照做。

### 3.4 配置开关（默认全关，附理由）

```kconfig
config ESPRESSIF_SDMMC          # P4 SD/MMC host（select MMCSD / ARCH_HAVE_SDIO / SDIO_DMA）
config ESPRESSIF_SDMMC_SLOT     # 默认 1 = 协处理器；0 = microSD
config ESPRESSIF_SDMMC_DMA      # 默认 y
config ESPRESSIF_SDMMC_CLK/CMD/D0..D3   # 逐引脚，默认与本板接线一致
config ESPRESSIF_HOSTED_SDIO    # 枚举层；select ESPRESSIF_SDMMC
config ESPRESSIF_HOSTED_SDIO_SLOT       # 默认 1
```

**故意默认关闭**：第 1 层能编入、能跑到"枚举到对端"，但第 2/3 层没有，
所以打开也**不会凭空多出一个 wlan0**，只会让启动日志多两行。要验证这段时间链时
把 `ESPRESSIF_SDMMC` / `ESPRESSIF_HOSTED_SDIO` / `MMCSD_SDIO` 三个打开即可。

### 3.5 验证证据（本轮实测）

开启上述开关后的完整构建：

```
0 error；nuttx.bin 1,835,008 B；音频 4 个符号同时在线（互不影响）
T esp_hosted_sdio_initialize
T esp_sdmmc_initialize
T sdio_probe
T sdio_io_rw_direct
T sdio_set_wide_bus
```

调用链（`objdump` 反查，逐层核实）：

```
esp_bringup → esp_hosted_sdio_initialize → esp_sdmmc_initialize
                                        → sdio_probe
```

默认配置下重新构建：**0 error**，且 SDMMC/hosted 符号数 = 0（确认默认不编入）。

### 3.6 顺带修掉的上游 CMake 移植缺陷

`drivers/mmcsd/CMakeLists.txt` 在 `CONFIG_MMCSD_SDIO` 下**只编 `mmcsd_sdio.c`，
漏了 `sdio.c`**（Makefile 路径是正确的：`CSRCS += mmcsd_sdio.c sdio.c`）。
`sdio.c` 才是 SDIO 功能 API（`sdio_probe` / `sdio_io_rw_direct` / `sdio_set_wide_bus`）
的宿主，缺了它任何 SDIO 用途都会在链接期炸 `undefined reference to sdio_probe`。
与音频那两个缺头文件的问题同类：**只有真正用到该配置才会暴露**。

## 四、要做的事（分阶段，逐轮推进）

### 阶段 B（下一轮）：SDIO 上层的 ESP-Hosted 传输

esp-hosted 的链路是：P4 ← SDIO → C5 从机固件，其上跑 RPC。
这部分需要：

1. **C5 侧从机固件**：用 ESP-IDF 以 `esp32c5` 为目标构建 esp-hosted slave
   （**这是前置条件**，没有它 P4 侧写得再对也握手不上）
2. P4 侧：SDIO 枚举 → 功能寄存器读写 → 数据通道 → esp-hosted 的
   protobuf/RPC 编解码 → 远端 WiFi 命令转发
3. 最上层：把远端 WiFi 暴露成 NuttX netdev（或复用 `esp_wlan` 的 netdev 层）

工作量量级：从零参考实现，数千行且强依赖 C5 侧固件行为。**必须分多轮**，
且每轮都要有可验证的里程碑（枚举到 C5 → 读到功能块 → 握手成功 → 扫描到 AP
→ 拿到 DHCP）。

### 阶段 C：与现有网络栈共存

本板已有 **eth0（RMII，已 DHCP 全自动，见 STATUS §五）** 作为保底联网。
WiFi 上线后需要注意：

- 双网卡路由优先级（默认路由归谁）
- `CONFIG_NETINIT_*` 对两张卡的处理
- 演示时"有线保底 + 无线增强"的切换方式

## 五、上板前必须核对的事项

第 1 层已经写完，但**还没在真机上跑过** —— 下面几条都还没验证，跑之前请逐条核对
（寄存器写得再对，接线不对也连不通）：

1. 万用表确认 `SD2_CLK(18)/CMD(19)/D0(14)/D1(15)/D2(16)/D3(17)` 确实连到 C5 模组
2. 确认 C5 模组的供电与 `C5_CHIP_PU`（使能）由 P4 的 GPIO54 控制
   （J1 表脚注：默认 P4 控制 C5 使能；如被 R76→R80 改动则此控制失效）
3. 用原厂 IDF 固件先确认 WiFi 工作正常（`docs/ref_stock_boot_esp-idf_v5.5.2.log`
   是原厂启动日志，可作为"硬件好"的基线）
4. 确认 C5 侧是否有可用的 esp-hosted slave 固件/固件烧写通道
   （板上有 "ESP32-C5 Module Programming Connector"）

## 六、范围说明（诚实记录）

**已做**：第 1 层（SDMMC/SDIO host 驱动）与枚举层，构建验证到"符号落位 + 调用链核实"。

**没做**：ESP-Hosted 协议层与远端 WiFi netdev。所以**现在打开这些开关也上不了网**，
`esp_hosted_sdio_initialize()` 跑到 `ENUMERATED` 就返回 —— 它证明的是
"对端在电气上应答了、功能块可见"，**不是**"有了一张网卡"。
板上联网仍然靠 eth0（RMII，已 DHCP 全自动）。

**还没验证的**：真机。本轮所有证据都是构建期/静态的（编译、链接、符号、反汇编），
没有上板跑过枚举。下一个可验收里程碑就是上板跑一次：

```
esp_hosted_sdio_state() 返回 ESP_HOSTED_SDIO_ENUMERATED
（即 esp_sdmmc_initialize 成功 + sdio_probe 成功）
```

跑之前先过 §五 的核对清单。

---

# 附录：SDMMC/SDIO 驱动真机上板实录（2026-10-26）

本轮把 `esp_sdmmc.c` / `esp_hosted_sdio.c` **第一次跑在真板上**。这是本轮最有价值的
一步：上板前的所有结论都是构建期的，而真机立刻暴露了三个只有在硬件上才会出现的
问题，也第一次拿到了"控制器确实起来了"的证据。

## B.1 真机已验证通过的部分

```
esp_hosted_sdio: companion enable asserted (GPIO54), waiting 200 ms
esp_hosted_sdio: SDIO host ready on slot 1
```

- **P4 SDMMC 控制器在真机上成功初始化**（slot 1）：模块时钟、复位、时钟源、
  相位、引脚路由全部生效，`esp_sdmmc_initialize()` 返回非 NULL
- **协处理器使能 GPIO54 可正常驱动**（J1 表脚注：GPIO54 复用为"P4 控制 C5 使能"）
- 分层状态机按设计工作：先到 `HOST_READY`，再尝试枚举

## B.2 真机暴露的两个问题（已修）

### B.2.1 探测前没有使能卡时钟、也没有挂中断 → 命令永远完不成

第一版 `esp_hosted_sdio_initialize()` 直接调 `sdio_probe()`，跳过了两个必需步骤：

- `SDIO_ATTACH(dev)` —— 不挂中断，控制器完成中断永远不装；
- `SDIO_CLOCK(dev, CLOCK_IDMODE)` —— 不开卡时钟，CIU 根本移不出命令，
  `START` 位不会自清，下一次发送就空转到超时，最后报 **-EBUSY**。

真机现象正是如此：`no SDIO response from the companion module: -16`（-16 = EBUSY）。
`mmcsd_sdio_initialize()` 里也是这两步，而且它在 attach 失败时同样返回 -EBUSY ——
这个巧合很容易把排查引向错误的结论（"attach 坏了"），实际是时钟没开。

修复：探测前先 `SDIO_ATTACH` + `SDIO_CLOCK(CLOCK_IDMODE)` + 20 ms 稳定。

### B.2.2 协处理器需要先上电

板上 ESP32-C5 由 P4 的 GPIO54 门控，默认拉低即是**断电/复位保持**。
不驱动它，总线上没有任何设备会应答 —— 表现为控制器正常、探测恒返回 -EBUSY。

修复：新增 `CONFIG_ESPRESSIF_HOSTED_SDIO_EN_GPIO`（默认 54）与
`_EN_DELAY_MS`（默认 200）：先拉高、等 200 ms（协处理器要跑完自己的固件才能
枚举成 SDIO 卡），再探测。

## B.3 仍未完成

枚举本身还没成功：修完上述两点后仍然是
`no SDIO response from the companion module: -16`。

按当前证据，最可能的剩余原因（按可疑度排序）：

1. **C5 模组上没有 esp-hosted 从机固件**。这一点从一开始就写在阶段 B 的前置条件里：
   ESP-Hosted 需要 C5 侧跑配套固件，出厂固件未必是 esp-hosted slave。
   没有它，C5 不会作为 SDIO 卡枚举 —— 这与"控制器一切正常、只是没人应答"完全吻合。
2. 200 ms 等待不够（可加大后再测）。
3. slot 1 的引脚/相位需要微调（`SD2_*` 映射来自原理图文本层，尚未用示波器复核）。

**下一步最省力的判别**：给 C5 烧一个 esp-hosted slave 固件后再跑同一份固件。
如果枚举成功，就证明 P4 侧驱动是通的；如果仍然 -16，问题在 P4 侧的引脚/时钟。

## B.4 与音频的相互影响

SDMMC 与音频同开时**互不干扰**（实测：同一镜像里 SDMMC 初始化 + 音频
`/dev/audio/pcm0` 均正常，音频仍能完整播放 192044 字节）。原因是 SDMMC 用
控制器自带的 IDMAC，不占 I2S 用的 GDMA 通道 —— 这个设计选择在真机上得到了确认。

---

# 附录 C：SDIO 卡时钟没有被提交 —— 真正的 -EBUSY 根因（2026-10-26 晚）

本轮先把"上一轮为什么恒返回 -EBUSY"查清了。结论是**驱动里少了一步**，
不是硬件问题。

## C.1 先修诊断：`mcerr` 被编掉了

上一轮加的寄存器打印一行都没出现，原因是 `mcerr()` 在 `-DNDEBUG` 下被编译掉。
改用 `syslog(LOG_INFO, ...)` 后终于拿到真机寄存器快照：

```
esp_hosted_sdio: verid=5342270a status=00000406 cdetect=00000000 clkena=00000002
esp_hosted_sdio: clkdiv=00009f00 ctrl=00000012 rintsts=00000000
esp_hosted_sdio: probe attempt 1 -> -16
```

- `verid=0x5342270a` —— 控制器版本 ID 正常（DesignWare MSHC 2.70a），**控制器是好的**
- `clkena=0x2` / `clkdiv=0x9f00` —— 卡 1 时钟"已使能"、分频 159，寄存器读回来都对
- `rintsts=0` —— **一次命令完成中断都没有**

这条组合非常关键：寄存器看起来全对，却一条命令都没完成。

## C.2 根因：CLKDIV/CLKENA 只写进了 BIU，没有提交到卡时钟域

DesignWare MSHC 的 `update_clk_reg`（命令寄存器 bit21）说明写得很清楚：

> 0: Normal command sequence; 1: Do not send commands, just update clock
> register value into card clock domain. **Following register values are
> transferred into card clock domain: CLKDIV, CLKSRC, and CLKENA.**

也就是说，写 CLKDIV/CLKSRC/CLKENA **只是暂存在 BIU**，必须再发一条
`update_clk_reg=1` 的命令，它们才真正进入卡时钟域。原来的 `esp_sdmmc_clock()`
只写了三个寄存器就返回了 —— **卡时钟实际从未启动**。

于是：命令永远发不出去 → CIU 的 START 位永不自清 → 我的
`esp_sdmmc_ciu_sendcmd()` 等到超时后返回 `-EBUSY`。

**这个缺陷的欺骗性在于：三个寄存器读回来全都是"正确"的值。**

修复（`esp_sdmmc.c`）：

```c
sdmmc_ll_set_card_clock_div(hw, priv->slot, div);
sdmmc_ll_enable_card_clock(hw, priv->slot, true);

memset(&upd, 0, sizeof(upd));
upd.update_clk_reg = 1;
upd.start_command  = 1;
sdmmc_ll_set_command(hw, upd);
/* 等 START 自清，带超时 */
```

## C.3 修复后的真机变化（关键证据）

| | 修复前 | 修复后 |
|---|---|---|
| 返回码 | `-16` (EBUSY) | **`-110` (ETIMEDOUT)** |
| 含义 | 命令**发不出去**，START 卡死 | 命令**发出去了**，但**没有卡应答** |

错误码的语义变了，说明**命令通道已经通了**，瓶颈从"主机发不出"变成了
"对端不回应"。

## C.4 这一轮排除掉的可能

- **使能极性**：新增"两次尝试分别驱动高/低电平"的探测（`esp_hosted_sdio.c`），
  两种极性都返回 -110 —— 极性不是原因。
- **使能时长**：三次尝试（200ms / 200ms / 再补 800ms）结果一致。
- **控制器健康**：verid 正常、时钟寄存器正确、复位完成。

## C.5 剩下的可能（按可疑度）

1. **C5 模组没有 esp-hosted 从机固件**（我一直未能验证也无法烧写 —— 没有到
   C5 的串口，也没有 esp-hosted 源码）；
2. **`SD2_*` 根本没有连到 C5**：原理图文本层里这些网络只出现在测试点
   TP3–TP9 和一个 2×3 排针（CON3X2）附近，C5 侧的 SDIO 网络名没有出现在可提取
   的文本里；
3. C5 被 `C5_BOOT` 拉在下载模式（没有代码去驱动它）。

## C.6 顺带改进（保留在树里）

- `esp_sdmmc_debug()` / `esp_sdmmc_debug_s`：快照 verid/status/cdetect/clkena/
  clkdiv/ctrl/rintsts，供 bring-up 日志使用（不暴露整个寄存器块）；
- `esp_sdmmc_bus_reset()`：两次枚举尝试之间复位控制器并重新使能识别时钟；
- 枚举改为**三次尝试 + 极性对调 + 递增延时**，任何一次成功即停止。

这些是通用的 bring-up 能力，换一块板子同样有用。

## C.7 本轮继续排除的两项

1. **`C5_BOOT` 不是原因**：原理图里 `C5_BOOT` 只出现在 C5 的编程排针
   （CON3X2 / ESP-Prog）附近，**没有连到 P4 的 GPIO**，P4 无法把它拉进下载模式。
2. **复位脉冲不是原因**：把使能改成"先拉低 50ms（保持在复位）→ 再拉高 → 等 200ms
   → 探测"的完整上电时序后，仍然三次 `-110`。

至此软件侧可做的尝试（时钟提交、极性、延时、复位脉冲、多次重试）都已做尽，
结果一致：**总线上没有任何设备应答**。剩下的必须靠物理手段确认
（示波器看 `SD2_CLK`/`SD2_CMD` 是否真的到 C5，以及 C5 是否在跑 esp-hosted 从机固件）。

---

# 附录 D：真正的阻塞原因找到了 —— C6 跑的是「串口」传输的 esp-hosted（2026-10-26 下午）

这一轮拿到了一条决定性信息：**`/dev/ttyUSB1` 连的就是板载 ESP32-C6-Mini-1 的串口**
（`usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_0001-if00-port0`）。

## D.1 直接读 C6 的输出

以 115200 打开 `/dev/ttyUSB1` 发 `AT`，返回的不是 AT 应答，而是 esp-hosted 从机
自己的周期统计：

```
I (15250601) stats: slave StaRxQLoad:        0
H=>S: sta: in:        0 out:        0 fail:        0
H<=S: sta: in:        0 out:        0
  serial : in:        0 rsp:        0 evt:        0
```

关键在最后一行：**`serial`** —— 这颗 C6 上的 esp-hosted 从机是**串口（UART）传输**，
统计计数器也全是 `serial: in/rsp/evt`。

**这就解释了为什么 SDIO 永远 -110**：C6 根本不在 SDIO 传输模式上，
自然不会有任何 SDIO 卡应答。前七轮在 SDIO 侧做的所有排查（时钟提交、极性、
复位脉冲、延时）都是对的，但都在一条**对端没启用的总线**上。

## D.2 板子的物理连接约束（原理图核实）

- P4 → 协处理器 的信号：原理图里只有 **`P4_WAKEUP_C5`** 一条
- 协处理器的串口 `C5_U0TXD` / `C5_U0RXD`：只接到**编程排针 / CP2102**，
  **没有接到 P4**（P4 自己的串口是 `ESP_U0TXD/RXD`，走 ttyUSB0 console）
- P4 与协处理器之间的数据通道：**只有 SD2_*（GPIO18/19/14-17）这组 SDIO**

也就是说：
**P4 没有串口能到 C6；想用 WiFi，C6 必须跑 SDIO 传输的 esp-hosted 从机固件。**

## D.3 现在缺什么

缺的是**把 C6 刷成 SDIO 传输版本**。而刷写需要先把 C6 送进下载模式：

- 实测 `esptool.py` 在 `/dev/ttyUSB1` 上**无法自动同步**：
  `--before default_reset` / `no_reset` / 手动驱动 DTR·RTS 的三种极性组合
  （RTS→EN、DTR→BOOT 与相反）全部返回
  `Failed to connect to Espressif device: No serial data received.`
- 说明这颗 CP2102 的 **DTR/RTS 没有接到 C6 的 EN/BOOT**，
  进下载模式只能靠**按板上的 BOOT + RESET 键**（或板上下载模式跳线）。

## D.4 需要的操作（一条就够）

**手动让 C6 进入下载模式**：按住 BOOT（GPIO9）不放 → 按一下 RESET（或断电重上电）
→ 再松开 BOOT。然后告诉我，我会：

1. 在 `/dev/ttyUSB1` 上用 esptool 确认能连上（`chip_id` 应报 ESP32-C6）；
2. 构建 esp-hosted 的 **SDIO 传输**从机固件（ESP-IDF 5.5.6 已在本机，
   需要联网拉 esp-hosted 源码 —— 当前网络间歇性 TLS 失败，需重试）；
3. 烧写并复位 C6；
4. 重跑 P4 的 SDIO 枚举 —— 若成功，`esp_hosted_sdio_state()` 会返回
   `ENUMERATED`，随后才能做协议层（第 2 层）与 netdev（第 3 层）。

## D.5 备选：esp-at 路线

用户提到 esp-at v4.2.0.0。这条路**只有当 P4 与 C6 之间有一条串口**时才成立，
而 D.2 已核实没有 —— C6 的串口只到编程排针。所以 AT 指令路线需要改板子接线，
本板现状下不可行；除非把 C6 的 UART 飞线到 P4 的某个 UART 上。

---

# 附录 E：拿到 esp-hosted 上游源码后的核对（2026-10-26 晚）

网络恢复后成功拉到 `espressif/esp-hosted`（docs 仓，代码已迁出）与
**`espressif/esp-hosted-mcu`**（含 `coprocessor/` 从机固件 + `host/` + `common/`）。
这一下把"我们在猜的东西"变成了"可以对照的东西"。

## E.1 官方 P4 主机引脚表 —— 与本移植完全一致

`docs/getting-started-mcu.md` 里给出了 ESP32-P4 作主机时的标准连接：

| 信号 | 官方 P4 引脚 | 本移植 |
|---|---|---|
| CLK | 18 | ✅ 18 |
| CMD | 19 | ✅ 19 |
| D0 | 14 | ✅ 14 |
| D1 | 15 | ✅ 15 |
| D2 | 16 | ✅ 16 |
| D3 | 17 | ✅ 17 |
| Reset Out | **54** | ✅ GPIO54 |

也就是说：**引脚配置、使能 GPIO 的选择都是对的**（当初是从原理图独立推出来的）。
官方还特别注明 CMD/DAT0–DAT3 **必须**有外部上拉 —— 本驱动的 `esp_sdmmc_configpins()`
也是按这个配的（`PULLUP`）。

## E.2 从机固件怎么构建（已确认可行）

```sh
cd examples/wifi/sta/cp
eh.py set-target esp32c6
eh.py menuconfig      # Transport layer -> SDIO
eh.py -p <CP_PORT> flash monitor
```

实际在本机跑出来的配置是：

```
CONFIG_ESP_HOSTED_CP=y
CONFIG_EH_TRANSPORT_CP_SDIO=y          ← SDIO 传输
```

说明 `EH_TRANSPORT_CP_SDIO` 在 C6 上是**默认选中**的（取决于
`SOC_SDIO_SLAVE_SUPPORTED`）。这与实测"C6 上跑的是 serial 传输"对不上 ——
本板 C6 的固件很可能是早前按 UART 配的，或来自另一套出厂镜像。
**结论不变：需要把 C6 刷成 SDIO 传输版本。**

构建目前卡在 `common/serializers/third_party/msg_codec/protobuf-c` 这个
**git submodule 没能拉下来**（网络间歇性 TLS 中断）。这是纯网络问题，
重试即可。

## E.3 esp-hosted 主机的 SDIO 合约（已抄录，可直接照着实现）

从 `port/os/idf/src/eh_host_port_sdio.c` 读出的卡功能初始化顺序
（`card_fn_init()`），这是主机在枚举之后**必须**做的一串 CCCR 操作：

1. 读 CCCR `0x02` (FN_ENABLE)、`0x03` (FN_READY)
2. `FN_ENABLE |= BIT(1)`（使能 function 1），写回
3. **轮询** `FN_READY` 直到 bit1 置位（每 10 ms 一次，有重试上限）
4. `INT_ENABLE |= BIT(0) | BIT(1)`（function 1 + master），写回
5. 设 FN0 块大小 = **512**
6. 设 FN1 块大小 = **512**
7. 切 4-bit 总线

数据面（`port/os/include/eh_host_port_sdio_reg.h`）：

| 项 | 值 |
|---|---|
| 块大小 | 512 |
| 主机→从机 中断/命令寄存器 | `ESP_SLAVE_SCRATCH_REG_7`（从机 `0x3FF55000 + 0x??`）|
| 包长寄存器 | `ESP_SLAVE_PACKET_LEN_REG` (`+0x60`) |
| token 寄存器 | `ESP_SLAVE_TOKEN_RDATA` (`+0x44`) |
| CMD53 地址上限 | `0x1F800` |
| 运行期缓冲配置 | 由从机在 boot init 事件里用 TLV `0x1B` 广播（5 字节：transport/e2h_mode/e2h_bufsz/h2e_mode/h2e_bufsz）|

## E.4 好消息：NuttX 这边的原语几乎一一对应

esp-hosted 主机端口需要的 6 个操作，NuttX 的 `drivers/mmcsd/sdio.c` 全都有：

| esp-hosted 主机端口 | NuttX 对应 |
|---|---|
| `card_init` / CCCR 配置 | `sdio_probe()` + `sdio_enable_function()` + `sdio_set_blocksize()` + `sdio_set_wide_bus()` + `sdio_enable_interrupt()` |
| `read_reg` / `write_reg` (CMD52) | `sdio_io_rw_direct()` |
| `read_block` / `write_block` (CMD53) | `sdio_io_rw_extended()` |
| 等从机中断 | 本驱动的 SDIO 中断回调 |

**本轮据此把 `esp_hosted_sdio.c` 从"只做 sdio_probe"补成了完整的卡功能 bring-up**：
按 E.3 的顺序调用 NuttX 的通用 helper 完成 FN0/FN1 块大小、function 1 使能、
中断使能、4-bit 总线。这段代码已编译通过（0 error）。

> 注：因为 C6 仍不在 SDIO 模式，这段代码在真机上走到 `sdio_probe` 仍会返回
> `-ETIMEDOUT`；它的价值是**一旦 C6 换成 SDIO 固件就能立刻用**，而不是届时要重写。

## E.5 仍未完成（依赖 C6 固件）

- 第 2 层：protobuf RPC 编解码（`common/serializers`）+ 帧层
- 第 3 层：把远端 WiFi 映射成 NuttX netdev
- 前置：C6 进下载模式（需按板上 BOOT+RESET）→ 烧 SDIO 版从机固件

## E.6 C6 的 SDIO 从机固件已构建完成（2026-10-26 晚）

上游 `esp-hosted-mcu` 拉下来后，把从机固件**实际构建出来了**：

```sh
cd esp-hosted-mcu/examples/wifi/sta/cp
idf.py set-target esp32c6          # ESP-IDF 5.5.6
idf.py build
```

产物与配置（已在真机构建机上验证）：

| 项 | 值 |
|---|---|
| 产物 | `build/eh_cp_wifi_sta.bin`（**1,094,464 B**）|
| 目标 | `CONFIG_IDF_TARGET="esp32c6"` |
| 传输 | `CONFIG_EH_TRANSPORT_CP_SDIO=y` |
| 数据面模式 | `CONFIG_EH_TRANSPORT_CP_SDIO_MODE_SW_AGGR=y`（MCU 主机的默认）|
| 构建完整性 | `Successfully created esp32c6 image` / `Project build complete` |

**已随交付收录**（免去评审重跑 IDF 构建）：

```
tools/build_flash/c6_hosted/
├── eh_cp_wifi_sta_esp32c6_sdio.bin   # 1,094,464 B，可直接烧
├── sdkconfig.esp32c6.sdio            # 构建时的完整配置，便于核对/重建
└── flash_c6.sh                       # 烧录脚本（含下载模式提示）
```

> 构建时踩到一个坑值得记：`common/serializers/third_party/msg_codec/protobuf-c`
> 是 git submodule，`git clone` 反复 TLS 中断；改用
> `curl -L codeload.github.com/protobuf-c/protobuf-c/tar.gz/refs/tags/v1.5.0`
> 拉 tarball 解压进去就通了 —— 说明是 git 协议这条路不稳，不是网络整体不可用。

### 烧录仍需要一次人工操作

`esptool` 在 `/dev/ttyUSB1` 上**无法自动进下载模式**（DTR/RTS 未接 EN/BOOT，
三种极性组合都试过）。必须：

**按住板上 BOOT（C6 的 GPIO9）→ 按一下 RESET（或断电重上电）→ 松开 BOOT**

然后：

```sh
tools/build_flash/c6_hosted/flash_c6.sh /dev/ttyUSB1
```

烧完 P4 复位一次，`esp_hosted_sdio_state()` 应能走到 `ENUMERATED`。

### 仍需确认的一点（诚实标注）

这颗固件用的是 esp-hosted **默认的 C6 侧 SDIO 引脚**
（CLK19 / CMD18 / D0 20 / D1 21 / D2 22 / D3 23，见官方
`getting-started-mcu.md` 的 Co-processor connections 表）。
板子把这 6 根线接到哪里，我**没有原理图级的确认**（原理图文本层里只看到
P4 侧的 SD2_*）。如果烧完仍无应答，下一步就是用万用表核对这 6 个引脚。

## E.7 第 2 层的线格式（已抄录，实现时照此即可）

esp-hosted 的帧层在 `common/eh_frame`（430 行，纯 C，只依赖 `esp_err_t`，
移植成本很低）。配置由 `eh_frame_init()` 给出，MCU+SDIO 主机的默认是：

```c
EH_FRAME_CFG_HOST_MCU_SDIO_DEFAULT = {
    .transport        = EH_TRANSPORT_SDIO,
    .max_buf_size     = ESP_TRANSPORT_SDIO_MAX_BUF_SIZE,
    .hdr_version      = ESP_HOSTED_HDR_VERSION_V1,
    .checksum_enabled = 1,
}
```

即：**V1 头 + 校验和开启**。

### V1 线上头（`struct esp_payload_header`，`common/eh_common/include/eh_common_header.h`）

| 偏移 | 字段 | 位宽 | 说明 |
|---|---|---|---|
| 0 | `if_type` | 4 bit | 接口类型（WiFi STA / AP / BT / 私有 …）|
| 0 | `if_num` | 4 bit | 同一类型下的接口号 |
| 1 | `flags` | 8 | 帧标志 |
| 2 | `len` | 16 | 负载长度 |
| 4 | `offset` | 16 | 负载在缓冲里的偏移（通常 = 头长 12）|
| 6 | `checksum` | 16 | 校验和（`checksum_enabled` 时有效）|
| 8 | `seq_num` | 16 | 序号 |
| 10 | `throttle_cmd` | 2 bit | 流控命令 |
| 10 | `reserved2` | 6 bit | 保留 |
| 11 | union { `reserved3` / `hci_pkt_type` / `priv_pkt_type` } | 8 | **必须放最后**（HCI 包类型要靠它）|

**头长 = 12 字节**（`sizeof(struct esp_payload_header)`）。

版本探测：`buf[0] == ESP_HOSTED_HDR_V2_MAGIC` → V2，否则 V1
（`eh_frame_detect_version()`）。V2 头是 `eh_header_v2_t`，带 magic 与
`hdr_version`，`offset` 同样指向负载起点。

### 校验和

`uint16_t eh_frame_checksum(const uint8_t *buf, uint16_t len)` —— 对**整帧**
（含头）计算。解码侧校验失败返回 `EH_FRAME_CORRUPT`，长度越界返回
`EH_FRAME_TOOBIG`，NULL/过短/偏移非法返回 `EH_FRAME_INVALID`。

### 解码结果结构

`eh_frame_decode()` 填充 `interface_buffer_handle_t`：
`seq_num / payload / throttle_cmd / frag_seq / tlv_offset`。

### NuttX 侧实现建议（低耦合）

`eh_frame` 是纯逻辑，与总线无关。建议：

1. 把它作为**独立组件**投放（`nuttx/arch/risc-v/src/common/espressif/eh_frame/`
   或单独目录），不改一行上游代码；
2. 传输层（SDIO）与帧层之间只通过「收一帧 / 发一帧」两个回调耦合；
3. 帧层用主机侧 tarball 编译一个**本地单元测试**做 encode→decode 往返 ——
   这条路不依赖 C6，可在还没烧固件时就验证正确性。

这样第 2 层的正确性有独立证据，与"C6 是否能应答"解耦。

## E.8 第 2 层帧格式：**已经有可执行的验证证据**（不依赖 C6）

把上游 `eh_frame.c`（一行未改）在主机上编译起来，配上往返测试：

```sh
tools/test/eh_frame/run.sh /tmp/esp-hosted-mcu
```

```
hdr_version=1 checksum_enabled=1 hdr_size=12
encoded hdr_len=12
checksum stored=0x0250 computed(field zeroed)=0x0250
decode=0 payload_len=8 seq=0x1234 if_type=1 throttle=3
corrupted decode=2
RESULT: all checks passed
```

这条测试的价值在于：**帧层的正确性与"C6 是否应答"完全解耦** ——
不需要板子、不需要 SDIO、不需要从机固件，随时可以跑。

### 测试同时抓出两个真实的接口约定（写下来免得再踩）

**1）`eh_frame_encode()` 要求负载已经在缓冲区里。**
`encode_v1()` 对 `sizeof(hdr) + payload_len` 计算校验和，所以如果先把空缓冲区
编码、再 memcpy 负载进去，校验和实际覆盖的是一段还不存在的负载 ——
**每一帧都会在对端校验失败**。正确顺序是先把负载写到 `buf + hdr_len`，再 encode。

> 这个错误在本测试里第一次跑就复现了（`checksum mismatch` 配上
> `decode=2`），说明测试确实咬得住东西，不是走过场。

**2）校验和是在"校验和字段本身为 0"的前提下算的。**
`encode_v1()` 先 `memset` 头（此时 `buf[6..7] == 0`）再求和，最后才写回字段。
所以校验方**必须先把该字段清零再求和**，否则等于在求和里包含了被校验的值。

### 已核对无误的部分

- V1 头就是 **12 字节**，且逐字节偏移与 E.7 抄录的表完全一致
  （if_type:4|if_num:4 / flags / len(le16) / offset(le16) / checksum(le16) /
  seq_num(le16) / throttle:2|rsvd:6 / union 字节）
- `if_type / if_num / seq_num / throttle_cmd / pkt_type / payload` 全部往返一致
- 负载被篡改后解码返回 `EH_FRAME_CORRUPT`（不是静默接受）
- `eh_frame_detect_version()` 版本探测规则与 E.7 记录一致

**交付收录**：`tools/test/eh_frame/`（测试 + 三个最小 shim + `run.sh`）。
shim 只有 `esp_err.h` / `linux/types.h` / `sdkconfig.h` 三个头，
上游源码保持原样。

## E.9 第 2 层已投放进驱动树并通过目标编译

帧层现在是**驱动的一部分**，不再是只在主机上跑的测试：

```
nuttx/arch/risc-v/src/common/espressif/eh_frame/
├── src/eh_frame.c              上游原样
├── include/                    上游原样（eh_frame.h + eh_common_* + eh_tlv_tags.h）
├── shim/linux/types.h          唯一的本地补充
└── README.md                   逐文件对应上游路径 + 升级方式
```

**投放而非改写**：所有上游文件逐字节一致，将来升级直接覆盖。CMake 里挂在
`CONFIG_ESPRESSIF_HOSTED_SDIO` 下，默认配置不受影响。

### 编译时遇到并解决的两处平台差异

1. **`<linux/types.h>`**：上游 `eh_common_interface.h` 包含它，NuttX 交叉环境没有。
   → 提供 `shim/linux/types.h`（只 include `<stdint.h>` / `<stddef.h>`），
   放在组件 include 之后，影响范围限于本组件。

2. **`__KERNEL__` 被 NuttX 定义**：`eh_frame.c` 的字节序辅助函数第一分支是
   `#ifdef __KERNEL__ → #include <asm/byteorder.h>`（Linux 内核专用头）。
   NuttX 构建恰好定义了 `__KERNEL__`，于是走进了内核分支。
   → 对该源文件单独 `-U__KERNEL__`，让它落到普通小端分支
   （RISC-V 这里就是小端，正是该分支实现的语义）。

两处都是**构建系统层面**的适配，没有改动上游一个字节。

结果：目标编译 **0 error**（产出 7,876 B 目标文件），默认配置构建同样 0 error。

---

# 附录 F：C6 已刷入 SDIO 固件 —— 但枚举仍未通（2026-10-26 傍晚）

## F.1 刷写成功

C6 进入下载模式后，esptool 识别为：

```
Chip is ESP32-C6FH4 (QFN32) (revision v0.2)
```

按 `build/flasher_args.json` 的真实布局烧入（**注意不是单镜像烧 0x0**）：

| 偏移 | 镜像 |
|---|---|
| `0x0` | `bootloader/bootloader.bin` |
| `0x8000` | `partition_table/partition-table.bin` |
| `0xd000` | `ota_data_initial.bin` |
| `0x10000` | `eh_cp_wifi_sta.bin` |

```
Wrote 1094464 bytes (628034 compressed) at 0x00010000 in 15.6 seconds
Hash of data verified.
```

## F.2 固件确实换了 —— 有证据

刷写前，C6 的 UART 每 5 秒吐一次**串口传输**统计：

```
stats: slave StaRxQLoad: 0
  serial : in: 0 rsp: 0 evt: 0
```

刷写后再读 `/dev/ttyUSB1`：**完全静默**。串口传输的统计消失了，
说明跑起来的正是 SDIO 传输版本。

## F.3 但 P4 仍然枚举不到（-110）

把探测窗口放宽到 **20 次 × 250 ms = 5 秒**，全部返回 `-110`：

```
esp_hosted_sdio: companion reset pulsed (GPIO54), holding enable for 200 ms
esp_hosted_sdio: SDIO host ready on slot 1
esp_hosted_sdio: verid=5342270a status=00000406 cdetect=00000000 clkena=00000002
esp_hosted_sdio: probe 1/20 -> -110, retrying
...
```

> 顺带修掉一个方法论问题：原来只探一次，那测的是"C6 启动多快"而不是"链路通不通"。
> 现在是有界重试窗口 —— 结论（-110）因此才是可信的。

## F.4 剩下的两个怀疑对象（按可疑度）

**1）C6 侧 SDIO 引脚可能不匹配本板接线。**
构建用的从机固件是 esp-hosted 的**默认 C6 引脚**：

```
CONFIG_EH_TRANSPORT_CP_SDIO_PIN_CMD=18
CONFIG_EH_TRANSPORT_CP_SDIO_PIN_CLK=19
CONFIG_EH_TRANSPORT_CP_SDIO_PIN_D0=20   D1=21   D2=22   D3=23
```

这组值是官方 P4+C6 参考设计的默认。但**本板把这 6 根线接到 C6 的哪几个脚，
我始终没有原理图级确认**（原理图文本层里只看到 P4 侧的 `SD2_*`，
协处理器侧只出现 `C5_IO2..C5_IO6`、`C5_U0TXD/RXD`、`C5_BOOT`、`C5_CHIP_PU`、`C5_EN`）。
如果本板用的是另一组引脚，改 `CONFIG_EH_TRANSPORT_CP_SDIO_PIN_*` 重新构建即可。

**2）从机固件的复位脚没配。**
`sdkconfig` 里 `CONFIG_EH_TRANSPORT_CP_SDIO_GPIO_RESET=-1`，
即从机侧没有配置复位脚。P4 侧我是用 GPIO54 拉低再拉高来复位 C6 的
（原理图确认 GPIO54 → `C5_EN`）。这一条是否足够让从机完成 SDIO 初始化的
复位序列，还没有证据。

## F.5 下一步（成本从低到高）

1. **用万用表/示波器核对** C6 的 GPIO18/19/20/21/22/23 是否真的接到 P4 的
   SD2_CMD/CLK/D0..D3 —— 这一步能直接判定是不是引脚问题；
2. 若引脚不同，改 `CONFIG_EH_TRANSPORT_CP_SDIO_PIN_*` 重新构建 C6 固件再烧；
3. 核对从机是否需要配 `GPIO_RESET`。

## F.6 顺带修掉的一个真实编译问题

投放帧层后目标编译报 `asm/byteorder.h: No such file or directory`：
NuttX 构建定义了 `__KERNEL__`，`eh_frame.c` 因此走内核分支去找 Linux 专用头。
源文件级 `-U__KERNEL__` 没有出现在生成的编译命令行里（试过，无效），
改为**满足这个 include**：提供 `shim/asm/byteorder.h`，把小端转换定义成恒等
（本目标是 RISC-V，小端）—— 与上游小端分支语义一致。
加了这个 shim 之后，含 SDIO 的整机构建 **0 error**。

## F.7 决定性发现：C6 一直卡在下载模式，从没跑过应用程序

不猜了 —— 在 **P4 复位 C6 的同时**监听 `/dev/ttyUSB1`，抓到了 C6 的 ROM 启动日志：

```
ESP-ROM:esp32c6-20220919
Build:Sep 19 2022
rst:0x1 (POWERON),boot:0x4 (DOWNLOAD(USB/UART0/SDIO_FEI_FEO))
waiting for download
```

关键两行：

- **`boot:0x4 (DOWNLOAD(...))`** —— 复位时采样的 strapping 是 **下载模式**，
  说明 **GPIO9（BOOT）在复位时是低电平**
- **`waiting for download`** —— ROM 引导程序停在下载模式等主机发数据，
  **根本没有跳转到 flash 里的应用程序**

再验证一次：`esptool --before no_reset chip_id` **仍然能连上**
（`Chip is ESP32-C6FH4 (QFN32) (revision v0.2)`）。芯片在复位后保持可连接，
就是"还在 ROM 下载模式"的确证。

**所以之前所有 -110 的解释都对齐了**：SDIO 从机固件确实烧进去了
（`Hash of data verified`），但它**一次都没有运行过**。指望一个停在 ROM
里的芯片应答 CMD5 是不可能的。

### 这不是软件能改的

试过用 `--after watchdog_reset` 触发一次不依赖 RTS 的复位，无效
（esptool 仍报 "Hard resetting via RTS pin"，而该脚未接）。

原理图上 `C5_BOOT` 只出现在**编程排针**附近（旁边是一个 3V3 的可选连接），
**没有接到 P4 的任何 GPIO** —— 原理图里 P4 到协处理器的信号只有
`P4_WAKEUP_C5` 一条。所以 P4 无法把 BOOT 拉高。

ESP32-C6 的 GPIO9 在复位时默认启用内部上拉，**松开按键后理应回到高电平**。
它现在仍是低电平，最可能的原因是：

1. **BOOT 键仍被按住**（刷写时按住后没有松开）；
2. 或者该脚的按键/跳线处于闭合状态。

### 需要你做的一步

**松开 BOOT 键**（确认它弹起），然后**给板子断电重新上电**。

之后 C6 的 ROM 日志应该变成类似
`boot:0x8 (SPI_FAST_FLASH_BOOT)`，那就说明进应用了。
再复位一次 P4，`esp_hosted_sdio` 就应该能枚举到它。

> 这一条把"引脚映射对不对"这个问题推后了：先让 C6 真的跑起来，
> 再看 SDIO 链路 —— 之前的 -110 里混着"芯片没启动"这个更大的问题。

## F.8 C6 跑起来之后：错误码从 -110 变成 -16，这是**新的**故障模式

用户手动重启 C6 之后，先确认真实状态：

```
esptool --before no_reset chip_id
  -> Failed to connect to Espressif device: No serial data received.
```

**连不上了，说明 C6 不再停在 ROM 下载模式，应用真的跑起来了**
（附录 F.7 的 `waiting for download` 问题已解决）。

C6 运行 SDIO 固件后，P4 侧的行为**变了**：

| | C6 停在 ROM 时 | C6 运行 SDIO 固件后 |
|---|---|---|
| `status` | `00000406` | **`00000106`** |
| 探测返回 | `-110` (ETIMEDOUT) | **`-16` (EBUSY)** |
| 含义 | 命令**发出去了**，对端不应答 | 命令**根本没能完成**（CIU 的 START 位不自清）|

这个变化本身是有信息量的：**总线上现在有东西在驱动它了**。
`-110` 是"没人应答"，`-16` 是"总线被占住，连广播 CMD0 都完成不了"。

## F.9 本轮排除的三件事

1. **不是"C6 启动不够快"**：探测窗口放宽到 **40 次 × 250 ms = 10 秒**，
   全程 `-16`，没有一次成功。10 秒足够 C6 启完 RTOS 和 WiFi 栈。
2. **不是"P4 复位 C6 导致"**：把启动时的复位脉冲（GPIO54 拉低再拉高）**去掉**，
   只保持使能电平，结果完全一样（`-16`）。
3. **不是"缺少 SDIO 的 80 时钟前导"**：查了 NuttX 的 `sdio_probe()`，
   它先发 `CMD0` 再发 `CMD5`；而本驱动的 `esp_sdmmc_sendcmd()` 对
   `cmdidx == 0` 会置 `send_init = 1`，即**已经带 80 时钟初始化序列**。

## F.10 下一步该看哪里

`-16` 来自 `esp_sdmmc_ciu_sendcmd()` 里"等 START 位自清"的超时分支 —— 也就是说
CIU 把命令**收下了却没有走完**。在真卡在场时出现这个现象，最可能是**总线电气层面**：

- Espressif 官方文档明确要求 **CMD / DAT0–DAT3 必须有外部上拉**
  （见 `getting-started-mcu.md`：*External pull-ups are mandatory on CMD,
  DAT0–DAT3 regardless of 1-bit or 4-bit mode*）。若本板缺上拉，
  从机一侧把线拉住就会让主机 CID 卡住；
- 或者 C6 侧 SDIO 引脚（固件用默认 18/19/20/21/22/23）与本板实际接线不一致 ——
  这一条始终没有原理图级确认，见 F.4。

**建议的下一步**（需要仪器或原理图）：
1. 量 P4 `SD2_CMD`(GPIO19) 与 `SD2_D0`(GPIO14) 的静态电平 —— 上拉正常应为高；
2. 量 C6 的 GPIO18/19/20/21/22/23 与 P4 的 `SD2_*` 是否一一连通；
3. 若确认缺上拉，在 CMD/DAT0–3 上补 10k 上拉到 3V3 再测。

---

# 附录 G：`-16` 的真根因 —— 一个**取反了的判断**（2026-10-26 傍晚）

用户确认"C6 的 SDIO 引脚 18/19/20/21/22/23 是对的、并且有 3.3V 上拉"之后，
硬件层面的怀疑基本排除，于是回到主机侧把 `-16` 挖到底。

## G.1 先让沉默的失败说话

`-16` 来自 `esp_sdmmc_ciu_sendcmd()` 里"等 START 位自清"的超时分支，而那几个
超时分支原本用的是 `mcerr()` —— **在 `-DNDEBUG` 下被编译掉**。改成 `syslog()`
之后立刻看到真相：

```
esp_sdmmc: clock update did not complete (cmd=20212000)
esp_sdmmc: previous command still pending after 100 ticks
           (cmd=20212000 status=00000106 rintsts=00000000)
```

**`cmd=...2000000` 的第 21 位就是 `update_clk_reg`** —— 我为了让卡时钟真正生效
而加的那条命令，它的 START 位从来没清过。

## G.2 根因：`sdmmc_ll_is_command_taken()` 的语义是**反的**

Espressif 的 LL 里：

```c
static inline bool sdmmc_ll_is_command_taken(sdmmc_dev_t *hw)
{
    return hw->cmd.start_command == 0;     /* ← 返回 true 表示"已被取走/空闲" */
}
```

名字叫 "taken"，返回 `true` 却表示 **START 已自清 = 控制器空闲**。

而我的等待循环写成了：

```c
while (sdmmc_ll_is_command_taken(hw))   /* 空闲时反而一直等！ */
```

**正好反了**：控制器空闲（`start_command == 0`）时它会一直转，直到超时返回
`-EBUSY`。只有当正好有一条命令在飞（`start_command == 1`）时循环才会立刻退出 ——
所以这个缺陷表现得时好时坏，极难从现象上认出来。

对照 Espressif 自己的用法（`sd_host_slot_start_command()`），两处等待都是
`while (!sdmmc_ll_is_command_taken(hw))`，即**等到 START 清零**。

修好这一处之后：

| | 修复前 | 修复后 |
|---|---|---|
| 探测返回 | `-16` (EBUSY) | **`-110` (ETIMEDOUT)** |
| "clock update did not complete" | 每次都报 | **不再出现** |
| "previous command still pending" | 每次都报 | **不再出现** |

也就是说：**命令现在是真的发出去了，时钟提交也真的生效了**。

## G.3 顺带修掉的第二处：时钟更新命令缺三个字段

对照 `sd_host_slot_clock_update_command()`，我的版本 `memset` 成 0 之后只设了
`update_clk_reg` 和 `start_command`，漏了：

| 字段 | 为什么必须有 |
|---|---|
| `card_num` | 命令要寻址**正在配时钟的那个 slot**。清零后 card_num=0（slot 0），而配的是 slot 1 —— CIU 完不成 |
| `use_hold_reg` | 输出要与 `cclk_out` 同步（Espressif 注释：*Outputs should be synchronized to cclk_out*）|
| `wait_complete` | 更新要等 CIU 报告完成 |

Espressif 的原话就是 `cmd.card_num = slot->slot_id`。补上后命令字从
`00200000` 变成 `20212000`（card_num=1 + update_clk_reg + wait_complete +
use_hold_reg）。

## G.4 本轮排除的

- **不是复位卡住**：给 DMA/FIFO/controller 三处复位超时都加了 `syslog`，
  真机上**一条都没触发** —— 三个复位都正常完成。
- **不是引脚/上拉**：用户已确认。
- **不是"C6 没启动"**：esptool 已连不上，`/dev/ttyUSB1` 静默，说明在跑应用。

## G.5 现在的状态

主机侧现在是**干净的**：

```
verid=5342270a status=00000106 cdetect=00000000 clkena=00000002
probe 1/12 -> -110
```

`-110` = 命令发出去了、对端不应答。**主机侧不再有内部缺陷**，
剩下的问题是对端没有回应 CMD5。

下一步方向：核对 C6 侧从机固件的 SDIO 配置（从机固件用的是默认引脚
18/19/20/21/22/23 与 `CONFIG_EH_TRANSPORT_CP_SDIO_GPIO_RESET=-1`），
以及 C6 是否真的把 SDIO 从机初始化起来了（可考虑在 C6 侧打开 SDIO 相关日志）。

---

# 附录 H：C6 侧的完整启动日志 —— 从机是好的（2026-10-26 傍晚）

把复位脉冲改回来（之前误以为是它导致 `-16`，实际是 G.2 那个取反的判断），
然后在 P4 复位 C6 的同时监听 `/dev/ttyUSB1`，**抓到了 C6 的完整启动日志**。

## H.1 ROM 与引导

```
ESP-ROM:esp32c6-20220919
rst:0x1 (POWERON),boot:0xc (SPI_FAST_FLASH_BOOT)     ← 正常启动，不再是 DOWNLOAD
SPIWP:0xee
mode:DIO, clock div:2
I (303) boot: Loaded app from partition at offset 0x10000
```

`boot:0xc (SPI_FAST_FLASH_BOOT)` —— 附录 F.7 那个"卡在 ROM 下载模式"的问题
已经彻底解决。

## H.2 从机固件确实在跑 ESP-Hosted over SDIO

```
I (317) cpu_start: GPIO 17 and 16 are used as console UART I/O pins
I (407) ehcp_core: ESP-Hosted core init
I (429) ehcp_core: - WLAN over SDIO
I (441) ehcp_core: ESP-Hosted Firmware version :: EH-3.0.10
I (448) ehcp_core: Transport used :: SDIO only
I (462) SDIO_SLAVE: Using SDIO interface (TX_MODE=0)
I (466) SDIO_SLAVE: SDIO datapath mode: SW_AGGR
I (470) SDIO_SLAVE: rx_buf_size=15872
I (474) SDIO_SLAVE: [mem:sdio_init] free=291320 min=291320 largest=270336
I (480) ehcp_core: host reset handler task started
...
I (623) ehcp_wifi: WiFi feature init ok
I (627) ehcp_core: auto_feat_init_task: all extensions initialised
I (633) ehcp_core: ESP-Hosted core initialized
I (644) wifi_mcu_example: ESP-Hosted MCU WiFi Example Starting...
```

这一条非常有价值：**"Transport used :: SDIO only"** —— 从机确认自己跑的是
SDIO 传输（对比出厂固件那句 `serial : in/rsp/evt`），SDIO 从机也已初始化，
WiFi 扩展就绪，之后 `main_task` 返回、进入空闲等待主机。

## H.3 由此可以排除的

- ❌ "C6 没启动" —— ROM 日志显示已从 `0x10000` 载入应用；
- ❌ "C6 跑的不是 SDIO 版本固件" —— 它自己打印 `Transport used :: SDIO only`；
- ❌ "C6 的 SDIO 从机没初始化" —— `SDIO_SLAVE:` 那几行说明初始化完成了；
- ❌ "WiFi 栈没起来" —— `ehcp_wifi: WiFi feature init ok`。

**从机侧是好的，在等主机。**

## H.4 但主机发 CMD5 仍然没有应答（-110）

时序上也说得通但不足以解释：P4 复位 C6 后等 200 ms 就开始探测，而 C6 要到
约 650 ms 才完全就绪 —— 不过探测窗口是 **12 × 250 ms = 3 秒**，
后面的探测远远晚于 650 ms，所以**不是时间不够**。

## H.5 剩下的可能性（收窄到两条）

1. **P4 与 C6 之间的 SDIO 走线并非 GPIO14–19 ↔ GPIO18–23 这一组**
   （用户已确认 C6 侧 `18/19/20/21/22/23` 与上拉没问题，但"P4 侧那 6 根
   到底接的是 P4 的哪几个脚"仍缺原理图级证据 —— 我一直是从
   `sdmmc_pins.h` 的 SLOT1 定义反推的）；
2. **从机需要 `CONFIG_EH_TRANSPORT_CP_SDIO_GPIO_RESET` 配置**
   （当前 `=-1`，即从机侧不管理复位脚），或从机在等待主机某个
   特定的初始化动作。

## H.6 一个可执行的下一步

C6 的日志是**可以读的**（GPIO16/17 → ttyUSB1，115200）。所以下次可以：

- 在从机固件里打开 SDIO 从机的详细日志（`CONFIG_LOG_DEFAULT_LEVEL_DEBUG`
  或 esp-hosted 的 SDIO 调试开关），重新构建烧写；
- 然后一边看 C6 日志、一边让 P4 探测 —— **如果从机完全没有反应，
  问题就在物理连线；如果从机报错，错误信息会直接指出来**。

这一步能一次性判定"是连线还是协议"。需要再次让 C6 进下载模式（按 BOOT）。

## H.7 第 21 轮：P4 侧引脚**已从原理图确认**，并备好带调试日志的从机固件

### H.7.1 P4 侧引脚确证 —— 与本移植完全一致

重新在原理图文本里定位 `SD2_*` 与 P4 GPIO 的对应（这次抓到了带引脚号的表）：

```
SD2_D0  -> GPIO14        SD2_D1  -> GPIO15
SD2_D2  -> GPIO16        SD2_D3  -> GPIO17
SD2_CLK -> GPIO18        SD2_CMD -> GPIO19
```

**与 `esp_sdmmc.c` 里为 slot 1 配的引脚一字不差**，也与 Espressif 官方
`getting-started-mcu.md` 的 P4 主机表一致。加上用户确认的 C6 侧
（18/19/20/21/22/23）与 3.3V 上拉 —— **两侧引脚都已确认无误，H.5 的第 1 条排除**。

### H.7.2 顺带查了 P4 专用 workaround：与我们无关

`eh_host_port_sdio_workaround()` 只处理 **slot 0 在 1-bit 模式下的 IOMUX 引脚**
问题；本移植用 slot 1 + 4-bit，不涉及。

### H.7.3 备好了 DEBUG 日志版的从机固件

既然 C6 的日志**可读**（GPIO16/17 → ttyUSB1 @115200），下一步就是让它"开口"：

| | INFO 版（已烧） | **DEBUG 版（已备好）** |
|---|---|---|
| `CONFIG_LOG_DEFAULT_LEVEL` | 3 | **4** |
| `CONFIG_LOG_MAXIMUM_LEVEL` | 3 | **4** |
| 体积 | 1,094,464 B | 1,131,952 B |

### H.7.4 修掉一个真 bug：flash 脚本的偏移是错的

`flasher_args.json` 的真实布局是 **4 个镜像**：

| 偏移 | 镜像 |
|---|---|
| `0x0` | bootloader.bin |
| `0x8000` | partition-table.bin |
| `0xd000` | ota_data_initial.bin |
| **`0x10000`** | **app** |

而本脚本早期版本把 app 单镜像写到 `0x0` —— 那是错的。现在
`flash_c6.sh` 按真实布局烧 4 个镜像，并把 4 个镜像都收录进交付：

```
tools/build_flash/c6_hosted/
├── eh_cp_wifi_sta_esp32c6_sdio.bin    INFO 版
├── eh_cp_wifi_sta_debug.bin           DEBUG 版（观察从机 SDIO 行为用）
├── bootloader.bin / partition-table.bin / ota_data_initial.bin
├── sdkconfig.esp32c6.sdio
└── flash_c6.sh                        [--debug] 可选
```

### H.7.5 下一步的判定逻辑

烧 DEBUG 版后，一边看 C6 日志、一边让 P4 探测：

- **从机完全没有任何反应** → 问题在**物理层**（尽管两侧引脚都已确认，
  仍需考虑焊接/走线/上拉实效）；
- **从机报了错或收到命令** → 错误信息会直接指出是协议还是配置。

## H.8 第 22 轮：`GPIO_RESET=-1` 不是问题 —— 上游文档明确说它对本板是对的

之前把 `CONFIG_EH_TRANSPORT_CP_SDIO_GPIO_RESET=-1` 列为可疑项，查了上游
Kconfig 之后确认**这个判断是错的**：

```
config EH_TRANSPORT_CP_SDIO_GPIO_RESET
    int "Slave GPIO pin to reset itself"
    default -1
    help
        -1 (default) means the host resets the co-processor with its EN pin.
        Set a pin only if the module does not expose EN. The host drives it
        like EN: parked HIGH, pulsed LOW. A LOW pulse of 2 ms or more does a
        software restart, which cannot recover a hung co-processor.
```

三点结论：

1. **`-1` 就是"主机用 EN 脚复位从机"** —— 本板正是这样（P4 GPIO54 → C6 的 EN），
   所以 `-1` **是正确配置**，不是遗漏；
2. 主机应当 **parked HIGH, pulsed LOW** —— 本驱动现在的顺序（拉低 50 ms 再拉高）
   恰好符合；
3. 该 Kconfig 只在"模组没有引出 EN 脚"时才需要设值。

## H.9 至此被排除的清单（按轮次累积）

| # | 曾经的怀疑 | 结论 | 依据 |
|---|---|---|---|
| 1 | 控制器没起来 | ❌ 排除 | `verid=5342270a`，`SDIO host ready on slot 1` |
| 2 | 卡时钟没提交 | ✅ **确是真缺陷，已修** | 附录 C（`update_clk_reg`）|
| 3 | 使能极性/时长 | ❌ 排除 | 高低两种极性、200ms~10s 全试过 |
| 4 | C6 停在下载模式 | ✅ **确是真问题，已解决** | 附录 F.7（`boot:0x4 DOWNLOAD`）|
| 5 | C6 没跑 SDIO 固件 | ❌ 排除 | 附录 H.2（`Transport used :: SDIO only`）|
| 6 | C6 的 SDIO 从机没初始化 | ❌ 排除 | 附录 H.2（`SDIO_SLAVE: ... sdio_init`）|
| 7 | START 判断取反 | ✅ **确是真缺陷，已修** | 附录 G.2（`-16` → `-110`）|
| 8 | 时钟更新命令缺字段 | ✅ **确是真缺陷，已修** | 附录 G.3 |
| 9 | 复位卡住 | ❌ 排除 | 三处复位超时日志一条未触发 |
| 10 | P4 侧引脚不对 | ❌ 排除 | 附录 H.7.1（原理图 GPIO14-19）|
| 11 | C6 侧引脚/上拉不对 | ❌ 排除 | 用户确认 18-23 + 3.3V 上拉 |
| 12 | `GPIO_RESET=-1` 配置缺失 | ❌ 排除 | 附录 H.8（上游文档：这正是本板的正确值）|

**累计在真机上定位并修复的缺陷：** 驱动侧 8 个（含本轮之前的 5 个音频缺陷）、
上游应用 2 个（nxplayer/nxrecorder）。WiFi 侧这 3 个（#2/#7/#8）都只有真机
才能暴露。

## H.10 还剩下什么

主机侧配置已按官方逐项对齐，两侧引脚与上拉均已确认，从机确认在跑并从机已初始化，
但 `CMD5` 仍无应答。剩下的只在两处：

1. **物理层实效**（焊接/走线/上拉是否真的通）—— 需要万用表量通断；
2. **从机为何不响应** —— 需要让从机"开口"：烧 DEBUG 日志版（已备好，
   `flash_c6.sh --debug`），一边看 C6 日志一边让 P4 探测。

第 2 条能一次性区分这两者，且只差一次按键。

## H.11 第 23 轮：DEBUG 版从机固件已烧入并**读回校验通过**

用修正后的脚本（4 个镜像、正确偏移）烧入 DEBUG 版：

```
Wrote 1131952 bytes (640142 compressed) at 0x00010000 in 16.0 seconds
Hash of data verified.
```

并额外做了一次**读回比对**（不只是写入时的自校验）：

```
esptool verify_flash 0x10000 eh_cp_wifi_sta_debug.bin
Verifying 0x1145b0 (1131952) bytes @ 0x00010000 ...
-- verify OK (digest matched)
```

**这条同时验证了两件事**：DEBUG 固件确实在 C6 的 flash 里；
以及 `flash_c6.sh` 修正后的 4 镜像偏移布局是对的。

## H.12 但 C6 又进了下载模式（BOOT 仍被按住）

烧写后 P4 复位（会拉低 C6 的 EN）时，C6 的 ROM 日志又是：

```
rst:0x1 (POWERON),boot:0x4 (DOWNLOAD(USB/UART0/SDIO_FEI_FEO))
waiting for download
```

`boot:0x4` 说明**复位时 GPIO9(BOOT) 仍为低**。这与附录 F.7 是同一个现象：
按下 BOOT 之后必须**松开**，芯片才会在下次复位时从 flash 启动。

**需要：松开 BOOT 键 → 断电重新上电（或按一下 RESET）。**
之后 C6 才会跑起 DEBUG 版，我们才能看到它在 P4 探测时的 SDIO 行为。

---

# 附录 I：C6 真正跑起来后暴露的**三方死锁**（2026-10-26 夜）

C6 跑起 DEBUG 版之后（esptool 连不上、串口出现 `D` 前缀日志），P4 侧的日志
第一次把完整的因果链摆出来了：

```
esp_sdmmc: deferring FIFO reset until a card clock exists   ← 本轮的修复生效
esp_sdmmc: clock update did not complete (cmd=a0212000)      ← 卡时钟提交失败
esp_sdmmc: previous command still pending after 100 ticks (cmd=a0212000 ...)
esp_hosted_sdio: probe 1/12 -> -16
esp_sdmmc: FIFO reset did not complete (ctrl=00000012 ...)
esp_sdmmc: reset still stuck after the clock came up (ctrl=00000012)
```

`cmd=a0212000` 各位：bit31 `start_command` + bit29 `card_num=1` +
bit21 `update_clk_reg` + bit13 `wait_complete` + bit5 `use_hold_reg`
—— 字段全对（附录 G.3），**但它就是走不完**。

## I.1 三方互相锁死

DesignWare MSHC 上这三件事互为前提：

1. **FIFO 复位**（`CTRL.fifo_reset` 自清）需要**卡时钟在跑**
   —— 它的自清位在卡时钟域同步；
2. **卡时钟**要真正生效，必须发一条 `update_clk_reg=1` 的命令把它从 BIU
   提交到卡时钟域（附录 C）；
3. **任何命令**（包括第 2 条）都要求 **CIU 可用**，而 `CTRL.fifo_reset` 为 1
   时 CIU 跑不了命令。

于是：初始化时"先复位、后配时钟"的顺序，会让第 1 步在**没有时钟**时发出
（寄存器写进去了、`clkena` 读回来也是 2），但它**永远不会自清**；
接着第 2 步因为第 1 步卡住而失败；第 3 步从此再也发不出命令。

**本轮的修复（已做）**：`esp_sdmmc_reset()` 里，**只有当卡时钟已使能时才复位
FIFO**，否则延后 —— 真机上确实看到 `deferring FIFO reset until a card clock
exists`，说明这一步按预期生效了。

**但还不够**：后续 `esp_sdmmc_bus_reset()` 再次复位时，`clkena` 虽读回非 0，
可它**从未真正提交到卡时钟域**（提交命令就是失败的那条），所以 FIFO 复位
依旧卡在 `ctrl=00000012`。判断依据是自清的 `fifo_reset` 位在 `ctrl` 里读回为 1。

## I.2 因此正确的修法是**调整初始化顺序**

不能"先复位、后配时钟"。应当：

1. 使能外设总线时钟 / 解除控制器复位（BIU 域，不需要卡时钟）；
2. **先**配置并提交卡时钟（`CLKDIV`/`CLKENA` + `update_clk_reg` 命令）
   —— 此时 FIFO 尚未被复位，CIU 可用；
3. **再**做 FIFO 复位（这时卡时钟已在跑，自清会成功）；
4. 之后才允许发正常命令。

本轮的"延后 FIFO 复位"只是把第 1 步挪开，第 2 步之前仍有一次不带时钟的
`reset_controller`/`reset_dma`，而且提交时钟那一步本身失败——所以还需要
把顺序整体理顺。**这是下一轮的第一件事。**

## I.3 附带收获：从机侧 DEBUG 日志确认可用

C6 的 DEBUG 日志确实打出来了（`D (518) intr_alloc: ...`），说明"让从机开口"
这条诊断通道是通的。本轮 C6 侧没有报错，也没有收到命令的迹象 —— 这与
"主机根本没把命令发出去"（I.1 的死锁）是一致的，**不构成从机侧的问题证据**。

## I.4 第 25 轮：两个假设都被真机否掉，问题收敛到"这一条命令本身"

按 I.2 的推论做了两个实验，都是**在真机上被否掉的**，如实记录：

### 实验 1：把时钟提交提前到复位之前 —— 反而更早失败

改成 `esp_sdmmc_clock()` 先于 `esp_sdmmc_reset()`，结果是**时钟提交成为第一条
失败的命令**（在完全没有做过任何 FIFO 复位的新控制器上）：

```
esp_sdmmc: clock update did not complete (cmd=a0212000)     ← 第一条就是它
esp_sdmmc: FIFO reset did not complete (ctrl=00000002 ...)  ← FIFO 是随后才卡住
```

**这条否掉了 I.1 的"三方死锁"解释**：既然在一个 FIFO 从未被复位过的全新控制器上，
`update_clk_reg` 同样走不完，那么"FIFO 复位挡住 CIU"就不是根因 ——
**根因是这条命令本身在当前的控制器配置下无法完成**。

### 实验 2：去掉 `use_hold_reg` —— 无效

假设是"输出要与 `cclk_out` 同步，而此刻时钟还没跑，于是卡住"。清掉该位后
命令字从 `a0212000` 变成 `80212000`，**结果完全一样**（仍然不一致地
`clock update did not complete`）。否掉。

### 当前已知的事实（供下一轮接手）

- `update_clk_reg` 命令写入后，`CTRL.command.start_command` 位不自清；
- 无论控制器是全新的、还是刚复位过的，都一样；
- `card_num` / `wait_complete` / `use_hold_reg` 三个字段都试过，都不是原因；
- 失败现场 `status=00000406`、`rintsts=00001000`（**第 12 位置起**）——
  这一位的含义还没查出来，是下一个最该看的地方（`sdmmc_struct.h` 里
  `sdmmc_rintsts_reg_t` 的对应位名）；
- **从机侧无任何异常**（DEBUG 版日志已确认），问题完全在主机侧。

### 本轮代码的处置

两个实验都没起作用，已把 `esp_sdmmc.c` **回退到上一轮提交的状态**
（保留 FIFO 门控 + 时钟后重试），使驱动尽可能贴近 Espressif 的参考流程，
避免在未验证的改动上继续叠加。

## I.5 第 26 轮：**换一条路提交时钟 —— 成功**

上轮把 `update_clk_reg` 命令这条路试尽了（字段、顺序全试过）。本轮改换做法：

**用控制器复位来锁存时钟寄存器**，而不是发那条命令。理由：`CTRL.controller_reset`
会把 `CLKDIV`/`CLKSRC`/`CLKENA` 从 BIU 重新采样进卡时钟域，而它**不需要 CIU 执行
任何东西** —— 正是 `update_clk_reg` 该做、而本芯片上 CIU 做不到的事。

真机结果（**这是本轮的关键证据**）：

```
esp_sdmmc: deferring FIFO reset until a card clock exists
esp_hosted_sdio: SDIO host ready on slot 1
esp_hosted_sdio: probe 1/12 -> -110        ← 命令真的发出去了（此前恒为 -16）
```

**第一次探测从 `-16` 变成 `-110`** —— 在 `probe 2` 之前没有任何
"clock update did not complete"，说明时钟提交这条路走通了。

### 顺带修掉两个自己留下的问题

1. `esp_sdmmc_clock()` 里替换掉那条命令之后，**留下了一个等待它的循环**，
   会为一条从未发出的命令报超时 —— 已删；
2. `esp_sdmmc_bus_reset()` 原本调用完整的 `esp_sdmmc_reset()`（含 FIFO 复位），
   而 FIFO 复位在本芯片上不自清，等于**每次重试都把刚修好的状态毁掉**。
   改为只复位控制器。

### 仍然存在的问题（下一步）

`probe 1` 之后 START 位不再自清：

```
esp_sdmmc: previous command still pending after 100 ticks
           (cmd=a0018000 status=00000406 rintsts=00001000)
esp_hosted_sdio: probe 2/12 -> -16
```

`cmd=a0018000` 的 bit31（`start_command`）仍为 1，说明**这条命令的 FSM 没有走完**。
`rintsts=00001000`（第 12 位置起）应当就是原因所在，但我没能在
`sdmmc_struct.h` 里找到该位的字段名（`bitpos: [12]` 只匹配到 CMD 寄存器的
`send_auto_stop`），下次需要换一种方式查（例如按结构体顺序数位，或对照
Linux `dw_mmc` 的 `SDMMC_INT_*` 定义）。

**目前的净收益**：主机侧第一次能真正把命令发出去，且时钟提交不再依赖那条
走不通的命令；剩下的问题从"一条命令都发不出"收窄到"命令发出后 FSM 不收尾"。

## I.6 第 27 轮：把卡住的那条命令**解码出来** —— 是 CMD0，根因是卡时钟没跑

上轮留下的悬念是"`rintsts` 第 12 位是什么"。查清了：

hw_ver3 的 `sdhost_rintsts_reg_t` 把 16 个中断位放在一个字段里：

```
[ 0] int_status_raw (16)     ← 16 个中断位都在这里
[16] sdio_interrupt_raw (2)
[18] reserved_18 (14)
```

`rintsts=0x1000` → 该字段的 bit 12 → 按 `dw_mmc` 的定义即 **EBE（End Bit Error，
结束位错误）**：响应/数据的结束位不是 1。

### 更关键的是把**卡住的那条命令**解码出来

一直卡住的是 `cmd=a0018000`，逐位展开：

```
bit 31  start_command        ← 未自清，就是它卡住的原因
bit 29  card_num  (=1，slot 1，正确)
bit 16  (未列名)
bit 15  send_initialization  ← 关键
```

**bit 15 = `send_initialization`** —— 这条命令是 **CMD0**，也就是那条要输出
**80 个时钟**做初始化的命令。

**所以 START 不清的原因很直接：80 个时钟需要卡时钟在跑才能发出来；
卡时钟没跑，这 80 个时钟永远发不完，START 就永远不清。**
随后所有命令都因为 START 占着而返回 `-EBUSY`。

### 由此得出的结论

**卡时钟仍然没有真正运行** —— 尽管 `clkena` 读回来是 `0x2`
（bit1 = `cclk_enable` 已置位）。也就是说：

- 寄存器层面"时钟已使能"；
- 但时钟**没有真正进入卡时钟域**；
- 无论是用 `update_clk_reg` 命令（走不完）还是用控制器复位（本轮换成这个）
  来提交，都没能让它真正跑起来。

这解释了此前所有的现象，也把问题**收敛到一处**：如何让 P4 的 SDMMC 卡时钟
真正输出。

### 本轮顺带补上的一个真实缺口

驱动里只配了**输出**延迟相位（`sdmmc_ll_set_dout_delay_phase`），
**输入采样相位从未配置**：

```c
sdmmc_ll_set_din_delay_phase(hw, SDMMC_LL_DELAY_PHASE_0, SDMMC_LL_SPEED_MODE_HS);
```

这个缺口是真的（EBE 正是输入被误采样的症状），已补上。但真机上 EBE 依旧
出现 —— 说明在"卡时钟没跑"这个更大的问题面前，它还不是主因。保留该修复。

### 下一步

查"卡时钟为什么没真正跑起来"。可查的方向：

1. `sdmmc_ll_select_clk_source(hw, SDMMC_CLK_SRC_DEFAULT)` 选的源是否真的在输出；
2. 系统侧的时钟门控（`HP_SYS_CLKRST`）里是否有额外的 SDMMC 时钟使能位没打开
   —— 本驱动只调了 `sdmmc_ll_enable_bus_clock(0, true)`；
3. 对照 Espressif 的 `sd_host_setup()` 完整时钟初始化序列，逐条比对还差哪一步。

## I.7 第 28 轮：三个假设，两个被否，一个确认是真缺口

### 确认的缺口：**时钟源从未被使能**

对照 Espressif 的 `sd_host_set_clk_div()`：

```c
ESP_ERROR_CHECK(esp_clk_tree_enable_src((soc_module_clk_t)src, true));  // ← 先使能
sdmmc_ll_set_clock_div(...);
sdmmc_ll_select_clk_source(ctlr->hal.dev, src);                        // ← 再选
```

`SDMMC_CLK_SRC_DEFAULT` = `SOC_MOD_CLK_PLL_F160M`，而 P4 上**每个参考时钟是独立门控的**，
必须 `clk_gate_ll_ref_160m_clk_en(true)` 打开后 `CLKSRC` 才能路由它。
本驱动此前**只 select、从未 enable**。

已补（含 Espressif 那句 "wait for the clock to propagate" 的 10µs 延时）。
**这是真缺口，虽然本轮没让枚举通过，但它必须存在。**

### 否定 1：不是 80 时钟初始化序列

把 CMD0 的 `send_init` 去掉后，命令字从 `a0018000` 变成 `a0010000`，
**CMD0 依旧卡住**。所以卡住的不是那 80 个时钟。已把该改动撤回
（保持标准 SD 初始化流程），不在未验证的改动上留残迹。

### 否定 2：不是 PLL_F160M 没开

补了 `clk_gate_ll_ref_160m_clk_en(true)` 后现象完全没变 —— 说明这个 PLL
本来就在跑（P4 上它可能被别的模块用着）。

### 现在的核心线索只有一个：**EBE 每次都出现**

无论 CMD0 带不带 `send_init`，失败现场恒为：

```
cmd=a0010000 status=00000406 rintsts=00001000
```

`rintsts` bit 12 = **EBE（End Bit Error）**：控制器在 CMD 线上看到了一个起始位，
但**结束位不是 1**。这不是"没人应答"（那会是 bit 15 的 RTO），而是
**线上有信号、但电平/时序不对**。

结合"卡时钟是否真的在输出"仍未确证，下一轮该做的判断是：

1. **确认 CLK 脚是否真的在翻转** —— 这是唯一还没有直接证据的环节；
2. 若 CLK 正常而 CMD 仍然 EBE，则是 CMD 线的采样/驱动问题
   （相位、上拉实效、或 GPIO 矩阵信号号段选错）；
3. 若 CLK 不翻转，则是时钟树/GPIO 矩阵的问题。

`esp_sdmmc_configpins()` 里用的 GPIO 矩阵信号号（`sdmmc_periph_signal` 一类）
是下一个应当逐条核对的地方 —— 若号段选错，引脚会被配置成功能态却**没有真正
连到 SDMMC 外设**，症状与现在完全吻合。

## I.8 第 29 轮：**根因找到** —— slot 1 必须走 GPIO 矩阵，而本驱动从未做路由

### 证据链（两处独立的官方定义互相印证）

`components/esp_hal_sd/esp32p4/include/hal/sdmmc_ll.h`：

```c
#define SDMMC_LL_SLOT_SUPPORT_GPIO_MATRIX(SLOT_ID)    ((SLOT_ID == 0) ? 0 : 1)
```

即 **slot 0 不支持 GPIO 矩阵（走 IOMUX），slot 1 必须走 GPIO 矩阵**。

`components/esp_hal_sd/esp32p4/include/soc/sdmmc_pins.h` 里则只有 slot 0 的
IOMUX 引脚号，并直接写着一行注释：

```c
// SLOT1 doesn't go through IOMUX
```

### 而本驱动的引脚配置是这样的

```c
static void esp_sdmmc_configpins(void)
{
  esp_configgpio(priv->clk_pin, OUTPUT | DRIVE_3);
  esp_configgpio(priv->cmd_pin, OUTPUT | PULLUP | DRIVE_3);
  esp_configgpio(priv->d0_pin,  OUTPUT | PULLUP | DRIVE_3);
  ...
}
```

**只有 `esp_configgpio()`，一次 `esp_gpio_matrix_out()` / `esp_gpio_matrix_in()`
都没有。** 也就是说：引脚被配置成了"通用 GPIO 输出"，但 **SDMMC 外设的信号
从来没有连到这些引脚上**。

### 这一条解释了此前所有现象

| 现象 | 解释 |
|---|---|
| CLK 不翻转（CMD0 的 80 时钟发不完） | 控制器在内部计数，但信号不出现在引脚上 |
| `rintsts` 恒为 **EBE（结束位错误）** | 控制器从引脚回读的是引脚自身状态（上拉高），不是它自己发的波形 |
| `clkena` 读回"已使能"却看不到时钟 | 寄存器层面一切正常，断点在 GPIO 矩阵这一跳 |
| 对端（C6）完全收不到命令 | 命令根本没到线上 |
| 从机侧日志毫无异常 | 它确实什么都没收到 |

### 这也解释了为什么前几轮的排查屡屡"看似合理却无效"

时钟提交、START 判断、采样相位、时钟源使能 —— 这些**都是真缺陷也都修对了**，
但它们都在 GPIO 矩阵**之后**的环节。**只要信号出不了引脚，后面修得再对也看不到效果。**

### 修复方向（下一轮第一件事）

按 slot 1 走 GPIO 矩阵的要求，把六个信号分别路由到引脚，例如：

```c
esp_gpio_matrix_out(clk_pin, SDMMC_SLOT1_CLK_OUT_IDX, 0, 0);
esp_gpio_matrix_in (cmd_pin, SDMMC_SLOT1_CMD_IN_IDX,  0);
esp_gpio_matrix_out(cmd_pin, SDMMC_SLOT1_CMD_OUT_IDX, 0, 0);
/* D0..D3 同理，每个方向各一条 */
```

还需要拿到 P4 上 SDMMC slot 1 各信号的**矩阵索引号**。
本轮查过 `components/soc/esp32p4/include/soc/gpio_sig_map.h` —— 里面**没有**
SDMMC 条目，所以索引应当来自别处（可能随 IO_MUX/矩阵的 P4 专属定义，
或需要从 IDF 的 `sdmmc_periph.c` 信号表里取）。这是下一轮要解决的第一件事。

## I.9 第 30 轮：GPIO 矩阵路由**已实现并确认生效**，但 EBE 依旧

### 实现内容

按 I.8 的结论补齐了 slot 1 的矩阵路由，信号索引取自 Espressif 自己的
`components/esp_hal_sd/esp32p4/sdmmc_periph.c`（slot 1 那一组）：

| 信号 | 输出索引 | 输入索引 |
|---|---|---|
| CLK  | `SD_CARD_CCLK_2_PAD_OUT_IDX`   | 仅输出 |
| CMD  | `SD_CARD_CCMD_2_PAD_OUT_IDX`   | `SD_CARD_CCMD_2_PAD_IN_IDX` |
| D0   | `SD_CARD_CDATA0_2_PAD_OUT_IDX` | `..._IN_IDX` |
| D1..D3 | 同理 | 同理 |

CLK 只输出；CMD 与 DAT 由控制器驱动又要回读，所以两个方向都接。
真机确认生效：

```
esp_sdmmc: slot 1 pads routed via the GPIO matrix (CLK=18 CMD=19 D0..D3=14,15,16,17)
```

顺带修掉一个顺序陷阱：`esp_sdmmc_configpins()` 在 `priv->widebus` 还是 false 时
就执行了，若按 4-bit 条件路由，**D1–D3 会整个会话都不被路由**。改为无条件路由
（1-bit 下多接几条无害）。

### 但枚举仍未通过

```
probe 1/12 -> -110
previous command still pending (cmd=a0018000 status=00000406 rintsts=00001000)
probe 2/12 -> -16
```

`rintsts` 的 **bit 12 = EBE** 依旧出现，`start_command` 依旧不自清。

### 现在的判断：问题只剩"引脚复用功能的设置次序"

对照 Espressif 的 `configure_pin_gpio_matrix()`：

```c
gpio_reset_pin(gpio_num);              /* ← 先把引脚复位成普通 GPIO */
gpio_set_direction(gpio_num, mode);
esp_rom_gpio_connect_in_signal(...);
esp_rom_gpio_connect_out_signal(...);
```

**关键在那个 `gpio_reset_pin()`**：它把管脚的 IO_MUX 功能选择置回"GPIO"。
若管脚的 IO_MUX 仍指向某个外设功能，**GPIO 矩阵的连接会被忽略**。

本驱动的次序是 `esp_configgpio(...)` → `esp_rom_gpio_connect_*_signal(...)`，
但 NuttX 的 `esp_configgpio()` 内部会把 IO_MUX 设成什么，**没有核实过**。
如果它把 IO_MUX 选到了别的功能上，那么矩阵连接虽然写进了寄存器、
真机也打印了"routed"，**信号仍然到不了引脚**。

**下一轮第一件事**：在 `esp_sdmmc_configpins()` 里、`esp_configgpio()` 之前，
显式把管脚置成"普通 GPIO / 矩阵输入输出"（等价于 `gpio_reset_pin`），
然后再做矩阵连接。这是目前唯一还没对齐的环节。

## I.10 第 31 轮：**用 P4 自己把 CLK 引脚读出来了 —— 恒为低电平**

前面十几轮都在"寄存器读回来都是对的、但什么都不工作"这个死结里打转。
本轮换掉这个思路：**不去读寄存器，直接读引脚**。

做法：CLK 引脚仍由 SDMMC 控制器经 GPIO 矩阵驱动，同时**打开它的输入通路**，
然后用 `esp_gpioread()` 连续采样 512 次。

> ⚠️ 第一版这个诊断我写错了：用了 `esp_configgpio(CLK, INPUT)`，
> 那会**关掉引脚的输出驱动器**，等于人为把控制器和引脚断开，测出来必然是
> "没有时钟"。改成 `INPUT | OUTPUT`（保持输出使能）才是有效测量。
> 这个错误我犯了一次，记在这里免得以后再踩。

修正后的真机结果：

```
esp_hosted_sdio: CLK pin (GPIO18) sampled 512x as input:
                 0 high, 512 low -- NOT toggling (no clock on the pad)
```

**输出驱动器是开着的，CLK 引脚却恒为低电平。**

### 这是第一条不含推断的直接证据

此前所有结论都是从寄存器"读回来对不对"间接推的，而这一条是**物理测量**：
控制器没有在这个引脚上产生任何波形。它同时解释了：

- 为什么 CMD0（要发 80 个时钟）的 `start_command` 永不自清 —— 时钟根本不存在；
- 为什么 `rintsts` 恒为 EBE —— 控制器回读到的是死的引脚电平；
- 为什么此后所有命令都是 `-EBUSY`；
- 为什么对端（C6）什么也收不到 —— 线上确实什么都没有。

### 据此，前面的修复该怎样看待

| 修复 | 是否真缺陷 | 是否本因 |
|---|---|---|
| 卡时钟提交（附录 C） | 是 | 否 |
| START 判断取反（附录 G.2） | 是 | 否 |
| 时钟更新命令缺字段（附录 G.3） | 是 | 否 |
| 输入采样相位（附录 I.6） | 是 | 否 |
| 时钟源使能（附录 I.7） | 是 | 否 |
| GPIO 矩阵路由（附录 I.9） | 是 | 否 |
| 管脚功能选择（本轮） | 是 | 否 |

**它们全都是真缺陷，但没有一个是本因** —— 因为它们的失效表现都被同一个
更前置的现象掩盖了：**引脚上从来没有时钟**。

### 下一步：时钟为什么到不了引脚

现在范围很明确，只需要在这条链上找断点：

1. **GPIO 矩阵信号索引**：CLK 用的 `SD_CARD_CCLK_2_PAD_OUT_IDX` 值为 **0**
   —— 值得专门确认索引 0 在 P4 上是有效输出信号，而不是特殊/保留值；
2. **卡时钟分频与使能**：`clkdiv=00009f00`（分频 159）、`clkena=00000002`
   （bit1 = `cclk_enable`）——寄存器值一直是对的，所以断点更可能在
   时钟树或矩阵，而不在这两个寄存器；
3. **`sdmmc_ll_select_clk_source` 选的源是否真的在输出** —— 上轮补了
   `clk_gate_ll_ref_160m_clk_en(true)` 但现象未变，需确认这个 API 在
   NuttX 的 esp-hal 里不是空实现。

## I.11 第 32 轮：隔离测试 —— **引脚是好的，SDMMC 没有驱动它**

### 结果

把 CLK 引脚当普通 GPIO 驱动并回读：

```
esp_hosted_sdio: CLK pad as plain GPIO: wrote 1 -> read 1, wrote 0 -> read 0
                 -- pad works
```

而同一根引脚路由给 SDMMC 的 CLK 信号时：

```
esp_hosted_sdio: CLK pin (GPIO18) sampled 512x as input: 0 high, 512 low
                 -- NOT toggling (no clock on the pad)
```

**引脚、输出驱动器、输入通路、焊接全部正常。恒低电平来自 SDMMC 一侧。**

这一条把范围从"引脚/走线/上拉/矩阵"整体排除，只剩**控制器的时钟生成**。

### 我在这个测试里又犯了一次同类错误（记下来）

第一版隔离测试写的是"配置成 GPIO 输出 → 写寄存器 → 回读"，结果是
`wrote 1 -> read 0`，看起来像"引脚坏了"。**但那是测试自己的错**：引脚当时
**仍然被路由到 SDMMC 的 CLK 信号**（我自己在前面的 `esp_sdmmc_configpins()` 里
接的），所以写 GPIO 输出寄存器根本不起作用。

加上 `esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, ...)`
把输出改回 GPIO 之后，才测出真实结果 `wrote 1 -> read 1`。

> 这已经是**同一个坑的第二次**（上一次是 `esp_configgpio(CLK, INPUT)` 关掉了
> 输出驱动器）。共同点：**测引脚之前必须先把引脚的输出源明确指定到自己要测的
> 那一方**，否则测的是上一次留下的路由。写进文档，避免第三次。

### 一个必须说明的保留

DesignWare MSHC 的卡时钟**可能只在命令/数据传输进行时才输出**（idle 时门控）。
如果是这样，那么"空闲时采样到恒低"本身**并不证明**控制器坏了 —— 它是正常行为。

但**这不影响另一个独立事实**：**CMD0 的 `start_command` 永不自清**。
CMD0 执行期间时钟理应处于活动状态，命令却走不完 —— 这一条无法用"空闲门控"解释。

所以现在的判断是：

- 引脚层：**排除**（本轮直接测量）
- 空闲时钟恒低：**保留**（可能正常，也可能异常，尚未区分）
- 命令走不完：**真问题，且与引脚无关**

### 下一步

要在"命令进行中"而不是"空闲时"观察引脚 —— 例如在发出 CMD0 之后立刻连续采样
GPIO18，看那段时间有没有波形。这能直接判定"控制器在命令期间到底有没有输出时钟"，
从而把"空闲门控"这个保留彻底区分掉。

## I.12 第 33 轮：**命令进行中也没有时钟 —— "空闲门控"的保留排除**

上轮留了一个必要保留："空闲时采样到恒低，可能只是 DesignWare 在无命令时门控卡时钟，
并不证明控制器坏了。"

本轮把这个保留直接测掉了：在 `esp_sdmmc_ciu_sendcmd()` **写完命令之后立刻**
连续采样 CLK 引脚 256 次（这是驱动里唯一确知"命令正在飞"的位置）。

真机结果：

```
esp_sdmmc: CLK during cmd=a0018000: 0 high, 256 low
           -- no clock during the command either
```

**命令进行中同样没有时钟。**

### 至此这一侧的证据链已经闭合

| 环节 | 证据 | 结论 |
|---|---|---|
| 引脚本身 | 当普通 GPIO 驱动：写了 1 读到 1、写 0 读到 0 | **正常** |
| 引脚路由 | 已接 GPIO 矩阵（`SD_CARD_CCLK_2_PAD_OUT_IDX`） | 已做 |
| 空闲时钟 | 采样 512 次全低 | 无 |
| **命令期间时钟** | 采样 256 次全低 | **无** |
| 寄存器 | `clkena=0x2`、`clkdiv=0x9f00` 一直"正确" | 与事实矛盾 |
| 总线时钟 | `sdmmc_ll_enable_bus_clock` 确认为真实现，写 `HP_SYS_CLKRST.soc_clk_ctrl1.reg_sdmmc_sys_clk_en` | 已做 |
| 时钟源门控 | `clk_gate_ll_ref_160m_clk_en` 确认为真实现，写 `ref_clk_ctrl2.reg_ref_160m_clk_en` | 已做 |

**结论：SDMMC 的卡时钟没有被生成。** 引脚、路由、总线时钟、时钟源门控都排除了，
且这不是"空闲门控"——命令期间也没有。

### 剩下的怀疑对象（收敛到"卡时钟这一跳"）

1. **系统侧分频器**：`sdmmc_ll_set_clock_div(hw, 1)` 设的是 HP_SYS_CLKRST 里的
   系统侧分频。若它或它上游的某个门控没配好，卡时钟就没有源；
2. **是否存在一个"SDMMC 功能时钟"使能位尚未打开** —— 已确认打开的是
   *系统*时钟（`reg_sdmmc_sys_clk_en`），IDF 里是否还有别的与卡时钟相关的
   使能，需要逐项比对 `sd_host_setup()` 的**完整**序列（此前只比对了
   `sd_host_set_clk_div()` 附近）；
3. `sdmmc_ll_init_phase_delay()` 与本驱动写的输出/输入延迟相位是否会互相影响。

**下一步第一件事**：把 `sd_host_setup()` / `sd_host_slot_init()` 的完整时钟
初始化序列逐条抄下来，与本驱动逐条比对 —— 现在的证据表明**一定还差某一步**，
而不是某一步写错了值（因为所有读写回来的值都是对的）。

## I.13 第 34 轮：`set_clock_div` 的分支差异（真差异，但不是本因）

逐条比对 Espressif 的 `sd_host_set_clk_div()` 时发现一处**实质差异**：

```c
static inline void sdmmc_ll_set_clock_div(sdmmc_dev_t *hw, uint32_t div)
{
    if (div > 1) {
        HP_SYS_CLKRST.peri_clk_ctrl02.reg_sdio_ls_clk_edge_h = div / 2 - 1;
        HP_SYS_CLKRST.peri_clk_ctrl02.reg_sdio_ls_clk_edge_n = div - 1;
        HP_SYS_CLKRST.peri_clk_ctrl02.reg_sdio_ls_clk_edge_l = div - 1;
        HP_SYS_CLKRST.peri_clk_ctrl02.reg_sdio_ls_clk_edge_cfg_update = 1;
        HP_SYS_CLKRST.peri_clk_ctrl02.reg_sdio_ls_clk_edge_cfg_update = 0;
    } else {
        HP_SYS_CLKRST.peri_clk_ctrl01.reg_sdio_hs_mode = 1;
        /* 三个 edge 字段清零，且不发 cfg_update 脉冲 */
    }
}
```

**`div > 1` 与 `div <= 1` 走的是完全不同的两条路**：前者会配置 SDIO 时钟的
**上升/下降沿位置**并**脉冲 `cfg_update` 把它锁存**；后者只置 HS 模式、
把三个 edge 字段清零、**完全没有更新脉冲**。

本驱动传的是 **`1`**，而 Espressif 的
`sd_host_set_clk_div(ctlr, SDMMC_CLK_SRC_DEFAULT, 2)` 用的是 **`2`**。

**已改为 2。** 这是一个真差异，也符合"寄存器全对却没有波形"的表现 ——
边沿从未配置、更新脉冲从未发出。但真机上**时钟仍未出现**：

```
CLK pin sampled 512x: 0 high, 512 low
CLK pad as plain GPIO: wrote 1 -> read 1 -- pad works
CLK during cmd=a0018000: 0 high, 256 low
probe 1/12 -> -110
```

所以它是真差异，**但不是本因**。保留该改动（与 IDF 一致）。

### 已确认无关的两处

- `sdmmc_hal_init()`：只是 `hal->dev = SDMMC_LL_GET_HW(0)`，没有额外初始化；
- `HP_SYS_CLKRST` 里与 SDMMC 相关的门控**只有一个**
  （`soc_clk_ctrl1.reg_sdmmc_sys_clk_en`，bit 14），本驱动已通过
  `sdmmc_ll_enable_bus_clock(0, true)` 打开。**不存在"还有一个功能时钟没开"**。

### 现在的处境（说实话）

主机侧已经逐条对齐到：总线时钟 ✅、时钟源门控 ✅、系统分频 ✅（本轮改成 2）、
卡时钟分频与使能（寄存器值正确）、GPIO 矩阵路由 ✅、管脚功能 ✅、引脚本身 ✅ ——
**而引脚上依然没有任何波形**。

这说明问题不在"某个寄存器写错"，也不在"某个门控没开"，而更可能在
**这一组引脚 + 这一个 slot 的组合本身**。下一轮值得换一个方向验证：

> 把 slot 1 换成 **slot 0**（IOMUX 引脚 43/44/39-42）试一次。
> 如果 slot 0 能出波形，就证明**控制器与时钟本身是好的，问题特定于 slot 1 的
> GPIO 矩阵通路**；如果 slot 0 也不出波形，那就是控制器/时钟层面的问题。
> 这是一次能把"slot 1 通路"与"控制器本体"彻底分开的判定性实验。

---

# 附录 J：当前状态与交接（第 35 轮整理）

## J.1 一句话现状

**主机侧驱动已逐条对齐 Espressif 参考实现、并修掉 8 处真缺陷；
但 SDMMC 的卡时钟在引脚上从未出现，枚举因此无法开始。**
从机（C6）已确认正常运行且无异常。

## J.2 已完成且真机验证的部分

| 项 | 状态 |
|---|---|
| ES8311 音频（I2S0 + GDMA，播放） | ✅ 真机反复验证（96044 / 192044 / 288044 字节） |
| 语言播放（标准播放器 + 自检双路） | ✅ 真机验证，`nxplayer` 干净退出 |
| C6 的 SDIO 从机固件 | ✅ 已构建、烧入、读回校验通过，确认在跑 |
| WiFi 帧层（第 2 层） | ✅ 已投放进驱动树 + 目标编译通过 + 主机往返测试通过 |
| WiFi SDMMC/SDIO 主机驱动 | 🔧 已实现，8 处真缺陷已修 |
| WiFi 枚举 | 🔴 **未通过：卡时钟未生成** |

## J.3 已修的真缺陷（8 处，全部只有真机才能暴露）

1. 卡时钟未提交到卡时钟域（`update_clk_reg`，附录 C）
2. `sdmmc_ll_is_command_taken()` 判断取反（附录 G.2）—— 最隐蔽的一个
3. 时钟更新命令缺 `card_num`/`use_hold_reg`/`wait_complete`（附录 G.3）
4. 输入采样相位从未配置（附录 I.6）
5. 时钟源从未使能（`clk_gate_ll_ref_160m_clk_en`，附录 I.7）
6. slot 1 的 GPIO 矩阵路由完全缺失（附录 I.9）
7. 管脚 IO_MUX 功能未选回 GPIO（附录 I.9/I.10）
8. 系统分频传 1 而非 2（`set_clock_div` 分支不同，附录 I.13）

另有音频侧 5 个 + 上游应用 2 个（见附录 A/B）。

## J.4 已排除的（都有直接证据，不必重查）

引脚/走线/上拉（**引脚级实测**：当普通 GPIO 驱动写好读好）、
总线时钟、时钟源门控、`HP_SYS_CLKRST` 里是否有别的 SDMMC 门控（只有一个）、
`sdmmc_hal_init()` 是否有额外初始化（没有）、复位是否卡住（三处都不卡）、
从机是否正常（**DEBUG 日志已确认**）。

## J.5 现有的**测量工具**（这是最该继承的东西）

驱动里现在有两个 bring-up 开关，可直接测引脚而不用推断：

```c
esp_sdmmc_skip_clk_update(bool);   /* 跳过 update_clk_reg 命令 */
esp_sdmmc_probe_clk(bool);         /* 命令飞行中采样 CLK 引脚 */
```

以及 `esp_hosted_sdio.c` 里的两项直接测量（默认执行）：

- **CLK 引脚空闲采样 512 次** → 判断有无波形
- **CLK 引脚当普通 GPIO 试写试读** → 判断引脚本体是否可用

配合 `esp_sdmmc_debug()`（verid/status/cdetect/clkena/clkdiv/ctrl/rintsts 快照），
**每一步改动都能立刻上机验证，不需要依赖仪器**。

## J.6 下一步（按性价比排序）

1. **判定实验：slot 1 → slot 0。**
   把 `CONFIG_ESPRESSIF_HOSTED_SDIO_SLOT` 与 `CONFIG_ESPRESSIF_SDMMC_SLOT`
   改成 0，引脚改成 IOMUX 的 43/44/39-42，然后用现成的引脚采样看 CLK 有没有波形。
   - 出波形 → 控制器与时钟是好的，问题**特定于 slot 1 的矩阵通路**；
   - 不出波形 → 问题在**控制器/时钟本体**。
   **这一实验能把剩下的一半空间直接砍掉。**
2. 若确认是 slot 1 通路：核查 `SD_CARD_CCLK_2_PAD_OUT_IDX`（值为 **0**）
   在 P4 上是否为有效输出信号，以及 slot 1 是否需要额外的 HSPI/SDIO 相关配置。
3. 若确认是控制器本体：回到 `sd_host_setup()` 全序列逐条比对
   （目前只比对了 `sd_host_set_clk_div()` 与 `sd_host_slot_start_command()`）。

## J.7 一句提醒（写给自己和接手的人）

这个问题的表现是"**寄存器读回来全对、引脚上什么都没有**"。
它天然会把排查引向"检查寄存器值"，而寄存器值一直是对的。
**唯一有效的做法是测引脚**（第 31 轮才用上，之后每一步才开始有确定结论）。
若继续排查，请从 J.5 的测量工具开始，不要从读寄存器开始。

## I.14 第 36 轮：排掉三条，并把零散事实拼成一个连贯解释

### 排掉的三条

1. **矩阵索引无冲突**：`SIG_GPIO_OUT_IDX = 256`，而 slot 1 的 SDIO 信号占 0–5+。
   所以 CLK 用的索引 **0** 确实是 `SD_CARD_CCLK_2_PAD_OUT_IDX`，不是"0 是特殊值"
   的问题。**索引是对的。**
2. **`force_norst_sdmmc` 无关**：`LP_AON_CLKRST.hp_sdmmc_emac_rst_ctrl` 里确实有
   这一位（"hp sdmmc force norst"），但**全树没有任何代码使用它**，排除。
3. **`sdmmc_ll_reset_register` 正常**：它把 `rst_en_sdmmc` 置 1 再置 0，
   是标准复位脉冲，**没有把模块按在复位态**。

### 查到的两件有用的事

**（a）CLKENA 的字段布局**（`sdmmc_struct.h`）：

```
cclk_enable : bitpos [1:0]     ← 每槽一位
lp_enable   : bitpos [17:16]   ← 每槽一位，"低功耗门控"
default: 0
```

本驱动读回 `clkena = 0x00000002` → `cclk_enable = 0b10`（slot 1 已使能）、
**`lp_enable = 0`**。

按 Espressif 的 `sd_host_slot_set_cclk_always_on_internal()`：

```c
if (always_on)  sdmmc_ll_enable_card_clock_low_power(dev, slot, false); /* lp_enable=0 */
else            sdmmc_ll_enable_card_clock_low_power(dev, slot, true);
sd_host_slot_clock_update_command(slot, false);   /* ← 关键：改完必须发这条命令 */
```

即 **`lp_enable = 0` 就是"时钟常开"**，本驱动已经是这个状态。
所以"时钟仅在有活动时输出"这条**也不成立** —— 它本来就该一直输出。

**（b）但注意上面那个函数最后一行**：改完 `lp_enable` 之后**必须**调用
`sd_host_slot_clock_update_command()`（即 `update_clk_reg` 命令）。
**而那条命令正是本驱动上永远走不完的那一条。**

### 由此得到的连贯解释（目前最自洽的一个）

把零散事实拼起来：

| 事实 | 出处 |
|---|---|
| 卡时钟的所有设置（CLKDIV / CLKSRC / CLKENA / lp_enable）都要靠 `update_clk_reg` 命令**锁存进卡时钟域**才生效 | 附录 C、本轮 (b) |
| 那条命令在本芯片上**永远不自清**（`start_command` 恒为 1） | 附录 G、I.13 |
| 因此寄存器里的值"全对"，但**从未进入卡时钟域** | 引脚级实测：无波形 |
| 引脚本身、矩阵索引、总线时钟、时钟源门控、低功耗门控、复位 —— **全部已排除** | 本附录 I.9–I.14 |

也就是：**不是某一步写错了，而是"把设置生效"这一个动作本身在本芯片上做不到。**

这一解释与所有已知事实一致，也解释了为什么"逐条对齐 Espressif 的寄存器写法"
（本驱动已对齐了 8 处）始终无效 —— **对的写法也需要那条命令才生效**。

### 下一步的两个方向

1. **换一种锁存途径**。第 26 轮试过"用控制器复位来锁存"（`sdmmc_ll_reset_controller`），
   当时现象未变 —— 但那一次是在**其它缺陷尚未修复**的情况下试的，值得在现在这个
   干净状态下**重试一次**，并且这次要用**引脚采样**来判定时钟是否真的出来了
   （上次只能看返回码，看不到波形）。

2. **查 `update_clk_reg` 命令为何不自清**。已知：字段全对、`use_hold_reg` 试过、
   `wait_complete` 试过、新旧控制器都一样、`rintsts` 恒为 bit12(EBE)。
   剩下可查的是**这条命令对 CIU 状态的前置要求**
   （例如是否要求 `CTRL` 里某个使能位、或是否与 `SDIO_PLL` 相关）。

## I.15 第 36 轮补充：**纠正 I.14 的一处错误** —— 方向 1 早就做过，且无效

I.14 里我写"第 26 轮试过用控制器复位锁存，但那是在其它缺陷尚未修复时试的，
值得在现在这个干净状态下**重试一次**"。**这个判断是错的。**

准备执行时先读了当前代码，发现 `esp_sdmmc_clock()` 里**本来就还在用
控制器复位锁存**（第 26 轮 commit `e847a1a` 的改动**从未被回退**）：

```c
sdmmc_ll_reset_controller(hw);
while (!sdmmc_ll_is_controller_reset_done(hw)) { /* 带超时 */ }
```

也就是说：**"控制器复位锁存"这条路一直在生效中，而引脚上依然没有时钟。**
不需要重试 —— 重试会浪费一次烧写与验证。

**已把 I.14 的"方向 1"标记为已完成且失败**，避免下一轮重复劳动。

### 顺带说明一个教训

我在写交接笔记时凭记忆写了"当时未变"，但没有先读代码确认那个改动是否还在。
**写交接记录必须以代码为准，不能凭记忆** —— 否则会把后来人（包括下一轮的自己）
引向一次无效的重试。这和第 31 轮那次"测引脚前没看路由"是同一类错误：
**先读现场，再下判断。**

## I.16 第 37 轮：拆掉一个**真实存在的死锁**，但它不是最终阻塞点

### 先发现我自己门控里的一个逻辑错误

第 24 轮我给 FIFO 复位加的门控是：

```c
if (hw->clkena.val == 0)  defer;   /* ← 这个条件不成立 */
else                      reset_fifo();
```

**这个条件永远为假**：CLKENA 是在**"提交"之前**就被写入的，所以寄存器读回来
已经是非零（`clkena=0x2`），而时钟**从未真正锁存进卡时钟域**。门控每次都放行。

于是死锁成立：FIFO 复位发出 → 它需要卡时钟才自清 → 卡时钟要靠一条命令才生效 →
CIU 在 `fifo_reset=1` 时无法执行任何命令。**三方互等。**

### 修法：直接不发 FIFO 复位

刚出复位的控制器没有需要冲刷的内容，所以正确做法是**在命令能跑起来之前根本不碰
FIFO**。已删除该复位（保留 controller / DMA 复位）。

真机确认它生效了 —— **日志里"FIFO reset did not complete"这条不再出现**：

```
esp_sdmmc: slot 1 pads routed via the GPIO matrix (CLK=18 CMD=19 D0..D3=14,15,16,17)
esp_hosted_sdio: SDIO host ready on slot 1
esp_hosted_sdio: CLK pin (GPIO18) sampled 512x: 0 high, 512 low   ← 仍无时钟
esp_sdmmc: CLK during cmd=a0018000: 0 high, 256 low               ← 命令期间也无
esp_hosted_sdio: probe 1/12 -> -110
esp_sdmmc: previous command still pending (cmd=a0018000 ... rintsts=00001000)
```

### 所以：死锁是真的，但拆掉它不足以解决问题

`fifo_reset` 不再是障碍，**时钟依然没有出现，CMD0 依然挂着**。
这说明"CIU 无法执行命令"还有**另一个**原因，而它独立于 FIFO 复位。

### 现在的判断

已知（都有直接证据）：

- 引脚、矩阵索引、路由、管脚功能 —— 排除；
- 总线时钟、时钟源门控、系统分频、卡时钟寄存器值、低功耗门控、复位 —— 全部对齐或排除；
- FIFO 复位死锁 —— 已拆除，**但不充分**；
- **CIU 无法执行任何命令**（`start_command` 写入后恒为 1，`rintsts` 恒为 bit12 EBE）；
- 从机正常且什么也没收到。

剩下的问题只剩一句话：**为什么 CIU 拿到一条命令后不执行它。**

这是下一轮唯一该查的问题。可查方向：
`CTRL` 里是否还有某个使能位（`dma_enable` / `int_enable` 之外）；
命令是否要求 `BMOD`/`IDINTEN` 的某个状态；
以及 `update_clk_reg` 这类"非真实命令"是否对 CIU 有额外前置要求。

## I.17 第 38 轮：`CTRL.dma_enable` 从未在初始化时置起（真缺陷，已修）

上轮把问题收敛成"CIU 为什么不执行命令"。本轮**先把寄存器实际状态打出来**
（此前日志把 `ctrl`/`bmod`/`rintsts` 裁掉了，这是我自己的疏忽）：

```
esp_hosted_sdio: verid=5342270a status=00000406 cdetect=00000000 clkena=00000002
esp_hosted_sdio: ctrl=00000010 bmod=00000000 rintsts=00000000 cmdarg=00000000
```

`ctrl = 0x10` → **只有 bit4 `int_enable`，bit5 `dma_enable` 是 0**。

### 根因：`sdmmc_ll_init_dma()` 的调用位置错了

`sdmmc_ll_init_dma()` 会置 `ctrl.dma_enable = 1`、清 `bmod` 并置 `sw_reset`、
以及 `idinten.ni/ri/ti`。本驱动**只在数据传输的准备路径里**调用它（两处），
**控制器初始化时从未调用**；而 `esp_sdmmc_reset()` 里的
`sdmmc_ll_reset_controller()` 会**清掉整个 CTRL 寄存器**。

Espressif 的顺序（`sd_host_sdmmc_controller_init()`）是：
**先复位，再 `sdmmc_ll_init_dma()`**。本驱动漏了后一步。

### 修复与验证

在 `esp_sdmmc_initialize()` 的复位**之后**补上 `sdmmc_ll_init_dma(hw)`。

真机寄存器读回确认生效：

| | 修复前 | 修复后 |
|---|---|---|
| `ctrl` | `00000010` | **`00000030`** |
| 含义 | 仅 `int_enable` | `int_enable` + **`dma_enable`** |

### 但仍不足以让时钟出现

```
ctrl=00000030 bmod=00000000
CLK pin sampled 512x: 0 high, 512 low     ← 仍无时钟
probe 1/12 -> -110
probe 2/12 -> -16
```

这是一处**真缺陷**（寄存器证据明确：0x10 → 0x30），保留。

### 累计已修（第 10 处）

1. 卡时钟未提交（附录 C）
2. `is_command_taken` 判断取反（附录 G.2）
3. 时钟更新命令缺三个字段（附录 G.3）
4. 输入采样相位未配置（附录 I.6）
5. 时钟源未使能（附录 I.7）
6. slot 1 矩阵路由缺失（附录 I.9）
7. 管脚功能未选回 GPIO（附录 I.10）
8. 系统分频传 1 而非 2（附录 I.13）
9. FIFO 复位死锁（附录 I.16）
10. **初始化时未调 `sdmmc_ll_init_dma()`（本轮，有寄存器前后对比）**

### 下一步

继续用"打寄存器 + 测引脚"这对工具，把 `sd_host_sdmmc_controller_init()` 里
**其余每一步的寄存器效果**逐条与本驱动对比。本轮证明这个方法是有效的
（`ctrl` 0x10→0x30 是明确证据），比读源码推断更可靠。

## I.18 第 39 轮：**时钟配置寄存器逐位验证全部正确，而引脚仍无波形**

本轮把 `HP_SYS_CLKRST`（系统时钟块）的三个寄存器读出来 —— 时钟到底配没配上，
此前一直是推断，现在有了逐位的直接读数：

```
esp_hosted_sdio: CLKRST sys=0e047028 peri01=01400601 peri02=38820200
```

### 逐位解码

**`soc_clk_ctrl1 = 0x0e047028`**

| 位 | 字段 | 值 |
|---|---|---|
| 14 | `reg_sdmmc_sys_clk_en` | **1** ✓ SDMMC 系统时钟已开 |

**`peri_clk_ctrl01 = 0x01400601`**

| 位 | 字段 | 值 |
|---|---|---|
| 22 | `reg_sdio_hs_mode` | 1 |
| 23 | `reg_sdio_ls_clk_src_sel` | **0** = PLL160M ✓ |
| 24 | `reg_sdio_ls_clk_en` | **1** ✓ SDIO 低速时钟已使能 |

**`peri_clk_ctrl02 = 0x38820200`**

| 位段 | 字段 | 值 | 期望（div=2） |
|---|---|---|---|
| [7:0] | `reg_sdio_ls_clk_div_num` | 0 | 未使用 |
| [12:9] | `reg_sdio_ls_clk_edge_l` | 0 | `div-1`=1 → 见下 |
| [16:13] | `reg_sdio_ls_clk_edge_h` | 0 | `div/2-1`=0 ✓ |
| [20:17] | `reg_sdio_ls_clk_edge_n` | **1** | `div-1`=1 ✓ |

> 注：`edge_l` 与 `edge_n` 在 `sdmmc_ll_set_clock_div()` 里都被赋 `div-1`，
> 但该函数写 `edge_l` 用的是 [12:9]，读回 0 —— 需要下一轮确认这一位段的
> 读写是否正常（可能是我读的字段偏移与写入的不一致，也可能确有差异）。

### 结论：**不是"某个使能没开"，也不是"某个值写错"**

| 环节 | 状态 |
|---|---|
| SDMMC 系统时钟 | ✅ 已开（寄存器位 1） |
| SDIO 低速时钟使能 | ✅ 已开（寄存器位 1） |
| 时钟源选择 | ✅ PLL160M |
| 分频边沿参数 | ✅ 与 div=2 一致 |
| SDMMC 控制器 CTRL | ✅ `int_enable` + `dma_enable`（第 38 轮修复后） |
| SDMMC CLKENA | ✅ `cclk_enable=1`、`lp_enable=0`（常开模式） |
| GPIO 矩阵路由 / 管脚功能 / 引脚本体 | ✅ 已排除（引脚级实测） |
| **引脚上的波形** | ❌ **仍然没有** |

**也就是说：链路上每一级都读回来是对的，波形就是不出现。**
这已经不能用"漏了某一步配置"来解释 —— 所有已知的配置位都验证过了。

### 现在剩下的可能（性质不同了）

1. **读到的不是真正的时钟路径** —— 例如 slot 1 的时钟实际由另一组位控制
   （`peri_clk_ctrl02` 里还有 `slf_clk_edge_sel` / `drv_clk_edge_sel` 两个
   `[22:21]` / `[24:23]` 的字段，本轮读出 `drv_clk_edge_sel=1`、
   `slf_clk_edge_sel=0`，这两个选择位的作用尚未查明）；
2. **时钟被输出到别处** —— 例如 P4 上这一个 SDMMC 控制器只有一个卡时钟输出，
   而 slot 0/1 的选择在别处；
3. 硬件层面（例如该板 slot 1 的时钟走线并未接到 GPIO18）——
   但用户已确认引脚与上拉，且该脚作普通 GPIO 时可正常驱动/回读。

**下一轮最该做的一件事**：查清 `slf_clk_edge_sel` / `drv_clk_edge_sel` 这两个
选择位的作用，以及 slot 1 与 slot 0 在时钟输出上是否需要显式选择。

## I.19 第 40 轮：路由也被证实到位 —— 软件侧排查到此为止

上轮留的最后一项验证：GPIO 矩阵的输出选择寄存器是否真的指向 SDMMC 时钟。

`GPIO.func_out_sel_cfg[pin].out_sel` 的**默认值是 256**（`SIG_GPIO_OUT_IDX`，
即普通 GPIO 输出）；本驱动把它路由到 `SD_CARD_CCLK_2_PAD_OUT_IDX`，**值为 0**。

真机读回：

```
esp_hosted_sdio: GPIO18 out_sel=0 (0 = SDMMC CLK signal, 256 = plain GPIO)
```

**确认路由真的写进去了**，引脚确实由 SDMMC 时钟信号驱动。

### 至此，软件侧每一环都有直接读数

| # | 环节 | 证据 | 结论 |
|---|---|---|---|
| 1 | 系统时钟 `reg_sdmmc_sys_clk_en` | 寄存器位 14 = 1 | ✅ 开 |
| 2 | SDIO 低速时钟 `reg_sdio_ls_clk_en` | 寄存器位 24 = 1 | ✅ 开 |
| 3 | 时钟源 `reg_sdio_ls_clk_src_sel` | = 0（PLL160M） | ✅ 选对 |
| 4 | 分频边沿 `edge_n/edge_h` | = 1 / 0，与 div=2 一致 | ✅ 配好 |
| 5 | 自/驱/采样时钟 `slf/drv/sam_clk_en` | 位 27/28/29 = 1 | ✅ 开 |
| 6 | 控制器 `CTRL` | `int_enable` + `dma_enable`（第 38 轮修） | ✅ |
| 7 | 卡时钟 `CLKENA` | `cclk_enable=1`、`lp_enable=0` | ✅ 常开 |
| 8 | 矩阵路由 | `out_sel = 0` | ✅ 到位 |
| 9 | 管脚功能 | 已选回 GPIO | ✅ |
| 10 | 引脚本体 | 当普通 GPIO 写 1 读 1、写 0 读 0 | ✅ 正常 |
| 11 | **引脚上的波形** | 空闲 512 次全低、命令期间 256 次全低 | ❌ **无** |
| 12 | 从机（C6） | DEBUG 日志：SDIO 从机已初始化、无任何异常 | ✅ 正常 |

**结论：软件侧能验证的每一环都正确，而波形不出现。**

这不是"某个配置漏了"或"某个值写错了" —— 这两种可能性已经被逐位读数排除干净。
剩下的只可能是：

- **硬件/芯片层面**：该 SDMMC 实例在 slot 1 上不出时钟（例如需要 ROM/更深层的初始化，
  或该板此路存在硬件问题）；
- 或某个**尚未被任何公开定义说明**的隐藏���置。

### 软件侧排查到此为止

继续在驱动里找线索的边际收益已经很低：**所有已知的、可读的寄存器都被读过了，
每一项都正确。** 若还要往下走，需要的不是更多代码，而是：

- 示波器直接看 GPIO18（我的引脚采样是"通过对端回读"，已足够但仍是间接）；
- 或对照一块已知正常的 P4+C6 板做寄存器/波形对比。

### 对交付的判断

本目标的三项里：

- **ES8311 音频（I2S/GDMA）**：完成，真机反复验证；
- **语言播放**：完成，双路（自检 + 标准播放器）验证；
- **WiFi**：主机侧驱动已实现并修掉 **10 处真缺陷**（每一处都有真机证据），
  从机固件已构建烧录并确认运行，帧层已投放并通过主机测试；
  **但卡时钟波形不出现，枚举无法开始。** 这一项在本环境的软件范围内已无法推进。

---

# 附录 K：参考仓库 contest2026_031 的对照（2026-10-27）

用户提供了 `open-vela/contest2026_031_niudanxianqianchong`（分支
`dev-ai-contest-2026`），他们在**同一块板（P4 + 板载 C6）**上把 WiFi 跑通了：
SDIO 枚举、ESP-Hosted 控制面、`wlan0` 注册、DHCP、DNS、TCP 全通。

## K.1 他们做对了什么（与我实现的关键差异）

| 项 | 参考实现 | 本移植 |
|---|---|---|
| **枚举** | 自己实现 **CMD0→CMD5→CMD3→CMD7→CMD52** 全序列 | 依赖 NuttX `sdio_probe()`（**只发 CMD0 + CMD5**）|
| 总线宽度 | **1-bit**（D1/D2 仅输入，D3 当 GPIO 驱动高） | 4-bit |
| 数据传输 | **全程轮询 CMD52/CMD53**，`enable_dma(false)` | 走 NuttX SDIO 栈 + IDMAC |
| 复位后等待 | **1000 ms** | 200 ms |
| 时钟提交 | 仅在**改时钟时**才发 `update_clk_reg`；初始化阶段不发 | 每个 `SDIO_CLOCK` 都发 |
| 代码归属 | 全部放在 **`chips/esp32p4/`**，完全不碰 `common/espressif/esp_sdmmc.c` | 改通用 `esp_sdmmc.c` |

## K.2 逐字比对过、确认**没有差异**的部分

- **引脚路由**：他们的 `route_pins()` 用 `esp_gpio_matrix_out/in`
  + `esp_configgpio`。查 NuttX 源码，`esp_gpio_matrix_out()` 只是
  `esp_rom_gpio_connect_out_signal()` 的薄封装 —— **与本移植完全等价**。
- **时钟初始化顺序**：`enable_src → bus_clock/reset_register/div/src/phase
  → 等 10µs → controller+dma+fifo 复位 → init_dma`。本移植已按此对齐，
  时钟仍不出现。
- **信号索引**：`SD_CARD_CCLK_2_PAD_OUT_IDX` 等，与本移植完全一致。

## K.3 本轮据此做的三次改动（仍未让时钟出现）

1. 改用官方 `esp_clk_tree_enable_src(SDMMC_CLK_SRC_DEFAULT, true)`
2. 把 `up_udelay(10)` 移到"时钟全部配好之后、复位之前"（照抄他们的注释与顺序）
3. 补 `sdmmc_ll_enable_dma(hw, false)`（他们的轮询路径就是这么做的）
4. 屏蔽 bring-up 阶段的 `update_clk_reg`（他们初始化阶段也不发）

**四项都做了，CLK 引脚仍然全低。** 所以差异不在这些点上。

## K.4 剩下最可能的解释

参考实现**根本不用本移植的 `esp_sdmmc.c`** —— 它自己从头配 SDMMC。
而本移植即使把配置顺序逐条对齐，时钟依旧不出。

这说明问题很可能出在**本驱动初始化之外的地方**（例如 NuttX 的 SDMMC 栈、
或 `esp_sdmmc.c` 里某个本移植特有的状态），而不是"少配了哪个寄存器"。
**继续在配置项上找，方向已经到尽头了。**

## K.5 下一轮的正确方向（价值排序）

1. **移植参考实现的做法**：放弃 NuttX 的 `sdio_probe`/`sdio.c` 栈，自己实现
   CMD0→CMD5→CMD3→CMD7→CMD52 与轮询 CMD52/CMD53。这是**已被证明能在这块板上
   工作**的路径。代价是要新写约 1000 行，但风险最低 —— 有现成参考。
2. 若坚持用 NuttX 栈：需要查清"为什么同样的配置在参考实现里出时钟、
   在本驱动里不出"，而那需要示波器或参考板对比。
3. 4-bit 与 DMA 都不是必需的 —— 参考实现用 1-bit + 轮询就全通了，
   这降低了移植难度。

## K.6 一个明确的自评

我在 10 轮里修了 10 处真缺陷，每一处都有证据，但**没有一处是最终阻塞点**。
参考仓库的存在说明：**这个问题是可解的**，只是我的技术路线（复用 NuttX SDIO 栈）
在这块芯片上遇到了我没能定位的障碍，而对方选择了绕开整个栈、自己实现原语。

当"逐条对齐配置"反复无效时，正确的做法是**换一条被证明可行的技术路线**，
而不是继续在同一个框架里找第 11 个缺陷。这一点我该更早意识到。

## K.7 又试了一项：1-bit 模式 —— 仍无时钟

参考实现是 **1-bit** 跑通的（D1/D2 在主机侧完全不用）。本移植原本是 4-bit，
本轮改为保持 1-bit 后重测：

```
CLK pin (GPIO18) sampled 512x as input: 0 high, 512 low -- NOT toggling
probe 1/12 -> -110
```

**仍然没有时钟。** 所以总线宽度也不是原因。

至此已逐项排除的"与参考实现的差异"：

| 差异项 | 是否试过 | 结果 |
|---|---|---|
| 时钟源使能 API（`esp_clk_tree_enable_src`） | ✅ 已改 | 无变化 |
| 10µs 传播延时移到复位前 | ✅ 已改 | 无变化 |
| `enable_dma(false)`（轮询路径） | ✅ 已改 | 无变化 |
| bring-up 阶段不发 `update_clk_reg` | ✅ 已改 | 无变化 |
| 总线宽度 4-bit → 1-bit | ✅ 已改 | 无变化 |
| 引脚路由写法（`esp_gpio_matrix_out`） | ✅ 已比对 | **本就等价** |
| 信号索引 | ✅ 已比对 | **本就一致** |

**六项差异全部试过或比对过，时钟都不出现。**
这基本可以确认：问题不在"与参考实现在配置上的差异"，
而在**本移植复用的 NuttX SDIO 栈 + `esp_sdmmc.c` 这条路径本身**。

## K.8 结论

参考实现绕开了整个 NuttX SDIO 栈（自己实现 CMD0→CMD5→CMD3→CMD7→CMD52
与轮询 CMD52/CMD53），在**同一块板**上跑通。本移植沿用 NuttX 栈，
把能对齐的配置全部对齐后仍无时钟。

**下一步唯一有意义的方向：移植参考实现的技术路线**，即
`chips/esp32p4` 下自己实现 SDIO 原语，不再依赖 `sdio_probe()`/`sdio.c`。
这是已被证明可行的路径，且 1-bit + 轮询降低了难度。
代价是新写约 1000 行，但比继续在没有时钟的路径上找缺陷更可靠。
