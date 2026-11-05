# 02 — 活体取证：现在跑在 C6 里的到底是哪个镜像

- 取证人：`device-forensics`（shared task `task-2`）
- 取证时间：2026-11-04 22:11 起（Asia/Shanghai）
- 取证方式：**纯只读**。未打开 / 未写入 / 未复位 `/dev/ttyUSB0`、`/dev/ttyUSB1`、`/dev/ttyACM0`；
  未使用 esptool 连串口、未烧录。设备输出全部经 `mcp__tabby__get_terminal_buffer` 读取。
  磁盘镜像分析全部离线（`python3 -m esptool image_info <bin>` + 直接解析 `esp_app_desc_t`）。
- 数据源：
  - C6 从机 `/dev/ttyUSB1` → sessionId `12930a8c-ee87-4740-b4ee-cf9aa297d6f4`，`totalLines=7721`
    （已按 `0-80 / 80-630 / 630-1180 / 1180-1730 / 1730-2280 / 2280-2830 / 2830-3380 / 3380-3930 /
    3930-4480 / 4480-5030` 分段覆盖；5030-7721 段为同一 boot↔错误簇的重复，未逐行回显）
  - P4 主机控制台 `/dev/ttyUSB0` → sessionId `00c17097-68c2-44c8-bf66-a5331a92be5f`，`totalLines≈5168`
    （已读 `0-120` 与 `4965-5165`）

---

## 0. 结论摘要（先看这里）

### 第二问（最关键）：现在 C6 里跑的，和之前那版是同一个镜像吗？

**是同一个。而且它就是仓库里被文档标注为「已知可用组合」的那一个：**

| | 值 |
|---|---|
| 活体 `app_init` Project name | **`eh_cp_wifi_sta`** |
| 活体 App version | **`v3.0.10-dirty`** |
| 活体 Compile time | **`Oct 26 2026 15:25:32`** |
| 活体 ELF file SHA256（打印前 9 位） | **`98147ff44`** |
| 活体 bootloader compile time | `Oct 26 2026 16:24:06` |
| 活体 ESP-Hosted 版本 | `EH-3.0.10`，`Transport used :: SDIO only` |
| 活体 5 段 esp_image 布局 | `36e54 / 0919c / b4d90 / 12728 / 03e30` |

**与会话外的独立判据 100% 一致 → 活体 == `tools/build_flash/c6_hosted/eh_cp_wifi_sta_esp32c6_sdio.bin`**
（`size=1094464`，`md5=3bb11304779b9a5cbeac6b1a7bd01d2c`）。
该文件的 `esp_app_desc_t` 逐字段为：version `v3.0.10-dirty`、project `eh_cp_wifi_sta`、
time `15:25:32` / date `Oct 26 2026`、idf `3ea97e75`、
**elf_sha256 `98147ff449618e914d2120b73bc8f5faeafb9b437135fb866f87c6bdee2b43f3`（前 9 位正是 `98147ff44`）**。

> **重要后果**：C6 **不是**被烧错了镜像 —— 现在这颗 C6 跑的正是文档（`docs/30_实现/应用层_DeskMate_UI.md` §31 /
> `docs/50_排障经验/08_驱动层调优与多副本陷阱.md`）里点名要「刷回」的那一个「已知可用组合」。
> 因此「P4 连不上 WiFi」**不可能**用「C6 固件被换错了」来解释。
>
> ⚠️ 但「已知可用组合」这个称呼本身有歧义：它**只对 SDIO 枚举层成立**。
> 详见下一节（已更正）与 §8。

### 更关键的推论（**已按 lead 更正 + 队友证据收窄**）：11-01 确实连上过，但**不是这颗固件**

> ⚠️ **本节是更正版。** 初版曾断言「没有任何一条记录显示这颗 C6 成功连过 AP」，
> 该断言**不成立**——仓库里有真实成功记录，只是**当时 C6 里是另一颗固件**。更正依据见 §8。

**事实一：11-01 真的跑通过（不是"只到枚举层"）。** 原始行（非转述）：

```
logs/20261101-135139_e2e/boot.txt:1284-1329
eh_hosted_nuttx: DhcpDnsStatus iface=0 link=1 dhcp=1 ip=192.168.1.9
eh_hosted_netdev: wlan0 address 192.168.1.9 mask 255.255.255.0 gw 192.168.1.1 (from the DhcpDnsStatus event)
eh_hosted_nuttx: connected to "Hello_World_2.4G" ssid="Hello_World_2.4G" rssi=-48 ch=6
eh_hosted_netdev: carrier on
eh_hosted_nuttx: Req_GetDhcpDnsStatus(353) -> 0 resp=yes
eh_hosted_nuttx: dhcp status iface=0 link=1 dhcp=1 ip=192.168.1.9
```
同型记录还有 `logs/20261101-081709_connect-run/boot.txt:503`
（`connected to "Hello_World_2.4G" ssid="" rssi=-49 ch=6`）、
`logs/20261101-083220_ip-apply/boot.txt:511`（`rssi=-50 ch=6`）、
`logs/20261101-134028_ip-applied/boot.txt`。
会话外独立锚点：git `46df328`（`2026-11-01 14:10:40 +0800`）标题即
「**验收④闭环——wlan0 拿到地址 192.168.1.9/24 gw 192.168.1.1**」。
⇒ 控制面（scan/connect）与地址配置层（DHCP）**都真通过**。

**事实二：但 11-01 成功窗口里，C6 跑的是 `c6_hosted_new` 那个 network-split 版，不是现在这颗 station 版。**
- 队友 `firmware-historian` 对 `logs/2026-11-02_session-raw.jsonl.zstd`（覆盖 10-31 05:36 → 11-01 14:16）
  做字符串计数：`ehcp_nw_split` ×56、`feat_nw_split` ×135、`eh_example_sta` ×42，
  而 **`wifi_mcu_example` × 0**。
- 我独立复核了这两组 tag 的归属（`strings -a` 逐项）：

  | tag | `c6_hosted/…_esp32c6_sdio.bin`（station） | `c6_hosted_new/…_network_split_station.bin` |
  |---|---|---|
  | `wifi_mcu_example` | **PRESENT** | absent |
  | `ehcp_nw_split` / `feat_nw_split` / `eh_example_sta` | **absent** | **PRESENT** |

  ⇒ `wifi_mcu_example` 是 `eh_cp_wifi_sta` 独有的 app tag；`ehcp_nw_split`/`feat_nw_split`/`eh_example_sta`
  只存在于 network-split 版。**`wifi_mcu_example` 计数为 0，就是"当时那颗不是 station 版"的直接反证。**

**事实三：现在这颗 station 版的结构性缺陷。**
- 活体日志（本文件 §4）：从机 `app_main` 把射频**当成别人的事**——
  `wifi_mcu_example: ESP-Hosted MCU WiFi Example Starting...` → 立刻
  `main_task: Returned from app_main()`，**从机自己从不调用 `esp_wifi_init` / `esp_wifi_start`**。
  于是主机第一个射频 RPC `req_wifi_set_mode` 被拒：
  `E (18757) ehcp_wifi_rpc: req_wifi_set_mode:108 failed [esp_wifi_set_mode(req_payload->mode)] = [12289]`，
  而 `12289 = 0x3001 = ESP_ERR_WIFI_NOT_INIT`（**不是** 0x3002 NOT_STARTED，
  也**不是** `ESP_ERR_WIFI_STATE`——那是另一个码，见 §8.2）。
- 该镜像 `strings` 里**没有** AP 凭据（`Hello_World` / `851285812` 均 absent）
  ⇒ 即使把射频 init 起来，**"谁来连 AP"仍然未定**。
- 11-04 21:30 实测（`logs/20261104-deliver-verify/FINDINGS.md`）：
  `eh_hosted_netdev: connect failed: 12289` / `DM-WIFI-CONNECT ssid='Hello_World_2.4G' rc=-5`
  —— **同一个 12289**，即当前这颗 station 版的症状。

**⇒ 更正后的结论：**
1. 「P4 通过 C6 连上 WiFi」**11-01 真的发生过**（`Hello_World_2.4G`，rssi −48…−50，ch 6，拿到 `192.168.1.9/24`）；
   **但当时跑的是 `c6_hosted_new/eh_cp_network_split_station.bin`（纯网络适配器版）。**
2. 这颗 network-split 版后来被判定**与当前 host 栈不兼容**（`docs/30_实现/应用层_DeskMate_UI.md` §31：
   刷入后主机卡在 MCUboot 之后、串口只出 2061/2084 B），因此被**回退**，C6 换成了现在这颗 station 版。
3. 现在这颗 station 版**不能当网络适配器用**（从机不 init/不 start 射频，且不持 AP 凭据）
   ⇒ 主机每一条射频 RPC 都吃 `0x3001 ESP_ERR_WIFI_NOT_INIT`。
4. 所以「11-04 17:03 刷回已知可用组合」刷回的是**枚举层可用**的那颗；
   它**从未**是"能连 AP"的那颗。**"曾经连上过"和"现在这颗"是两颗不同的固件** ——
   这正是第二问必须收窄的地方。

---

## 1. 活体身份清单（全部来自 C6 `/dev/ttyUSB1` 缓冲）

### 1.1 ROM / bootloader 层

```
ESP-ROM:esp32c6-20220919
Build:Sep 19 2022
rst:0x1 (POWERON),boot:0xc (SPI_FAST_FLASH_BOOT)
SPIWP:0xee
mode:DIO, clock div:2
load:0x40875730,len:0x1604
load:0x4086b910,len:0xd08
load:0x4086e610,len:0x31e0
entry 0x4086b91a
I (23) boot: ESP-IDF 3ea97e75 2nd stage bootloader
I (24) boot: compile time Oct 26 2026 16:24:06
I (24) boot: chip revision: v0.2
I (24) boot.esp32c6: SPI Speed      : 80MHz
I (28) boot.esp32c6: SPI Mode       : DIO
I (31) boot.esp32c6: SPI Flash Size : 4MB
```

`bootloader compile time` 落在磁盘 `tools/build_flash/c6_hosted/bootloader.bin` 的
`esp_bootloader_desc_t` 上（该字段实测为 `26 2026 16:24:06` → `Oct 26 2026 16:24:06`）。

**关于「bootloader 16:24:06 vs app 15:25:32 差了 59 分钟」**：这不是异常，是**两次 `idf.py build` 的产物**。
- app 在 `15:25:32` 构建完成 → 得到 `eh_cp_wifi_sta_esp32c6_sdio.bin`；
- 同一目录里随后（`16:24:06`）又构建了一次 bootloader（`c6_hosted/bootloader.bin`）；
- `c6_hosted/eh_cp_wifi_sta_debug.bin` 的 app 时间是 `16:24:45`（比 bootloader 晚 39 秒），
  说明 `16:24` 这一轮确实是一次**完整的重建**，只不过那轮产出的 **app 是 debug 版**，
  而烧进芯片的 app 是 `15:25:32` 那个 release 版。
- 结论：**bootloader 与 app 时间戳不同 = 用了同一构建目录里不同轮次的两个文件**，
  `flash_c6.sh` 正是这么组合的（`BOOTLOADER="$DIR/bootloader.bin"` + `FW="$DIR/eh_cp_wifi_sta_esp32c6_sdio.bin"`）。
  **不是固件被掉包。**

### 1.2 分区表（活体）

```
I (35) boot: Partition Table:
I (38) boot: ## Label            Usage          Type ST Offset   Length
I (44) boot:  0 nvs              WiFi data        01 02 00009000 00004000
I (51) boot:  1 otadata          OTA data         01 00 0000d000 00002000
I (57) boot:  2 phy_init         RF data          01 01 0000f000 00001000
I (64) boot:  3 ota_0            OTA app          00 10 00010000 001c0000
I (70) boot:  4 ota_1            OTA app          00 11 001d0000 001c0000
I (77) boot: No factory image, trying OTA 0            ← 仅早期若干次出现
I (303) boot: Loaded app from partition at offset 0x10000
I (311) boot: Set actual ota_seq=1 in otadata[0]        ← 早期若干次出现
```

磁盘 `tools/build_flash/c6_hosted/partition-table.bin` 实测条目与上表**逐字段一致**
（`md5=f2b3d62c4664784c4b896843201c3f0d`；与 `c6_hosted_new/partition-table.bin` 同 md5）。
`ota_data_initial.bin` 两目录同 md5（`84d04c9d6cc8ef35bf825d51a5277699`）。

### 1.3 app descriptor（活体）

```
I (319) app_init: Application information:
I (323) app_init: Project name:     eh_cp_wifi_sta
I (328) app_init: App version:      v3.0.10-dirty
I (332) app_init: Compile time:     Oct 26 2026 15:25:32
I (337) app_init: ELF file SHA256:  98147ff44...
I (341) app_init: ESP-IDF:          3ea97e75
I (345) efuse_init: Min chip rev:     v0.0
I (349) efuse_init: Max chip rev:     v0.99
I (353) efuse_init: Chip rev:         v0.2
```

### 1.4 esp_image 段布局（活体）

```
I (81)  esp_image: segment 0: paddr=00010020 vaddr=420c0020 size=36e54h (224852) map
I (130) esp_image: segment 1: paddr=00046e7c vaddr=40800000 size=0919ch ( 37276) load
I (139) esp_image: segment 2: paddr=00050020 vaddr=42000020 size=b4d90h (740752) map
I (279) esp_image: segment 3: paddr=00104db8 vaddr=4080919c size=12728h ( 75560) load
I (296) esp_image: segment 4: paddr=001174e8 vaddr=4081b8d0 size=03e30h ( 15920) load
```

### 1.5 运行时/栈参数（活体，任务要求「没列到的也提出来」）

| 项 | 活体值 |
|---|---|
| CPU | `cpu_start: Unicore app`；`cpu freq: 160000000 Hz` |
| 控制台引脚 | `cpu_start: GPIO 17 and 16 are used as console UART I/O pins` |
| heap | `heap_init: At 40825110 len 00057500 (349 KiB): RAM` / `At 4087C610 len 00002F54 (11 KiB): RAM` / `At 50000000 len 00003FE8 (15 KiB): RTCRAM` |
| flash 探测 | `spi_flash: detected chip: generic`；`flash io: dio` |
| coexist（**coexist 版本，此前未列出**） | `coexist: coex firmware version: 99f7a27`；`coexist: coexist rom version 5b8dcfa` |
| sleep GPIO | `sleep_gpio: Configure to isolate all GPIO pins in sleep state` / `Enable automatic switching of GPIO sleep configuration` |
| ESP-Hosted core | `ehcp_core: ESP-Hosted Firmware version :: EH-3.0.10`；`Transport used :: SDIO only` |
| feature caps | `Supported features are: - WLAN over SDIO`；`Core caps: 0x01  ext_caps: 0x00000000` |
| 扩展描述符 | 4 个：`rpc_mcu`(prio 60) / `feat_system`(120) / `feat_wifi`(200) / `ext_rpc`(5) |
| RPC 注册 | `MCU RPC registered: req [0x0101,0x0197], evt [0x0301,0x0315]`；`++ RPC slow-req id [0x011e]` |
| SDIO 从机参数 | `SDIO_SLAVE: Using SDIO interface (TX_MODE=0)`；`SDIO datapath mode: SW_AGGR`；`rx_buf_size=15872`；`advertising transfer size 1536` |
| SDIO 初始化时内存 | `[mem:sdio_init] free=291320 min=291320 largest=270336` |
| 协商结果 | `Host capabilities: 44`；`RPC version negotiated: V2`；`Negotiation complete: wire_hdr=V1 (12-byte legacy), rpc=V1 (variant-specific)` |
| iTWT | `itwt_evt_pub: iTWT event publisher initialized`；`ehcp_wifi_itwt: iTWT feature extension init ok` |

---

## 2. 候选镜像对照（离线 `esptool` + `esp_app_desc_t` 解析）

`esptool image_info` **不打印 app descriptor**，所以下面每组都用「`image_info` 段布局」+
「从文件偏移 `0x20` 直接解析 `esp_app_desc_t`」两条独立路径得出。

| 候选文件 | size (B) | md5 | Project name | App version | Compile time | ELF SHA256(前9) | ESP-IDF | 段布局 (len@load) |
|---|---|---|---|---|---|---|---|---|
| **活体（C6 日志）** | — | — | **`eh_cp_wifi_sta`** | **`v3.0.10-dirty`** | **`Oct 26 2026 15:25:32`** | **`98147ff44`** | `3ea97e75` | `0x36e54@0x420c0020, 0x0919c@0x40800000, 0xb4d90@0x42000020, 0x12728@0x4080919c, 0x03e30@0x4081b8d0` |
| `c6_hosted/eh_cp_wifi_sta_esp32c6_sdio.bin` | 1094464 | `3bb11304779b9a5cbeac6b1a7bd01d2c` | `eh_cp_wifi_sta` | `v3.0.10-dirty` | `Oct 26 2026 15:25:32` | `98147ff44` | `3ea97e75` | `0x36e54@0x420c0020, 0x0919c@0x40800000, 0xb4d90@0x42000020, 0x12728@0x4080919c, 0x03e30@0x4081b8d0` |
| `c6_hosted/eh_cp_wifi_sta_debug.bin` | 1131952 | `6099d612fb46ebbd5dacf24b7122b005` | `eh_cp_wifi_sta` | `v3.0.10-dirty` | `Oct 26 2026 16:24:45` | `017ed307e` | `3ea97e75` | `0x3ca84@0x420c0020, 0x0356c@0x40800000, 0xb7fa8@0x42000020, 0x185cc@0x4080356c, 0x03fdc@0x4081bb40` |
| `c6_hosted_new/eh_cp_network_split_station.bin` | 1115248 | `19c9262ac8e97712a6589ae4e7732bfe` | `eh_cp_network_split_station` | `1` | `Oct 28 2026 20:29:15` | `9e718d166` | `3ea97e75` | `0x386dc@0x420c0020, 0x07914@0x40800000, 0xb844c@0x42000020, 0x14194@0x40807914, 0x03e30@0x4081bab0` |
| `test/c6_sdio_minimal/prebuilt/c6sdiotest.bin` | 175392 | `20cac317249800202d253d8c102e938c` | `c6sdiotest` | `1` | `Oct 29 2026 14:54:41` | `3987e457a` | `3ea97e75` | `0x09788@0x42020020, 0x06868@0x40800000, 0x13d90@0x42000020, 0x05680@0x40806868, 0x018b8@0x4080bef0` |

### 逐字段判定

| 字段 | 活体 | `..._sdio.bin` | `..._debug.bin` | `..._split_station.bin` | `c6sdiotest.bin` |
|---|---|---|---|---|---|
| Project name | `eh_cp_wifi_sta` | ✅ 一致 | ✅ 一致 | ❌ | ❌ |
| App version | `v3.0.10-dirty` | ✅ 一致 | ✅ 一致 | ❌ (`1`) | ❌ (`1`) |
| Compile time | `Oct 26 2026 15:25:32` | ✅ **一致** | ❌ (`16:24:45`) | ❌ (`Oct 28 …`) | ❌ (`Oct 29 …`) |
| ELF SHA256 前 9 位 | `98147ff44` | ✅ **一致** | ❌ (`017ed307e`) | ❌ (`9e718d166`) | ❌ (`3987e457a`) |
| ESP-IDF | `3ea97e75` | ✅ | ✅ | ✅ | ✅ |
| seg0 len@vaddr | `36e54@420c0020` | ✅ | ❌ | ❌ | ❌ |
| seg1 len@vaddr | `0919c@40800000` | ✅ | ❌ | ❌ | ❌ |
| seg2 len@vaddr | `b4d90@42000020` | ✅ | ❌ | ❌ | ❌ |
| seg3 len@vaddr | `12728@4080919c` | ✅ | ❌ | ❌ | ❌ |
| seg4 len@vaddr | `03e30@4081b8d0` | ✅ | ❌ | ❌ | ❌ |
| Entry point | — | `408002e8` | `408002e8` | `408002e8` | `408002ea` |
| 镜像大小／分区可容纳 | — | 1094464 B（ota_0 上限 0x1c0000） | 1131952 B ✅ | 1115248 B ✅ | 175392 B ✅ |

### 判定与判据强度

- **判定：活体 == `tools/build_flash/c6_hosted/eh_cp_wifi_sta_esp32c6_sdio.bin`。**
  不是 debug 版，不是 `c6_hosted_new` 的网络适配器版，不是 `c6sdiotest`。
- **判据强度：极强（本文件中最高等级）。** 理由是命中了三类**互相独立、且不能伪造**的指纹：
  1. **ELF SHA256**（`98147ff449…`）：这是 IDF 构建时对 ELF 内容做的 SHA-256 并嵌进 app 描述符，
     它同时覆盖代码、数据、版本串和构建时间 —— 前 9 位 `98147ff44` 与日志打印完全一致；
  2. **编译时间戳** `Oct 26 2026 15:25:32`（minute/秒级）；
  3. **5 段 `esp_image` 的 (len, vaddr) 组合**：5 元组全中，debug 版与 split_station 版在这 5 项上**无一命中**。
  另外 bootloader 时间戳 `16:24:06` 也精确落在 `c6_hosted/bootloader.bin` 上，
  说明整条烧录链是 `c6_hosted/` 那一套（`flash_c6.sh` 的 app 偏移 `0x10000` 与活体
  `Loaded app from partition at offset 0x10000` 也吻合）。
  **不存在「活体不在磁盘上」的可能。**
- 补充：活体镜像里 `strings` 能搜到 `WiFi init config changed`、
  `esp_wifi_set_mode`、`ESP-Hosted MCU WiFi Example`；
  **搜不到** AP 凭据 `Hello_World` / `851285812`。即：代码里**有**射频初始化的路径，
  但**没有**内置 AP 凭据，且这次启动**没有走**那条路径（见 §4）。

---

## 3. 这颗 C6 自烧录以来被重启过几次 / 固件换过没

### 3.1 缓冲里能看到的

7721 行缓冲里，应用级启动（`wifi_mcu_example: ESP-Hosted MCU WiFi Example Starting...`）
出现**约 14 次**，**每一次的 app descriptor 都是同一份**
（`eh_cp_wifi_sta` / `v3.0.10-dirty` / `Oct 26 2026 15:25:32` / `98147ff44` / 同样的 5 段布局）。

其中时间戳严格单调上升、且带完整 host 交互的只有 **3 个**：

| # | 应用启动后 `ESP_OPEN_DATA_PATH` 时刻 | `Send slave up event` | `advertising transfer size 1536` | 是否收到 `Slave init_config` |
|---|---|---|---|---|
| 1 | `I (1540)` | ✅ | ✅ | ❌ 之后立刻又 ROM boot |
| 2 | `I (13966)` | ✅ | ✅ | ❌ 之后立刻又 ROM boot |
| 3 | `I (18116)` | ✅ | ✅ | ✅ `I (18436) Slave init_config received from host` |

**每次重启后 RPC 序列并不相同**：只有第 3 次走到了完整的
`init_config → Host capabilities: 44 → RawTP Stop → RPC V2 协商 → ESP init event → req_wifi_set_mode`；
前两次都停在 `advertising transfer size 1536` 就被下一次 host reset 打断。
**复位原因**：所有重启都报 `rst:0x1 (POWERON)`，并且 P4 侧明确说这是它干的 ——
```
esp_hosted_sdio: controller reset + clock reprogrammed, now resetting the companion (500 ms low, then 10 s to come up)
```
即 `rst:0x1 (POWERON)` 在这条链路上是**主机拉复位脚**的结果，不是芯片自己掉电。

> 读日志时要注意：缓冲里有些行开头被 UART 未同步字节污染，例如 `E BOD�ESP-ROM:`、
> `E BOD: B`、`E BO�`、`E"`、`I (345) efuse_init: Miev: v0.99`、`I (317) cpu_start: GPIO 17…`
> 被截成半行。这些是**主机复位瞬间的 UART 残字节**，不要当成 `BOD`（brown-out）错误来解读 ——
> 同框还出现了 `esp_hosted_sdio0:  bctosm=p0a0n0i2o5n7 frfe scettl:=…` 这种两行交错的乱码，
> 属于同一现象。**缓冲里没有任何一条 `Brownout`/`brownout` 字样。**

### 3.2 烧录来源与时间线（会话外证据）

| 时间 | 事件 | 证据 |
|---|---|---|
| 2026-10-26 15:25:32 | 构建 `eh_cp_wifi_sta_esp32c6_sdio.bin` | `esp_app_desc_t.time/date`；git `0c49422`（2026-10-26，`feat(wifi): 构建出 C6 的 SDIO 从机固件并随交付收录`） |
| 2026-10-26 16:24:06 / 16:24:45 | 同目录重建 bootloader / debug app | 磁盘两个文件描述符 |
| 2026-10-30 | 烧入 `1094464 B @ 0x10000` + `Hash of data verified`（= station 版） | `logs/2026-10-30_wifi-sdio-session.md:7775-7776`（同文件 `:19254` 另有一次 `1115248 B` = split_station 版） |
| 2026-10-30 起 | **Wi-Fi 侧一直失败**（`12289` / `rc=-5`）—— ⚠️ 此时段在位的具体是哪一颗，见 §8.1 的证据限制 | 同日 session 记录 |
| **2026-11-01 08:17–14:16** | ✅ **真正连上 AP**：`connected to "Hello_World_2.4G" rssi=-48…−50 ch=6` + `wlan0 address 192.168.1.9` —— **当时 C6 里是 `c6_hosted_new` 的 network-split 版**（§8.1） | `logs/20261101-135139_e2e/boot.txt:1284-1329`；git `46df328` |
| 2026-11-02 前 | network-split 版被判与 host 栈不兼容 → **回退**成 station 版 | `docs/30_实现/应用层_DeskMate_UI.md` §31 |
| 2026-11-04 17:03 | 按文档「刷回已知可用组合」= 再次烧 `c6_hosted/eh_cp_wifi_sta_esp32c6_sdio.bin` | `docs/30_实现/应用层_DeskMate_UI.md` §31；`docs/50_排障经验/08…md:129` |
| 2026-11-04 21:30 | `DM-WIFI-CONNECT ssid='Hello_World_2.4G' rc=-5`，`connect failed: 12289` | `logs/20261104-deliver-verify/FINDINGS.md:78-91` |
| 2026-11-04 22:11 | 本次取证读到的活体 | 本文件 |

**判定**：
1. 已确认 C6 的**分区在跑 ota_0**（`Loaded app from partition at offset 0x10000`），
   缓冲里**没有出现过 ota_1 加载**，`Set actual ota_seq=1 in otadata[0]` 只是 bootloader 每次写回同一 slot；
   **没有观察到任何 OTA 切换**。
2. 在**本缓冲覆盖的时间窗内**（约 250 s 内 14 次启动），**固件没有被换过**，
   也没有任何一次启动了别的工程名/版本。
3. **不能**仅凭本缓冲证明「10-26 至 11-04 之间从未烧过别的镜像」—— 缓冲只是最近约 7721 行；
   但从会话外证据看，唯一被实际写进去过的另一版是 `eh_cp_network_split_station.bin`
   （10-30 的 `1115248 B`），且它**不是**当前活体。

---

## 4. 失败指纹时间线（回答 lead 的 4 个问题）

### Q1. 指定串的全文检索结果（C6 `/dev/ttyUSB1`，7721 行）

| 检索串 | 是否出现 | 出现位置与上下文 |
|---|---|---|
| `WiFi init config changed, reinitializing` | **❌ 从未出现** | 但该串**存在于镜像里**（`strings` 命中），说明是**代码路径未被走到**，不是固件缺功能 |
| `not started` / `0x3002` / `12290` / `ESP_ERR_WIFI_NOT_STARTED` | **❌ 全部从未出现** | 注释里预测的 0x3002 在活体上**一次都没有成立** |
| `wifi_init` / `esp_wifi_init` | **❌ 日志中从未出现为日志行**（`ehcp_wifi: WiFi feature init ok` 是 **hosted 的 feat_wifi 扩展**初始化，不是 `esp_wifi_init`） | 从机从未调用 `esp_wifi_init` |
| `sta start` / `wifi start` / `STA start` | **❌ 从未出现** | 从机从未启动射频 |
| `connect` / `scan` | **⚠️ 出现在 `req_wifi_scan_start` 里**（主机请求），**没有**从机自己 scan/connect 的日志 | 见下 |
| `WIFI_NOT_INIT` / `12289` | **✅ 出现，且远不止一条** | 见 Q2 |

`req_wifi_scan_start` 相关原文（证明主机在**反复尝试扫描**，而从机一律拒绝）：
```
E (206536) ehcp_wifi_rpc: req_wifi_scan_start:1110 failed [esp_wifi_scan_start(p_a, req_payload->block)] = [12289]
E (206905) ehcp_wifi_rpc: req_wifi_scan_start:1110 failed [esp_wifi_scan_start(p_a, req_payload->block)] = [12289]
```

### Q2. `req_wifi_set_mode` 这条 E 行出现几次？时间戳间隔？

**不是 1 次，是反复出现。** 本缓冲里 `12289` 呈现为**有结构的错误簇**，
每次「主机驱动一轮 WiFi 操作」产生 4 条，簇内间隔 ~0.4 s，簇间间隔 ~6.5–7 s：

```
E (18757)  ehcp_wifi_rpc: req_wifi_set_mode:108       failed ... = [12289]     ← 开机后第一次
E (161408) ehcp_wifi_rpc: req_wifi_sta_get_ap_info:1422 failed ... = [12289]
E (161909) ehcp_wifi_rpc: req_wifi_set_mode:108       failed ... = [12289]
E (162315) ehcp_wifi_rpc: req_wifi_get_mode:93        failed ... = [12289]
E (162724) ehcp_wifi_rpc: req_wifi_scan_start:1110    failed ... = [12289]
E (163093) ehcp_wifi_rpc: req_wifi_scan_start:1110    failed ... = [12289]
E (169583) ... req_wifi_set_mode:108                  failed ... = [12289]
E (169926) ... req_wifi_get_mode:93                   failed ... = [12289]
E (170313) ... req_wifi_scan_start:1110               failed ... = [12289]
E (170693) ... req_wifi_scan_start:1110               failed ... = [12289]
E (177054) ... (同型 4 条)   E (184484) ... (同型 4 条)
E (191970) ... (同型 4 条)   E (199424) ... (同型 4 条)
E (207036) ... req_wifi_sta_get_ap_info:1422          failed ... = [12289]
```
（另有更早的一轮：`E (205254) get_ap_info` / `E (205754) set_mode` / `E (206103) get_mode` /
`E (206536)+E (206905) scan_start` / `E (213282)…` / `E (220670)…` / `E (228090)…` /
`E (235510)…` / `E (242970)…` / `E (250491) get_ap_info`。）

**含义（重要）**：这不是「主机重试同一个 set_mode」，而是
**主机每一轮 `SIOCSIWSCAN`/connect 尝试都完整走到从机，并被从机逐个拒绝**。
⇒ **SDIO 数据通路（RPC）在 250 s 尺度上是通的**，坏的是**公式里的射频归属**：
从机没 `esp_wifi_init`／没 `esp_wifi_start`，所以 `set_mode`/`get_mode`/`scan_start`/`sta_get_ap_info`
**全部**返回 `0x3001 ESP_ERR_WIFI_NOT_INIT`。

> 对 `nuttx/…/eh_hosted_nuttx_entry.c:337-373` 注释的判定：
> 注释假设「从机应用**已经自己 init 并 started 了电台**，所以主机故意不发 `Req_WifiInit`」。
> **活体日志反驳这个假设**：从机 `app_main` 直接返回，射频从未启动；
> 且从机侧实证的失败码是 **0x3001 NOT_INIT（不是注释预言的 0x3002 NOT_STARTED）**。
> 0x3001 恰好就是「没人调用过 `esp_wifi_init`」的语义
> —— 与「主机故意不发 WifiInit」**互为因果**。
>
> **⚠️ 调和结论（重要，避免后人误用）**：注释的前提**并非凭空捏造**——
> 它依据的观测（「从机已自己 init 并 started 电台」）**成立在 `c6_hosted_new` 的 network-split 版在位期间**
> （那颗固件才是自 init/自 start、才有 `ehcp_nw_split`/`feat_nw_split`/`eh_example_sta` tag，
> 也正是 11-01 真正连上 AP 的那颗，见 §8.1）。
> **换成现在这颗 `eh_cp_wifi_sta`（station 示例）之后，注释的前提就不再成立**，
> 而注释与其对应的「故意不发 `Req_WifiInit`」代码路径**没有被同步更新**，
> 这才是 `0x3001 ESP_ERR_WIFI_NOT_INIT` 的直接来源。
> ⇒ 请勿把「注释里的前提」当成「当前 station 版从机的行为」。

### Q3. 缓冲里 C6 被 host reset 过几次？每次 RPC 序列是否不同？

- **完整读到 `ESP_OPEN_DATA_PATH` 的应用周期：3 次**（见 §3.1），
  生效的 host reset 计数也可以用 C6 侧 `advertising transfer size 1536` 数出来：**3 次**。
- **应用级重启（ROM boot → `app_main` 返回）：约 14 次**，其中约 11 次发生在
  真正建立连接之前（主机在 `probe → reset → 再 probe` 的循环里），
  每次重启**都还没有**走到 `ESP_OPEN_DATA_PATH`。
- **每次重启后的 RPC 序列**：前两次只到 `advertising transfer size 1536` 即被下一次 reset 打断
  （**不完整**）；第三次才是完整的
  `Send slave up → advertising transfer size 1536 → slave init_config → Host capabilities: 44 →
   RawTP Stop/unchanged → flow-ctl 阈值 → RPC V2 → wire_hdr=V1/rpc=V1 → Set RPC endpoints →
   ESP init event (event_id=1 msg_id=0x0301 len=0) → event ESPInit → req_wifi_set_mode → 12289`。
  **⇒ 每次被 reset 后序列并不相同，且只有第三次的主机↔从机握手是完整的。**

### Q4. 三方离线比对

见 §2：**活体 == `c6_hosted/eh_cp_wifi_sta_esp32c6_sdio.bin`**，
判据为 ELF SHA256 前 9 位 `98147ff44` + 编译时间 `Oct 26 2026 15:25:32` + 5 段布局全中；
`debug.bin`（`017ed307e` / `16:24:45`）与 `network_split_station.bin`
（`9e718d166` / `Oct 28 20:29:15`）在 descriptor 与段布局上**逐项不符**。
**不是「活体不在磁盘上」。**

---

## 5. P4 主机侧同一时间窗的日志

`/dev/ttyUSB0`（sessionId `00c17097-68c2-44c8-bf66-a5331a92be5f`）

### 5.1 复位 C6 的那一行（关键）

```
esp_sdmmc: ISR error raw=00000100 [re=0 rcrc=0 dcrc=0 rto=1 drto=0 hto=0 frun=0 hle=0 sbe=0 ebe=0]
esp_hosted_sdio: probe 1/4 -> -5, retrying
esp_hosted_sdio: companion enumerated (sdio_probe returned 0)
esp_hosted_sdio: CCCR 0x04 interrupt enable 02 -> 03 (ret 0)
esp_hosted_sdio: card bus width requested 4 bit (CCCR 0x07 was 42, ret 0)
esp_hosted_sdio: host-ready mask 01 written (ret 0), readback 00 (ret 0)
esp_hosted_sdio: leaving the card clock at the identification rate for the official stack
esp_sdmmc: ISR error raw=00000104 [re=0 rcrc=0 dcrc=0 rto=1 drto=0 hto=0 frun=0 hle=0 sbe=0 ebe=0]
esp_hosted_sdio: controller reset + clock reprogrammed, now resetting the companion (500 ms low, then 10 s to come up)
```
判读：SDIO 枚举**是成功的**（`sdio_probe returned 0`、CCCR 中断使能、4-bit 总线、host-ready mask 都写成功），
但随后控制器被复位、**C6 被拉复位脚**（`500 ms low, then 10 s to come up`），
对应 C6 侧那一串 `rst:0x1 (POWERON)`。
`rto=1`（response timeout）是唯一的错误位。

### 5.2 `no card interrupt ... (#N)` 的计数增长（尾部窗口）

```
eh_host_port_sdio: no card interrupt within 2000 ms, polling the companion (#832)
   …64 次 [dm] heartbeat …
eh_host_port_sdio: no card interrupt within 2000 ms, polling the companion (#896)
   …64 次 [dm] heartbeat …
eh_host_port_sdio: no card interrupt within 2000 ms, polling the companion (#960)
   …64 次 [dm] heartbeat …
eh_host_port_sdio: no card interrupt within 2000 ms, polling the companion (#1024)
   …64 次 [dm] heartbeat …
eh_host_port_sdio: no card interrupt within 2000 ms, polling the companion (#1088)
   …64 次 [dm] heartbeat …
eh_host_port_sdio: no card interrupt within 2000 ms, polling the companion (#1152)
   …64 次 [dm] heartbeat …
eh_host_port_sdio: no card interrupt within 2000 ms, polling the companion (#1216)
   …64 次 [dm] heartbeat …
eh_host_port_sdio: no card interrupt within 2000 ms, polling the companion (#1280)
```
- 计数**严格每 64 递增一次**（832 → 1280，本次读到的最新值 1280），
  说明是**固定周期的轮询节拍**（每个 # 之间夹 64 行 `[dm] heartbeat`）。
- `[dm] heartbeat: loops=9x cam_page_calls=0` 一直在跑 ⇒ **P4 应用层活着**，不是死机。
- 判读：主机在等从机的 SDIO 卡中断，从机**没有按时拉中断**，主机退化为轮询；
  这正是上面 `no card interrupt` 与 `esp_hosted` 复位循环的直接体现。

### 5.3 这一段里**没有**出现的东西

- **没有** `eh_hosted_netdev` / `connect failed` / `rc=-5` / `ESP_ERR_WIFI` 这一类行出现在这个尾部窗口
  （那些在更早的时间点，见 `logs/20261104-deliver-verify/FINDINGS.md`）。
  即：**当前这一刻 C6 侧的 WiFi RPC 根本还没走到** —— 主机还在「等中断/复位从机」的阶段。
- **没有** `dsm`/`NuttX` 崩溃栈，没有看门狗复位。

---

## 6. 对用户三问的直接回答

1. **「之前和 esp p4 通讯上过的 WiFi 固件是 c6 中哪一种固件？」**
   → **要分两颗答（本节已按 lead 更正）**：
   - **11-01 真正连上 AP 的那颗 = `c6_hosted_new/eh_cp_network_split_station.bin`**
     （纯网络适配器版，`App version: 1`，`Oct 28 2026 20:29:15`，ELF `9e718d166…`）。
     证据：`logs/20261101-135139_e2e/boot.txt:1284-1329` 的
     `connected to "Hello_World_2.4G" rssi=-48 ch=6` + `wlan0 address 192.168.1.9`；
     以及队友的 tag 计数（`wifi_mcu_example` × **0**、`ehcp_nw_split` ×56，见 §8.1）。
   - **现在这颗 = `eh_cp_wifi_sta`**（`v3.0.10-dirty`，`Oct 26 2026 15:25:32`，ELF `98147ff4…`），
     即 `tools/build_flash/c6_hosted/eh_cp_wifi_sta_esp32c6_sdio.bin`
     —— 文档称的「WiFi station 示例 / 已知可用组合」。
     仓库记录里它**只被证实到「SDIO 枚举 + RPC 传输通」**，
     **没有任何一条记录证明它自己成功连上过 AP**。
   ⇒ **"之前通讯上过的那颗"≠"现在这颗"**：前者是 network-split 版，后者是 station 版。
2. **「和现在在运行的 c6（tabby 里的 usb1）读出来的是一样的吗？」**
   → **一样，且是逐字节级同一个镜像**（ELF SHA256 + 编译时间 + 5 段布局三项独立指纹全中）。
   不是 debug 版，不是 `c6_hosted_new` 的网络适配器版。
   收窄后的完整表述：**活体 == `c6_hosted/eh_cp_wifi_sta_esp32c6_sdio.bin`，但它 ≠ 11-01 真正跑通时那颗
   `c6_hosted_new/eh_cp_network_split_station.bin`**。
   ⇒ 所以「C6 固件没有被换错」成立，但「C6 固件不是变量」只在「相对于 11-04 17:03 那次刷回」的意义上成立；
   **相对于 11-01 那个能用的状态，固件确实被换过（换成了站模式版）。**
3. **「为什么现在 p4 没法通过 c6 连上 WiFi？」**
   → 两层原因，缺一不可（详细根因归队友）：
   - **配对层（当前直接原因）**：从机 `app_main` 立即返回，**射频从未 `esp_wifi_init`/`esp_wifi_start`**；
     主机遵循 `eh_hosted_nuttx_entry.c:337-373` 的注释**故意不发 `Req_WifiInit`**、**故意不调 `eh_host_wifi_start`**，
     于是 `set_mode` / `get_mode` / `scan_start` / `sta_get_ap_info` **全部**返回
     `12289 = 0x3001 = ESP_ERR_WIFI_NOT_INIT`（实证）。主机侧的 `SIOCSIWESSID` 因此 `rc=-5`。
     **注释里「从机已自己启动电台」的前提在这颗镜像上不成立**
     —— 该前提属于 network-split 版在位期间（§4 调和结论），换固件后代码路径没同步更新。
     这是可以直接改代码修掉的。
   - **传输层（并发症状）**：主机反复 `no card interrupt within 2000 ms` 并
     `resetting the companion (500 ms low, then 10 s to come up)`，
     导致 C6 在 250 s 内被重启约 14 次，只有 3 次走到握手、只有 1 次走到 WiFi RPC。
     即使配对层修好，这一层也会继续拖垮它。
   - 另外：该镜像 `strings` 里**没有** `Hello_World` / `851285812`
     ⇒ **即使射频被正确 init，"谁来连 AP" 仍未定**（从机不持凭据；要么从机自带 SSID，
     要么换回纯网络适配器固件由主机连 —— 后者虽是 11-01 能跑通的那条路，
     但 `docs/30_实现/应用层_DeskMate_UI.md` §31 记录它与**当时的** host 侧改动不兼容，
     需要连同 host 栈一起回到 11-01 的配对状态才能真正复原）。

---

## 7. 未决点 / 置信度 / 需要什么才能补全

**置信度**
- 「活体 == `c6_hosted/eh_cp_wifi_sta_esp32c6_sdio.bin`」：**≈99%**（三类独立指纹全中，无任何反证）。
- 「从机从未 `esp_wifi_init`/`start`；12289 = 0x3001 = `ESP_ERR_WIFI_NOT_INIT`」：**≈99%**
  （活体日志 + 镜像 `strings` + IDF 头文件定义三方一致）。
- 「本缓冲窗口内固件没换过、无 OTA 切换」：**≈97%**（14 次启动 descriptor 全同；仅受限于缓冲只有 7721 行）。
- 「**11-01 真正跑通时在位的是 network-split 版**」：**≈97%**
  （队友 tag 计数 `wifi_mcu_example` ×0 为直接反证；我方 `strings` 归属复核独立成立；见 §8.1）。

**未决点**
1. **缓冲起点之前的 C6 历史不可见**。要 100% 证明「10-26 至今没烧过第三版镜像」，
   需要 flash 侧的物理证据（例如 esptool 写记录的完整归档、或 C6 的 `otadata`/分区 dump —— 都需连串口，本轮**禁止**）。
2. **`rst:0x1 (POWERON)` 的原始原因**未从 C6 侧独立确认（本轮只能从 P4 侧
   `controller reset + clock reprogrammed, now resetting the companion` 反推是主机拉的复位）。
3. **「no card interrupt」的首个 N** 未取到（本次只读了 P4 缓冲的 `0-120` 与 `4965-5165`，
   中间 4845 行未逐行读）。取首值需要再读一段 P4 缓冲。
4. **12289 错误簇的精确总条数**未做穷举（5030-7721 段未逐行回显；
   已确认的簇见 §4 Q2）。若需要精确计数，需再拉该段并本地 grep。
5. ~~「之前通讯上过」的原始出处未能定位；仓库里能找到的 Wi-Fi 实测记录全是失败。~~
   **→ 已解决（2026-11-04 22:2x 更正）**：出处是 **2026-11-01 08:17–14:16** 的会话，
   记录在 `logs/20261101-081709_connect-run/boot.txt:503`、
   `logs/20261101-083220_ip-apply/boot.txt:511`、
   `logs/20261101-134028_ip-applied/boot.txt`、`logs/20261101-135139_e2e/boot.txt:1284-1329`，
   会话外锚点 git `46df328`。当时在位的是 network-split 版（§8.1）。
6. **「2026-10-30 那次烧录之后、到 11-01 之前，C6 里到底是哪一颗」**未逐条坐实。
   已知 10-30 的 session 里**两版都烧过**（`:7775` station 1094464 B、`:19254` split 1115248 B），
   但中间还夹着多次切换；要精确到"哪一刻是哪一颗"，需要按时间线重放
   `logs/2026-11-02_session-raw.jsonl.zstd`（覆盖 10-31 05:36 → 11-01 14:16）的 tag 计数。
   本文件 §0/§3.2 已把不确定处显式标注，未把推断写成实测。

**本轮遵守的约束（自证）**
- 全程未打开/写入/复位 `/dev/ttyUSB0`、`/dev/ttyUSB1`、`/dev/ttyACM0`；
  未执行任何 esptool 串口操作；未烧录。
- 设备输出仅经 `mcp__tabby__get_terminal_buffer` 获取。
- 除本文件外未修改仓库任何文件。

---

## 8. 附录：更正记录与追加证据（2026-11-04 22:2x）

> 本节由 `device-forensics` 依 lead 的指正追加，并附独立复核结论。
> **受影响章节：§0（重写）、§3.2（表格补行）、§4 Q2 注释判定（追加调和结论）、§6 第 1/2/3 问（更正）、§7（未决点 5 关闭）。**

### 8.1 追加证据：11-01 成功窗口里 C6 跑的是 network-split 版

**（a）队友 `firmware-historian` 的计数结果**（对 `logs/2026-11-02_session-raw.jsonl.zstd`，
覆盖 10-31 05:36 → 11-01 14:16 的会话原文）：

| 字符串 | 出现次数 |
|---|---|
| `ehcp_nw_split` | 56 |
| `feat_nw_split` | 135 |
| `eh_example_sta` | 42 |
| **`wifi_mcu_example`** | **0** |

**（b）我方独立复核（`strings -a` + `grep -F`，两个候选镜像逐项）**：

| tag | `c6_hosted/eh_cp_wifi_sta_esp32c6_sdio.bin` | `c6_hosted_new/eh_cp_network_split_station.bin` |
|---|---|---|
| `wifi_mcu_example` | **PRESENT** | absent |
| `ehcp_nw_split` | absent | **PRESENT** |
| `feat_nw_split` | absent | **PRESENT** |
| `eh_example_sta` | absent | **PRESENT** |

两条独立路径互相印证，且 `wifi_mcu_example` 是 station 版**独有**的 app tag
（本文件 §1 的活体日志每轮启动都打印它）。
⇒ 成功窗口里 `wifi_mcu_example` 计数为 **0**，即**当时跑的绝不是现在这颗 station 版**，
而是只含 `ehcp_nw_split`/`feat_nw_split`/`eh_example_sta` 的 **network-split 版**。

**（c）因此第二问的答案必须收窄为**：

> **现在活体 == `c6_hosted/eh_cp_wifi_sta_esp32c6_sdio.bin`（此判定不变，指纹很硬）；
> 但它 ≠ 11-01 真正跑通时那颗 `c6_hosted_new/eh_cp_network_split_station.bin`。**

即「是同一个镜像吗」这个问题的答案是**分层的**：
- 相对于**11-04 17:03 那次刷回**：**是同一个**（活的 == 刷进去的 == 文档点名的那颗）。
- 相对于**11-01 真正连上 AP 的状态**：**不是同一个**（11-01 是 network-split 版）。

### 8.2 口径统一：`12289` 是 `ESP_ERR_WIFI_NOT_INIT`，不是 `ESP_ERR_WIFI_STATE`

- `12289 = 0x3001`。IDF 头文件（`nuttx/arch/risc-v/src/esp32p4/esp_hosted/official/port/idf_components/
  injected_idf_v6.1/include/injected/esp_wifi.h:63-66`）定义为：
  - `ESP_ERR_WIFI_NOT_INIT    = ESP_ERR_WIFI_BASE + 1 = 0x3001` ← **本案例的码**
  - `ESP_ERR_WIFI_NOT_STARTED = ESP_ERR_WIFI_BASE + 2 = 0x3002`
  - `ESP_ERR_WIFI_NOT_STOPPED = ESP_ERR_WIFI_BASE + 3 = 0x3003`
- **`ESP_ERR_WIFI_STATE` 是另一个码**（语义为"Wi-Fi 当前状态不允许该操作"），
  **不是** `0x3001`。
- 经全文核查：**§0–§7 从未把 `12289` 标为 `ESP_ERR_WIFI_STATE`**
  （在追加本节之前，全文 `grep -c "ESP_ERR_WIFI_STATE"` = **0**；
  §0/§4/§6 自始即写作 `ESP_ERR_WIFI_NOT_INIT`。现全文出现的 6 次
  **全部来自本节的口径统一说明**，均为"否定式"表述，无一处把 `12289` 断言为该码）。
  此条口径以本节为准，供交叉引用。
- ⚠️ **需要下游注意**：`logs/20261104-deliver-verify/FINDINGS.md`（队友文档）在正文里把
  `12289` 解释为 `ESP_ERR_WIFI_STATE`，**那一处是误标**，建议一并更正为
  `ESP_ERR_WIFI_NOT_INIT (0x3001)` —— 该文件不在我的写范围内，仅在此标注。

### 8.3 调和结论：host 注释的前提属于 network-split 时代

`eh_hosted_nuttx_entry.c:337-349` 声称「从机应用**已经自己 init 并 started 了电台**，
所以主机故意不发 `Req_WifiInit`」。活体日志显示**现在这颗 station 版**从机 `app_main` 立即返回、
从不 `esp_wifi_init`。两者并不矛盾，而是**时代错位**：

- 该注释所依据的观测发生在 **`c6_hosted_new` network-split 版在位期间**
  （那颗才自 init/自 start，也正是 11-01 连上 AP 的那颗）。
- 换成 station 版之后，前提失效，但**注释与「故意不发 `Req_WifiInit`」的代码路径没有同步更新**
  ⇒ 直接产生 `0x3001 ESP_ERR_WIFI_NOT_INIT`。

**给后人的一句话**：读到 `eh_hosted_nuttx_entry.c:337-373` 时，
请把「从机已自己启动电台」当作**历史前提**，而不是**当前 station 版的实测行为**。
