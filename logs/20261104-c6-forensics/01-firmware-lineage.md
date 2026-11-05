# C6 固件谱系与烧录时间线（固件考古）

> 调查者：firmware-historian（shared task `task-1`）
> 时间：2026-11-04 22:1x（Asia/Shanghai）
> 方法：**纯离线**。仅读磁盘镜像、git、docs/、logs/，敏感区串口只用 `mcp__tabby__get_terminal_buffer` 读缓冲。
> **未打开 / 未写入任何 `/dev/ttyUSB*`、`/dev/ttyACM*`；未跑 esptool 连串口；未烧录。**
> 回答的是用户三问中的**第一问**（之前通讯上过的是哪一版 C6 固件），并为第二问提供比对基线。

---

## 0. 结论摘要（TL;DR）

| 问题 | 结论 | 置信度 |
|---|---|---|
| 现在 C6（ttyUSB1）里跑的是哪一版？ | `tools/build_flash/c6_hosted/eh_cp_wifi_sta_esp32c6_sdio.bin`（**逐字节同级**：项目名 + 编译时刻 + ELF SHA256 + bootloader 编译时刻 + 分区表全部对上） | **极高（直接证据，4 条独立指纹互锁）** |
| 之前和 P4 真正通讯成功过的是哪一版？ | **同一颗文件 `c6_hosted/eh_cp_wifi_sta_esp32c6_sdio.bin`**（记录在册的烧录→通讯成功链条完整） | **高** |
| 和现在读出来的一样吗？ | **一样**。用户问题的第二问：**是同一种固件**，不是两种。 | **极高** |
| 那为什么现在连不上 WiFi？ | 不是"固件种类变了"，而是这颗固件是 **wifi_mcu_example（Station 示例，射频自持、无 AP 凭据）** ⇒ 主机任何 `set_mode/scan/connect` 都被从机以 `12289 = ESP_ERR_WIFI_STATE` 拒绝。**这与 2026-11-01 那次"成功"是两件事**：11-01 拿到的 IP 是 %%host 侧声明 network-split 能力%% + **另一颗**从机固件（Oct 28 的 `eh_cp_network_split_station`）时代的事，且那条路径当晚已被回退。 | **高** |

一句话：**C6 的固件从头到尾就是我们自己构建的 `eh_cp_wifi_sta`（Oct 26 15:25:32 那颗）。现在跑的就是它，和当初通讯成功时是同一颗。连不上 WiFi 的原因不在"固件换过"，而在这颗固件"谁拥有射频"的定位与主机栈的期望不匹配。**

---

## 1. 磁盘镜像定性（离线取证）

### 1.1 逐镜像指纹表

用 `python3 -m esptool image_info <bin>`（**纯离线，不接串口**）+ 解析 `esp_app_desc_t`（image 偏移 0x20）+ `strings`：

| 镜像 | 大小(B) | 项目名 | 版本 | 编译时刻 | IDF | 内嵌 ELF SHA256 | 盘上文件 SHA256(前16) |
|---|---|---|---|---|---|---|---|
| `tools/build_flash/c6_hosted/eh_cp_wifi_sta_esp32c6_sdio.bin` | **1,094,464** | `eh_cp_wifi_sta` | `v3.0.10-dirty` | **Oct 26 2026 15:25:32** | `3ea97e75` | **`98147ff449618e914d2120b73bc8f5faeafb9b437135fb866f87c6bdee2b43f3`** | `bd1b042fe7aff8dd4a48ef571f7e366e` |
| `tools/build_flash/c6_hosted/eh_cp_wifi_sta_debug.bin` | 1,131,952 | `eh_cp_wifi_sta` | `v3.0.10-dirty` | **Oct 26 2026 16:24:45** | `3ea97e75` | `017ed307e999f5b004f4d190b3defca6ba702cfcf1c955804ab8bc67057529e3` | `c8e88ce5ff78f7e74cba780ca86f2884` |
| `tools/build_flash/c6_hosted_new/eh_cp_network_split_station.bin` | 1,115,248 | `eh_cp_network_split_station` | `1` | **Oct 28 2026 20:29:15** | `3ea97e75` | `9e718d166c9da998eb32795c1f98b10acc30445f4e60555b8776714292b3553c` | `ee0097887874716641e3ae3f9d501875` |
| `tools/test/c6_sdio_minimal/prebuilt/c6sdiotest.bin` | 175,392 | `c6sdiotest` | `1` | **Oct 29 2026 14:54:41** | `3ea97e75` | `3987e457a91fb22dffea8073e15468ee6297d445108c8497f67b90ef588d1d33` | `f99e6147c37888567a444feecaf59759` |

bootloader 编译时刻（`strings`，用于与设备启动日志比对）：

| bootloader | 编译时刻 |
|---|---|
| `tools/build_flash/c6_hosted/bootloader.bin` | **`Oct 26 2026 16:24:06`** |
| `tools/build_flash/c6_hosted_new/bootloader.bin` | `Oct 28 2026 20:28:17` |
| `tools/test/c6_sdio_minimal/prebuilt/bootloader.bin` | `Oct 29 2026 14:54:05` |

> 注：`tools/build_flash/**` 下所有文件的 mtime 都是 `2026-11-04 18:05:56`（= 交付树检出/整理时刻），**不能用 mtime 当烧录时间**。唯一可靠的时间刻度是**镜像内嵌的编译时刻**和**日志里的真实事件**。

### 1.2 三条产品线的区分（用什么判据）

这四颗镜像属于 esp-hosted v3.0.10 下**互不相同的三条线**，可用镜像内字符串唯一区分（`strings` 计数）：

| 标记字符串 | `eh_cp_wifi_sta_*.bin` | `eh_cp_network_split_station.bin` | `c6sdiotest.bin` | 含义 |
|---|---|---|---|---|
| `ehcp_nw_split` | **0** | 1 | 0 | 从机 network-split 模块 |
| `nw_split_sta_cp` | **0** | 1 | 0 | network-split station 示例应用 |
| `feat_nw_split` | **0** | 8 | 0 | 能力描述符 |
| `wifi_mcu_example` | 1 | **0** | 0 | esp-hosted **wifi_sta 示例**的 app tag |
| `ehcp_wifi` | 3 | 3 | 0 | WiFi RPC 特性 |
| `ESP_OPEN_DATA_PATH` | 1 | 1 | 0 | 数据面开关事件 |

⇒ **`c6_hosted/` 这套是 esp-hosted-mcu `examples/wifi/sta/cp` 编出来的「Station 示例」；`c6_hosted_new/` 那套是 `examples/network_split/station/cp` 编出来的「network-split station 示例」。两者是不同示例工程，不是同工程的两个配置。** `c6sdiotest` 是最小 SDIO 测试件（无 esp-hosted 栈，用于量 SDIO 链路本身）。

**镜像内的源码路径也相互印证**：`eh_cp_wifi_sta_*.bin` 内含
`//tmp/eh_components/esp_hosted/coprocessor/features/eh_cp_feat_rpc_ext_v2/src/eh_cp_feat_rpc_ext_v2.c`
⇒ 都来自同一份 `esp_hosted/coprocessor/` 源码树（v3.0.10）。

### 1.3 传输配置（`sdkconfig.esp32c6.sdio` 直接读，非推断）

`tools/build_flash/c6_hosted/sdkconfig.esp32c6.sdio`：

| 行 | 配置 | 值 |
|---|---|---|
| 428 | `CONFIG_IDF_TARGET` | `"esp32c6"` |
| 634 | `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` | `"partitions_eh_cp_ota_4m.csv"` |
| 2266-2268 | `CONFIG_ESP_HOSTED=y` / `CP=y` | 从机角色 |
| 2346-2347 | `CONFIG_EH_TRANSPORT_CP_SDIO=y`（SPI/UART 均 not set） | **SDIO 传输** |
| 2357 | `CONFIG_EH_TRANSPORT_CP_SDIO_MODE_SW_AGGR=y` | 数据面 SW_AGGR |
| 2359 | `CONFIG_EH_TRANSPORT_CP_SDIO_GPIO_RESET=-1` | 从机不自管复位脚（本板正确值，见 `docs/30_实现/连接_WiFi_SDIO.md:1220-1243`）|
| 2366-2371 | `PIN_CMD=18 CLK=19 D0=20 D1=21 D2=22 D3=23` | 从机 SDIO 引脚 |
| 2374 | `CONFIG_EH_TRANSPORT_CP_SDIO_PSEND_PSAMPLE=y` | 发送/采样均正沿 |
| **2423** | **`# CONFIG_ESP_HOSTED_CP_FEAT_NW_SPLIT is not set`** | **这颗固件不带 nw_split 模块** |
| 2398-2416 | `CP_RPC_V2=y` / `FEAT_WIFI=y` / `FEAT_WIFI_EXT_ITWT=y` / `FEAT_SYSTEM=y` | 具备 RPC + WiFi 特性 |

分区表（解析三个 `partition-table.bin`）：

| 镜像的分区表 | 布局 | 与设备实测是否一致 |
|---|---|---|
| `c6_hosted/partition-table.bin` | nvs@0x9000(0x4000) / otadata@0xd000 / phy_init@0xf000 / **ota_0@0x10000(0x1C0000)** / ota_1@0x1d0000 | ✅ 逐项一致 |
| `c6_hosted_new/partition-table.bin` | **与上面 md5 完全相同**（`f2b3d62c…`） | ✅ 逐项一致 |
| `c6_sdio_minimal/prebuilt/partition-table.bin` | nvs@0x9000(0x6000) / phy_init@0xf000 / **factory@0x10000(0x100000)**（无 OTA） | ❌ 不一致 |

---

## 2. 「现在读出来的」到底是什么：与活体自报的指纹比对

**判据（4 条独立指纹，全部命中同一颗文件）：**

| # | 指纹 | 活体 C6（`ttyUSB1`, sessionId `12930a8c-…`）自报 | 盘上 `c6_hosted/eh_cp_wifi_sta_esp32c6_sdio.bin` | 命中 |
|---|---|---|---|---|
| 1 | 项目名 | `app_init: Project name:     eh_cp_wifi_sta` | `eh_cp_wifi_sta` | ✅ |
| 2 | 编译时刻 | `app_init: Compile time:     Oct 26 2026 15:25:32` | `Oct 26 2026 15:25:32` | ✅ |
| 3 | **内嵌 ELF SHA256** | `app_init: ELF file SHA256:  98147ff44...` | `98147ff449618e914d2120b73bc8f5faeafb9b437135fb866f87c6bdee2b43f3` | ✅ **前 9 位精确匹配** |
| 4 | bootloader 编译时刻 | `I (24) boot: compile time Oct 26 2026 16:24:06` | `c6_hosted/bootloader.bin` 内嵌 `Oct 26 2026 16:24:06` | ✅ 精确匹配 |
| 5（旁证）| 分区表 | `ota_0 OTA app 00 10 00010000 001c0000` / `ota_1 001d0000 001c0000` | `c6_hosted` 与 `c6_hosted_new` 的分区表（两者 md5 相同） | ✅ |

**排他性**：
- 不是 DEBUG 版 —— 其编译时刻是 `Oct 26 2026 16:24:45`，与设备自报的 `15:25:32` 不符；
- 不是 network-split 版 —— 其项目名是 `eh_cp_network_split_station`、编译时刻 `Oct 28 2026 20:29:15`；
- 更决定性的是：**`eh_cp_wifi_sta_*.bin` 里 `ehcp_nw_split` / `nw_split_sta_cp` / `feat_nw_split` 计数为 0**，而活体 C6 加载的特性描述符恰好只有
  `rpc_mcu / feat_system / feat_wifi / ext_rpc` **4 个，没有 `feat_nw_split`**，并打印
  `wifi_mcu_example: ESP-Hosted MCU WiFi Example Starting...`。

**活体原文（直接证据，来自 `mcp__tabby__get_terminal_buffer`，非串口直连）**：

```
I (23) boot: ESP-IDF 3ea97e75 2nd stage bootloader
I (24) boot: compile time Oct 26 2026 16:24:06
I (323) app_init: Project name:     eh_cp_wifi_sta
I (328) app_init: App version:      v3.0.10-dirty
I (332) app_init: Compile time:     Oct 26 2026 15:25:32
I (337) app_init: ELF file SHA256:  98147ff44...
I (341) app_init: ESP-IDF:          3ea97e75
I (441) ehcp_core:                 ESP-Hosted Firmware version :: EH-3.0.10
I (448) ehcp_core:                 Transport used :: SDIO only
I (485) ehcp_core: auto_feat_init_task: found 4 extension descriptor(s)
I (491) ehcp_core:   desc[0] name='rpc_mcu'    prio=60
I (499) ehcp_core:   desc[1] name='feat_system' prio=120
I (507) ehcp_core:   desc[2] name='feat_wifi'   prio=200
I (515) ehcp_core:   desc[3] name='ext_rpc'     prio=5
I (644) wifi_mcu_example: ESP-Hosted MCU WiFi Example Starting...
```

> ⚠️ 同一条 Tabby 缓冲里也含**历史**轮次（`四、五、六、八、十一` 号 Tabby session）的旧滚动内容，**不要只看行首**。判定依据是"**最后一次冷启动**（`rst:0x1 (POWERON)`）之后的那段"，其上仍是最新的 station 固件；且最新一轮里主机发来的
> `Host capabilities: 44` / `RPC version negotiated: V2` 说明**最新那次启动确实与 P4 完成了 SDIO 数据面对接**。

⇒ **结论（极高置信度）**：**现在 C6 里的，就是 `tools/build_flash/c6_hosted/eh_cp_wifi_sta_esp32c6_sdio.bin`，逐字节同级。** 附带地，其分区布局与 `partitions_eh_cp_ota_4m.csv`（OTA 双槽、app@0x10000）一致，**不是** `c6_sdio_minimal` 那条线。

---

## 3. 烧录时间线（重建）

**时间刻度说明**：星号 `*` 的时间来自 `logs/*_session-raw.jsonl.zstd` 里每条事件自带的 epoch 毫秒（utc+8 换算），**不是**我推测的。

| # | 时间 | C6 里的固件 | 事件 / 依据 | 当时实测结果 |
|---|---|---|---|---|
| 0 | ≤ 2026-10-26 15:45 | **出厂镜像（身份未定）** | 出厂镜像走**串口**传输的 esp-hosted：每 5 秒打印 `stats: slave StaRxQLoad: 0` / `serial : in: 0 rsp: 0 evt: 0`。`docs/30_实现/连接_WiFi_SDIO.md:815-822`（附录 F.2）| P4 探测 `-110`；**从未上过 SDIO** |
| 1 | **2026-10-26 15:45:35**（±2 min）| `eh_cp_wifi_sta_esp32c6_sdio.bin`（INFO，1,094,464 B） | 首次刷入 4 镜像布局：`Wrote 1094464 bytes (628034 compressed) at 0x00010000 in 15.6 seconds / Hash of data verified`。`docs/30_实现/连接_WiFi_SDIO.md:805-812`（附录 F.1）；会话时间锚 `logs/2026-10-30_wifi-sdio-session.md:7775`（紧跟在 `10-26 15:45:35` 的助手消息后）| P4 仍 `-110`；原因是 C6 卡在 ROM 下载模式（`boot:0x4 DOWNLOAD`），`连接_WiFi_SDIO.md:880-931`（F.7）|
| 2 | **2026-10-26 16:20:49** | 同上 INFO 版 | **读回自报**：`app_init: Project name: eh_cp_wifi_sta` + `Compile time: Oct 26 2026 15:25:32` + **`ELF file SHA256: 98147ff…`**。`logs/2026-10-30_session-raw.jsonl.zstd`（epoch 1793002849）| 这次读到的是**盘上同一颗**（指纹三条全中）|
| 3 | **2026-10-26 16:36:48**（±2 min）| `eh_cp_wifi_sta_debug.bin`（DEBUG，1,131,952 B）| `Wrote 1131952 bytes (640142 compressed) at 0x00010000 in 16.0 seconds` + 独立 `esptool verify_flash` 读回 `-- verify OK (digest matched)`。`连接_WiFi_SDIO.md:1276-1294`（H.11）；时间锚 `logs/2026-10-30_wifi-sdio-session.md:8833`（`10-26 16:36:48` 之后）| C6 又进下载模式（BOOT 被按住），`连接_WiFi_SDIO.md:1296-1309`（H.12）|
| 4 | **2026-10-28 21:19:55**（±2 min）| `eh_cp_network_split_station.bin`（1,115,248 B，Oct 28 20:29:15）| `Wrote 1115248 bytes (638024 compressed) at 0x00010000 in 16.0 seconds`。`logs/2026-10-30_wifi-sdio-session.md:19254`（时间锚 `10-28 21:19:55`）| P4 `probe 1/4..4/4 -> -110`，与之前**完全相同** ⇒ 当时判定"排除了 C6 固件内容这个方向"。`logs/2026-10-30_wifi-sdio-session.md:19266-19295` |
| 5 | **2026-10-28 21:50:52**（±2 min）| **刷回** `eh_cp_wifi_sta_esp32c6_sdio.bin`（INFO）| `Wrote 1094464 bytes (628034 compressed) at 0x00010000 in 15.6 seconds` + `Hash of data verified`。`连接_WiFi_SDIO.md:4859-4871`（附录 **S.28** 标题即"刷回交付原始固件"）；时间锚 `logs/2026-10-30_wifi-sdio-session.md:20172` | 仍 `-110`。但**正是这次回退，把 `eh_cp_wifi_sta` 装回了 C6，并一直留在里面** |
| 6 | **2026-10-30 10:58:31–11:05:59** | 仍在跑 **network-split 版** | 读 C6 控制台：`app_init: Project name: eh_cp_network_split_station` / `App version: 1` / `Compile time: Oct 28 2026 20:29:15` / `ELF file SHA256: 9e718d166...`；并打印 `feat_nw_split`、`nw_split_sta_cp: ESP-Hosted network_split station CP starting…`。`logs/2026-10-30_wifi-sdio-session.md:32405-32412`、`:32442`、`:32479`、`:33050` | **与 #5 冲突 ⇒ 见 §5「不确定性 U-2」** |
| 7 | **2026-10-30 11:35** | 同上（或短暂被 IDF 官方 P4 固件覆盖过 P4 侧）| 横向对比：烧 IDF 官方 P4+C6 WiFi 固件，主机侧 `esp_wifi_init -> ESP_OK` / `*** WIFI INIT OK - link works ***` / `esp_wifi_start -> ESP_OK`。`logs/2026-10-30_wifi-sdio-session.md:34514-34530` | **首次证明 C6 的 SDIO 从机能与"官方主机栈"完整对接** |
| 8 | **2026-10-31 05:36 – 11-01 14:16** | **network-split 版在跑** | raw session 里 `ehcp_nw_split` 共 56 次，`feat_nw_split` 共 135 次，时间跨度 `10-31 05:36:26 → 11-01 14:16:20`；`eh_example_sta` 42 次；**同期 `wifi_mcu_example` 计数 = 0**（=`eh_cp_wifi_sta` 的应用 tag 完全没出现）。`logs/2026-11-02_session-raw.jsonl.zstd` | 这一段正是 P4↔C6 **大规模通讯成功**的窗口 |
| 9 | **2026-11-01 06:56:53** | 同上 | git `12b15f9`：`feat(wifi): **scan 成功拿到真实 AP 列表**（验收④ scan 达成，25 个 AP）` | 25 个 AP |
| 10 | **2026-11-01 08:22:07** | 同上 | git `a26b1bb`：`feat(wifi): **验收④收尾——connect + DHCP 取 IP 真机达成**` | connect + DHCP |
| 11 | **2026-11-01 13:20:21** | 同上 | git `1e473ab`：`根因=主机未声明 network-split 能力（两侧同缺）`；`d6cf44d`（13:28）量出 18 个 `esp_netif` 函数 | 定位到**主机侧**缺 feature |
| 12 | **2026-11-01 14:10:40** | 同上 | git `46df328`：`**验收④闭环——wlan0 拿到地址 192.168.1.9/24 gw 192.168.1.1**`；配套日志 `logs/20261101-133103_nwsplit/`、`logs/20261101-134028_ip-applied/`、`logs/20261101-135139_e2e/`、`logs/20261101-140118_e2e2/` | **P4↔C6 通讯的顶点**：`wlan0 address 192.168.1.9 mask 255.255.255.0 gw 192.168.1.1` ×3 |
| 13 | **2026-11-04 ~16:13–17:06** | **先刷成 network-split 版，再刷回 station 版** | git `e4b4e8f`(16:13) 提议刷 `c6_hosted_new`；git `f89f1d0`(17:03) 实测：烧 `c6_hosted_new` ✅ hash verified → **P4 卡在 MCUboot 之后**（2061 B / 2084 B，无 NSH）⇒ 判定"新固件与 host 栈不匹配"；git `246bff2`(17:06) `处置：C6 刷回 c6_hosted/eh_cp_wifi_sta_esp32c6_sdio.bin`。同文另记于 `docs/30_实现/应用层_DeskMate_UI.md:2152-2177`（§31）、`docs/50_排障经验/08_驱动层调优与多副本陷阱.md:110-133` | 网络适配器方向**失败并回退** |
| 14 | **2026-11-04 21:30** | **`eh_cp_wifi_sta`（INFO）** | 连接实测：`deskmate --wifi-connect Hello_World_2.4G 851285812` ⇒ `eh_hosted_netdev: connect failed: 12289` / `DM-WIFI-CONNECT rc=-5`。`logs/20261104-deliver-verify/FINDINGS.md:78-91` | **枚举 + RPC 协商仍然成功**（C6 侧打印 `Slave init_config received from host` / `Host capabilities: 44` / `RPC version negotiated: V2`），但 `set_mode` 被 `12289` 拒 |
| 15 | **2026-11-04 22:11（本次）** | 同上（`98147ff44…`） | 活体读缓冲，见 §2 | P4 控制台在刷 `eh_host_port_sdio: no card interrupt within 2000 ms, polling the companion (#N)`，N 持续增长 |

### 3.1 时间线图示

```
出厂(串口版 esp-hosted)
   │  10-26 15:45  首次烧 INFO  eh_cp_wifi_sta_esp32c6_sdio.bin   (1,094,464 B / 15:25:32 / 98147ff44)
   │  10-26 16:36  改烧 DEBUG  eh_cp_wifi_sta_debug.bin           (1,131,952 B / 16:24:45)
   │  10-28 21:19  改烧 nwsplit eh_cp_network_split_station.bin    (1,115,248 B / 20:29:15 / 9e718d166)
   │  10-28 21:50  刷回 INFO   eh_cp_wifi_sta_esp32c6_sdio.bin     ← S.28「刷回交付原始固件」
   │  10-30 10:58  读到的却是 nwsplit …                            ← ★不确定性 U-2
   │  10-31~11-01  nwsplit 在跑；P4↔C6 大规模成功（scan→connect→IP 192.168.1.9）
   │  11-04 16:13  再试 nwsplit（"纯网络适配器"）→ P4 卡 MCUboot 后
   │  11-04 17:06  刷回 INFO   eh_cp_wifi_sta_esp32c6_sdio.bin     ← 已知可用组合
   ▼  11-04 22:11  INFO 仍在（98147ff44…）——与 §2 指纹一致
```

---

## 4. 第一问的结论：**和现在读出来的，是同一种固件**

### 4.1 直接回答

> **「之前和 ESP32-P4 通讯上过的 WiFi 固件是 C6 中哪一种？」**
> —— 是 **`eh_cp_wifi_sta`**，即 esp-hosted-mcu `examples/wifi/sta/cp` 编出的 **Station 示例从机固件**，版本 `v3.0.10-dirty`、构建于 **2026-10-26 15:25:32**，产物体积 **1,094,464 B**，盘上文件名 `tools/build_flash/c6_hosted/eh_cp_wifi_sta_esp32c6_sdio.bin`。
>
> **「和现在在运行的 c6 读出来的是一样的吗？」**
> —— **一样。** 不是"同一型号的不同构建"，而是**同一颗镜像**：项目名、编译时刻、**内嵌 ELF SHA256（`98147ff44…`）**、bootloader 编译时刻、分区布局，五条指纹全部命中（§2）。

### 4.2 支撑这条结论的证据链（区分"直接"与"推断"）

**直接证据（在册日志里，从烧录到通讯成功是一条不断裂的链）：**
1. 烧录记录：`连接_WiFi_SDIO.md:811`（F.1，`Wrote 1094464 … at 0x00010000`）与 `:1288`（`verify_flash 0x10000 eh_cp_wifi_sta_debug.bin` → `verify OK (digest matched)`）；
2. 读回自报：`logs/2026-10-30_wifi-sdio-session.md:1105` 的 C6 启动日志含 `Transport used :: SDIO only`、`:1115` 含 `wifi_mcu_example: ESP-Hosted MCU WiFi Example Starting...`；raw session 在 **10-26 16:20:49** 明确记录 `Project name: eh_cp_wifi_sta` + `ELF file SHA256: 98147ff…`；
3. 该状态延续到 10-30 11:35 与**官方 P4 主机栈**对接成功（`WIFI INIT OK - link works`，`logs/2026-10-30_wifi-sdio-session.md:34514-34530`）；
4. 11-04 的实测（`logs/20261104-deliver-verify/FINDINGS.md:78-91`）里 C6 侧仍在正常应答 RPC —— 说明这颗固件与主机栈的**传输/RPC 层面是可用的**；
5. 当下活体指纹（§2）与 1–4 指向**同一颗文件**。

**推断（明确标注）：**
- **推断 A**：`eh_cp_wifi_sta` 这颗固件在 10-29 ~ 10-30 之间被**再次烧入过一次**（因为 §5 U-2 的时序冲突）。推断依据是"当下 ELF SHA 就是它"+"11-01 的日志里它是 host 侧的讨论对象而非 C6 的运行态"这类间接线索。**这条我没有直接烧录记录，属推断。**
- **推断 B**：11-01 那次的 `DhcpDnsStatus ip=192.168.1.9` **不是** `eh_cp_wifi_sta` 产生的。理由（强）：`eh_cp_wifi_sta_*.bin` 内 `ehcp_nw_split` 字符串计数 = **0**，它在物理上不可能发出 network-split 的网络 UP 事件；而 11-01 的窗口里 raw session 记录 `ehcp_nw_split`×56 / `feat_nw_split`×135 / `wifi_mcu_example`×**0**（§3 #8）。

### 4.3 如果非要一句最短的答案

**是同一颗**：`c6_hosted/eh_cp_wifi_sta_esp32c6_sdio.bin`（`eh_cp_wifi_sta` v3.0.10-dirty @ Oct 26 2026 15:25:32，ELF SHA `98147ff44…`）。之前通讯成功时是它，现在跑的还是它。

---

## 5. 不确定性清单（诚实标注）

| ID | 不确定项 | 现状 | 影响 | 可怎么解 |
|---|---|---|---|---|
| **U-1** | **11-04 那次"刷回 station 版"的确切时刻与哈希** | 只有 `处置：` 这句话（`246bff2` / `docs/30…:2170` / `docs/50…:129`），**没有 `Wrote … Hash of data verified` 记录** | 低。当下活体指纹已独立证明"现在就是 station 版" | 无需再证；若要精确到秒，查 11-04 会话 raw（未导出为 `.md`）|
| **U-2** | **10-28 21:50「刷回 INFO」（S.28）与 10-30 10:58「读到的却是 nwsplit」冲突** | 我**无法用现有证据消解**。可能：(a) S.28 的 `Wrote 1094464 … 15.6 s` 文本被复制过一次、时序归属有误；(b) 10-29/10-30 之间又有一次未记录的换固件。**两者我都没有决定性证据。** 已查 10-29 全天 raw session：只有 `p4sdiohost`（P4 侧测试固件）的 `app_init` 记录，**没有** C6 的 `Wrote` 记录 —— 即两次写入之间的窗口里没有可归属的 C6 烧录事件 | **中**。不影响 §2/§4 的最终结论（当下指纹是硬的），但让"10-29~10-30 期间 C6 里到底是什么"存在一个不确定窗口 | 逐条翻 10-29 raw session 的 `Wrote`/C6 banner；或接受该窗口（不影响第一问答案）|
| **U-3** | **`.bin` 文件哈希 ≠ 设备 flash 内字节的完全证明** | 我比对的是**镜像内嵌的 `app_desc.elf_sha256`**（应用级身份）与 bootloader 编译时刻，**不是**设备 flash 的读回摘要 | 低。`elf_sha256` 是 IDF 写入 image 的编译期哈希，**足以证明"设备里跑的就是这颗构建产物"**；但它不是 flash 逐字节 CRC | 需要时用 `esptool verify_flash`（**本任务禁止碰串口**，不能做）|
| **U-4** | **出厂镜像的确切身份** | 只有行为证据（每 5 秒 `serial : in/rsp/evt` 统计，`连接_WiFi_SDIO.md:815-822`）；**盘上没有对应的 `.bin`，也没抓到版本 banner** | 低（对"之前通讯成功过的是哪一版"无影响）| 已无处可查（已被覆盖）|
| **U-5** | 两颗固件在设备上的 **IMAGE_CHIP_ID / 安全启动状态** | 未查（不影响结论） | 极低 | `esptool image_info` 已给 `Detected image type: ESP32-C6` |
| **U-6** | **11-01 "成功"的判据层级** | `wlan0` 拿到 IP 是**从机（network-split 版）代做 DHCP 后经事件通告**；`46df328` 自己注明"出方向 IP 报文尚未出网（`ENETUNREACH`），属数据面后续" | **中**，对用户第三问很关键：**"成功"只到"控制面 + 地址配置"这一层，真实数据面从未打通** | 属另一位队友（协议根因）的范畴 |
| — | 未做的取证 | 未打开/未写入任何 `/dev/ttyUSB*`、`/dev/ttyACM*`；未跑 esptool 连串口；未烧录；未改 `nuttx/`、`app/` | — | 遵约束 |

---

## 6. 对第三问（为什么现在连不上 WiFi）的**交叉提示**

（详细归因属协议根因队友；这里只给**固件侧**的两条硬事实，避免与他们的工作重叠）

1. **射频所有权冲突（配对问题）**：`eh_cp_wifi_sta` 是 esp-hosted 的 **Station 示例** —— 从机自己 `esp_wifi_init/start` 并**自己持有射频**，固件内**没有 AP 凭据**（`strings eh_cp_wifi_sta_esp32c6_sdio.bin` 无 `Hello_World`；`sdkconfig.esp32c6.sdio` 无 `CONFIG_EXAMPLE_WIFI_SSID`）。于是主机的任何 `set_mode / get_mode / scan_start / sta_get_ap_info` 都被从机以
   `E (18757) ehcp_wifi_rpc: req_wifi_set_mode:108 failed [esp_wifi_set_mode(req_payload->mode)] = [12289]`（`12289 = ESP_ERR_WIFI_STATE`）拒绝。活体缓冲与 `logs/20261104-deliver-verify/FINDINGS.md:78-91` 双向印证。
2. **11-01 那条能拿 IP 的路已被回退，且依赖**另一颗**固件**：那条路 = host 侧声明 network-split 能力 + **`eh_cp_network_split_station`**（Oct 28 那颗，含 `feat_nw_split`）。
   注意 `1e473ab` 的机制结论说"从机 `EH_CP_FEAT_NW_SPLIT_READY` 镜像**主机声明的能力**"——这句话**只对"是否处理 353 号请求"成立**；**从机得先把这个模块编进固件**才能处理，而 `eh_cp_wifi_sta` 里 `ehcp_nw_split` 字符串 = 0（§1.2）。所以**"改 host 能力就能恢复 IP"是不可行的**，必须换从机固件。
   而该固件 11-04 已被实验证明与当前 host 栈不匹配（P4 卡在 MCUboot 之后），并被**主动回退**。⇒ 现在处于"两边都不是对方要的那颗"的状态。

**另有一条与固件无关、但同样致命的现场事实**（属协议队友）：当下 P4 控制台在刷
`eh_host_port_sdio: no card interrupt within 2000 ms, polling the companion (#N)`（N 持续增长）。
而 C6 缓冲显示最新那次启动**是**完成了 `Slave init_config received from host` / `RPC version negotiated: V2` 的 ⇒ 说明**枚举与 RPC 协商能过，但 SDIO 卡中断/数据面在某个时刻停摆**。

---

## 7. 附：本报告用到的全部证据出处

**盘上镜像 / 配置（离线读取）**
- `tools/build_flash/c6_hosted/eh_cp_wifi_sta_esp32c6_sdio.bin`、`eh_cp_wifi_sta_debug.bin`、`bootloader.bin`、`partition-table.bin`、`sdkconfig.esp32c6.sdio`（配置引用见 §1.3 行号）
- `tools/build_flash/c6_hosted_new/eh_cp_network_split_station.bin`、`bootloader.bin`、`partition-table.bin`、`flash.sh`
- `tools/test/c6_sdio_minimal/prebuilt/{c6sdiotest.bin,bootloader.bin,partition-table.bin}`
- 命令（均离线）：`python3 -m esptool image_info <bin>`、`strings -a <bin>`、内嵌 `esp_app_desc_t` 解析（偏移 0x20）

**docs / git**
- `docs/30_实现/连接_WiFi_SDIO.md:791-822`（附录 F：烧录 + 出厂口径）、`:1080-1130`（附录 H：C6 启动日志）、`:1183-1210`（H.7：INFO/DEBUG 对照）、`:1276-1309`（H.11/H.12：DEBUG 烧入与读回）、`:1220-1243`（H.8：`GPIO_RESET=-1`）、`:4312-4470`、`:4851-4871`（S.27/S.28：换固件对照与刷回）
- `docs/30_实现/应用层_DeskMate_UI.md:2152-2177`（§31：`c6_hosted_new` 不兼容并回退）
- `docs/50_排障经验/08_驱动层调优与多副本陷阱.md:110-133`（同上的排障篇）
- `docs/00_入口/STATUS.md:83-95`（第 13 轮：固件构建完成与收录位置）
- `logs/20261104-deliver-verify/FINDINGS.md:44-54`、`:78-91`
- git：`246bff2`、`f89f1d0`、`e4b4e8f`、`46df328`、`1e473ab`、`d6cf44d`、`12b15f9`、`a26b1bb`、`d58f379`、`6f7a4f4`

**日志 / 会话（含真实时间）**
- `logs/2026-10-30_wifi-sdio-session.md`：`:7775`（INFO 首次写入）、`:8833`（DEBUG 写入）、`:19254`（nwsplit 写入）、`:20172`（刷回 INFO）、`:32405-32479`、`:33050`（C6 nwsplit banner 与特性）、`:34514-34530`（官方 P4 主机栈对接成功）
- `logs/2026-11-02_p4-sync-session.md`：`:25850-25870`（**C6 侧**输出：`eh_example_sta: disconnected (reason 201: NO_AP_FOUND)` + `ehcp_nw_split: Sending network DOWN event`）、`:38829`（**注意：这是 P4 主机侧**收到 261 B 事件并打印 `TX evt → host: event_id=1 msg_id=0x0309 len=261`，别当 C6 侧证据用）
- `logs/2026-10-30_session-raw.jsonl.zstd`、`logs/2026-11-02_session-raw.jsonl.zstd`（epoch 时间刻度来源）
- `logs/20261101-133103_nwsplit/console.txt`（P4 侧 SDIO/RPC 双通：`companion enumerated`、`kind=resp id=550 (WifiStaGetApInfo)`、`kind=evt id=777 (DhcpDnsStatus)`）

**活体（只读 Tabby 缓冲，未触碰串口设备）**
- C6 `/dev/ttyUSB1`：`mcp__tabby__get_terminal_buffer(sessionId="12930a8c-ee87-4740-b4ee-cf9aa297d6f4")`
- P4 `/dev/ttyUSB0`：`mcp__tabby__get_terminal_buffer(sessionId="00c17097-68c2-44c8-bf66-a5331a92be5f")`
