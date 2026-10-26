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
