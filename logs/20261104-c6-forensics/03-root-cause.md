# 03 · 为什么现在 P4 连不上 WiFi —— 主从协议根因（代码/日志级）

调查者：teammate `protocol-analyst`（shared task `task-3`）
时间：2026-11-04 22:1x–22:3x（Asia/Shanghai）
方式：**只读**。未打开任何串口设备（ttyUSB0/1、ttyACM0 全程未触碰，读设备输出只用 Tabby terminal buffer）；
未写任何源码、未烧录、未复位。唯一写入物是本文件。

---

## 0. 结论摘要

**根因（一句话）**：主机栈在 **2026-11-01 06:40** 依据"从机应用自己已经把电台 init 并 start 了"这一实测前提，
**故意删掉了 `Req_WifiInit`**（并同时跳过 `Req_WifiStart`）；但**这个前提只对当时在位的从机固件成立**。
今天烧在 C6 上的是另一颗固件（`eh_cp_wifi_sta`，纯 RPC 从机，`app_main()` 立即返回、自己从不 `esp_wifi_init`），
它唯一的初始化来源就是主机那条被删掉的 `Req_WifiInit`。于是 C6 的 WiFi 驱动**从未 init**，
主机发出的第一条 WiFi RPC（`Req_SetWifiMode`）就被 IDF 以
`12289 = 0x3001 = ESP_ERR_WIFI_NOT_INIT` 拒绝，之后 `set_config → connect` 全部继承同一状态，
主机 NuttX 侧把 connect 的拒绝翻译成 `-EIO`，应用看到 `rc=-5`。

**定性**：这不是"从机固件坏"，也不是"版本/协议不配对"（握手与 RPC 编解码都成功），
而是 **主机侧初始化序列缺环（候选 b）** 与 **从机固件角色与主机假设不配对（候选 a）** 的**合取**：
主机按 A 固件的契约跳过 init，现场却是 B 固件在等 init。

| 分项判定 | 结论 | 置信度 |
|---|---|---|
| 现场 C6 固件身份 | `tools/build_flash/c6_hosted/eh_cp_wifi_sta_esp32c6_sdio.bin`（ELF SHA `98147ff4…` 完全一致） | **确定**（100%） |
| 该固件是否自己 init 电台 | 否。`app_main()` 只做 NVS + default event loop；活体日志 `Returned from app_main()` + `set_mode→0x3001` 双重证明 | **确定** |
| 主机是否发过 `Req_WifiInit` | 当前代码路径**不发**（注释与代码都在，见 §3） | **确定** |
| 12289 的语义 | `ESP_ERR_WIFI_NOT_INIT`（IDF 原文："WiFi driver was not installed by esp_wifi_init"） | **确定** |
| `rc=-5` 的来源 | NuttX 粘合层 `eh_hosted_netdev.c:809` 的 `-EIO`，源自从机 `connect` 返回 12289 | **确定** |
| 主机注释所依据的固件 | `eh_cp_network_split_station`（自持电台、自己连 AP），**不是**今天这颗 | **高**（日志三方对照 + 会话字符串计数 + 10-30 从机横幅） |
| 版本/握手不配对 | **排除**。V2 严格匹配通过，`Req_SetWifiMode` 被正确解码并派发到 IDF | **高** |
| `Host capabilities: 44` | CP 打印的是 **TLV tag 0x44**（`eh_cp_core.c:317-318`），**不是**能力值；主机实发的 caps 字节是 0 | **确定** |
| 11-04 试 network-split 时主机是否声明 nw_split 能力 | **已声明**（`<port>/CMakeLists.txt:144-145`，11-04 三个提交里都在）⇒ "没声明能力"这条旧解释**不适用** | **高** |
| SDIO 轮询/伴生复位与射频失败 | **独立**的传输层问题（不同层、不同因），但会放大修复难度 | **高** |

---

## 1. 身份锚定：现在跑的到底是哪一颗 C6 固件

活体（Tabby C6 会话 `12930a8c-ee87-4740-b4ee-cf9aa297d6f4` = /dev/ttyUSB1，最近一次上电）：

```
I (323) app_init: Project name:     eh_cp_wifi_sta
I (328) app_init: App version:      v3.0.10-dirty
I (332) app_init: Compile time:     Oct 26 2026 15:25:32
I (337) app_init: ELF file SHA256:  98147ff44...
I (341) app_init: ESP-IDF:          3ea97e75
I (441) ehcp_core: ESP-Hosted Firmware version :: EH-3.0.10
```

盘上三颗候选镜像，用 `esp_app_desc_t`（magic `32 54 cd ab`，偏移 0x20）解出：

| 镜像 | project | version | 编译时间 | elf_sha256 |
|---|---|---|---|---|
| `tools/build_flash/c6_hosted/eh_cp_wifi_sta_esp32c6_sdio.bin` | **eh_cp_wifi_sta** | **v3.0.10-dirty** | **Oct 26 2026 15:25:32** | **98147ff449618e914d2120b73bc8f5faeafb9b437135fb866f87c6bdee2b43f3** |
| `tools/build_flash/c6_hosted/eh_cp_wifi_sta_debug.bin` | eh_cp_wifi_sta | v3.0.10-dirty | Oct 26 2026 16:24:45 | 017ed307e999f5b0… |
| `tools/build_flash/c6_hosted_new/eh_cp_network_split_station.bin` | eh_cp_network_split_station | 1 | Oct 28 2026 20:29:15 | 9e718d166c9da998… |

⇒ **现场固件 = `c6_hosted/eh_cp_wifi_sta_esp32c6_sdio.bin`**（md5 `3bb11304779b9a5cbeac6b1a7bd01d2c`），
version / project / 编译时间 / ELF SHA 四位一体全部吻合，不是 debug 版、不是 network-split 版。

**从机所用的 IDF 也在本机**：`/home/ez/esp/esp-idf` 的 HEAD = `3ea97e75`（v5.5.6, release/v5.5），
与从机横幅 `ESP-IDF: 3ea97e75` 逐字符一致 —— 因此本文引用的所有 IDF 错误码都来自**编译这颗固件的同一份 IDF 源码**。

**官方 esp-hosted 3.0.10 源码也在本机**（不在本仓库内，只读引用）：
`/home/ez/p4official/managed_components/espressif__esp_hosted/`（`idf_component.yml:25  version: 3.0.10`）。
下文所有 `examples/…`、`coprocessor/…` 路径都指这棵树。

---

## 2. 从机侧契约：`eh_cp_wifi_sta` 是"纯 RPC 从机"，自己从不 init

### 2.1 应用层：`app_main()` 立即返回，不碰 WiFi

`examples/wifi/sta/cp/main/main.c`（这就是 `project(eh_cp_wifi_sta)`，见同目录 `CMakeLists.txt`）：

```c
13: static const char *TAG = "wifi_mcu_example";
15: void app_main(void)
16: {
17:     ESP_LOGI(TAG, "ESP-Hosted MCU WiFi Example Starting...");
19:     /* Initialize NVS */
20:     esp_err_t ret = nvs_flash_init();
...
27:     /* create default event loop */
28:     ESP_ERROR_CHECK(esp_event_loop_create_default());
29: }
```

全文 29 行，**没有 `esp_wifi_init` / `esp_wifi_start` / `esp_wifi_set_config` / `esp_wifi_connect`**。

活体日志逐行对上：

```
I (644) wifi_mcu_example: ESP-Hosted MCU WiFi Example Starting...   ← 正是 main.c:17 的那句
I (664) main_task: Returned from app_main()                         ← app_main 返回（main.c:29）
```

### 2.2 RPC 层：唯一能 init 的入口是主机的 `Req_WifiInit`

`coprocessor/features/eh_cp_feat_rpc_ext_v2/src/eh_cp_feat_rpc_ext_v2_handler_req_wifi.c`

- **第 101–111 行**（`req_wifi_set_mode`），其中 **第 108 行**：
  ```c
  108: 	RPC_RET_FAIL_IF(esp_wifi_set_mode(req_payload->mode));
  ```
  活体从机日志：
  ```
  E (18757) ehcp_wifi_rpc: req_wifi_set_mode:108 failed [esp_wifi_set_mode(req_payload->mode)] = [12289]
  ```
  `RPC_RET_FAIL_IF` 会把**源表达式与文件行号**打进日志 —— 日志里的 `108` 与源码 `:108` 完全一致，
  这证明现场镜像就是用这份 3.0.10 源码编出来的（不是别的分支、不是手改过的版本）。

- **第 505–561 行**（`req_wifi_init`）：**第 556 行**
  ```c
  556: 	RPC_RET_FAIL_IF(esp_wifi_init(get_merged_init_config(&cfg, req_payload->cfg)));
  ```
  即：**`esp_wifi_init` 只在主机发 `Req_WifiInit` 时才会被调用**（经 `--wrap` 走到下面的包装）。

- **第 460–503 行**（`__wrap_esp_wifi_init`）—— 这是 11-01 那次"改配置导致重初始化"的机制本体：
  ```c
  469: 	if (s_mcu_wifi_initialized) {
  471: 		if (has_cached_config && wifi_init_config_changed(config, &cached_wifi_init_config)) {
  472: 			ESP_LOGI(TAG, "WiFi init config changed, reinitializing");
  476: 			esp_wifi_stop();
  477: 			esp_wifi_deinit();
  478: 			s_mcu_wifi_initialized = false;
  479: 		} else {
  480: 			ESP_LOGW(TAG, "WiFi already initialized with same parameters");
  481: 			return ESP_OK;
  482: 		}
  483: 	} else {
  484: 		ESP_LOGI(TAG, "First-time WiFi initialization");
  485: 	}
  ```
  - 第 174–… 行的 `wifi_init_config_changed()` **逐字段比较**并打印 `changed: 旧 -> 新`。
  - 关键：`s_mcu_wifi_initialized` 只有在**曾经 init 成功过**时才为真。今天这颗固件从未 init，
    所以如果主机补发 `Req_WifiInit`，走的是**第 484 行 "First-time WiFi initialization"**（无 stop/deinit）。

**结论（从机侧）**：`eh_cp_wifi_sta` 是**主机驱动的纯 RPC 从机**；
它的电台能不能起来，**完全取决于主机发不发 `Req_WifiInit`**。
"从机应用已自持电台"这句话对这颗固件**不成立**。

---

## 3. 主机侧契约：为什么它不发 init（以及这个决定是什么时候做的）

### 3.1 当前代码

`nuttx/arch/risc-v/src/esp32p4/esp_hosted/official/port/os/nuttx/src/eh_hosted_nuttx_entry.c`

```c
333:  {
334:    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
335:    esp_err_t          werr;
337:    /* Do NOT re-initialize the companion's Wi-Fi here.
339:     * Measured: Req_WifiInit makes the companion re-run its Wi-Fi init ("WiFi
340:     * init config changed, reinitializing" on its console), which stops the
341:     * radio its own application had already started. ... */
348:    syslog(LOG_INFO, "%s: leaving the companion's Wi-Fi initialized/started "
349:           "(no RPC wifi_init)\n", ENTRY_TAG);
351:    werr = ESP_OK;
353:    if (werr == ESP_OK)
355:      {  werr = eh_host_wifi_set_mode(WIFI_MODE_STA);          ← 直接 set_mode，前面没有 init
360:    /* No eh_host_wifi_start() here. ... */
372:    syslog(LOG_INFO, "%s: skipping RPC wifi_start (...)\n", ENTRY_TAG);
```

被删掉的原始调用（`eh_host_wifi.c:64–124` 的 `eh_host_wifi_init()`，内部发 `RPC_ID__Req_WifiInit`）
仍完整存在，只是**没人调用**。`eh_host_wifi_set_mode()`（`eh_host_wifi.c:141–151`）
把从机返回的状态**原样透传**：`int rc = r->resp_event_status; return (esp_err_t)rc;` ——
所以主机侧拿到的就是 12289，不做任何映射。

### 3.2 这次删除的提交与日期（本仓库 HEAD 的祖先链上）

- 提交 **`f62cab7`**（2026-11-01 06:40:44，`test(wifi): ENOTTY 查明是结构性的（本树 netdev 无 SIOCSIWSCAN 派发）；从机 scan 回 WIFI_MODE`），
  **是 HEAD 的祖先**（`git merge-base --is-ancestor f62cab7 HEAD` = 真）。
  同内容的兄弟提交 `2e474ab` / `2739e14` 在其他 ref 上，**都不是 HEAD 的祖先**。
- 该提交改的文件：`nuttx/arch/risc-v/src/common/espressif/esp_hosted_official/port/os/nuttx/src/eh_hosted_nuttx_entry.c`
  （搬迁前的旧路径），diff 的正是这两行：

  ```diff
  -    werr = eh_host_wifi_init(&init_cfg);
  -    syslog(... "%s: RPC wifi_init -> %d\n", ENTRY_TAG, werr);
  +    /* Do NOT re-initialize the companion's Wi-Fi here. ... */
  +    syslog(LOG_INFO, "%s: leaving the companion's Wi-Fi initialized/started (no RPC wifi_init)\n", ENTRY_TAG);
  +    (void)init_cfg;
  +    werr = ESP_OK;
  ```

### 3.3 这个决定所依据的实测，是在**另一颗从机固件**在位时做的

同一天的三个日志目录（均已落盘，逐行核对无误）：

| 日志 | 行 | 原始记录 | 含义 |
|---|---|---|---|
| `logs/20261101-060525_wifi-init/boot.txt` | 198 | `eh_hosted_nuttx: RPC wifi_init -> 0` | 主机当时**发了** WifiInit，从机回 0 |
| `logs/20261101-062845_scan-diag/boot.txt` | 220 | `eh_hosted_nuttx: direct eh_host_wifi_scan_start -> 12290` | 12290 = **0x3002 ESP_ERR_WIFI_NOT_STARTED**（电台存在但被停） |
| `logs/20261101-063304_scan-ioctl/boot.txt` | 149 / 178 / 197 | `leaving the companion's Wi-Fi initialized/started (no RPC wifi_init)` / `RPC wifi_set_mode(STA) -> 0` / `direct eh_host_wifi_scan_start -> 12294` | 12294 = **0x3006 ESP_ERR_WIFI_STATE**；且 `set_mode` 返回 **0** |

**判别点**：06:33 那次 `wifi_set_mode -> 0`（成功）。
按 §2.2，`set_mode` 成功 ⇒ `esp_wifi_set_mode` 没报 NOT_INIT ⇒ **从机的 WiFi 驱动当时是已 init 的**。
而主机当时**已经不发送** WifiInit（第 149 行就是那句"no RPC wifi_init"）。
⇒ 当时在位的那颗固件**是自己 init 的**，`eh_cp_wifi_sta` 不是（今天实测 `set_mode -> 12289`）。

**当时 C6 里是哪颗固件？两条独立证据都指向 network-split 版：**

1. 从机横幅有落盘记录：`logs/2026-10-30_wifi-sdio-session.md:32407`（及 `:33050`）
   `I (327) app_init: Project name:     eh_cp_network_split_station`。
2. 队友 firmware-historian 对 raw session 窗口（10-31 05:36 → 11-01 14:16）的字符串计数：
   `ehcp_nw_split` × 56、`feat_nw_split` × 135、`eh_example_sta` × 42、**`wifi_mcu_example` × 0**。
   而 `wifi_mcu_example` 是 `eh_cp_wifi_sta` 独有的 app tag（`examples/wifi/sta/cp/main/main.c:13`），
   `ehcp_nw_split / eh_example_sta` 只存在于 network-split 那颗镜像里（我对两个 .bin 做过 `strings` 交叉验证）。
3. 机制上自洽：`examples/network_split/station/cp/main/main.c` 的 app 自己把电台拉起来 ——
   ```c
   17: static const char *TAG = "nw_split_sta_cp";
   ...
   31:     ESP_ERROR_CHECK(eh_cp_init());
   32:     /* Brings up the STA (init+start); connects only if the core nw_split auto-connect-on-STA_START is enabled */
   33:     ESP_ERROR_CHECK(eh_example_sta_start(CONFIG_EH_EXAMPLE_WIFI_SSID, CONFIG_EH_EXAMPLE_WIFI_PASSWORD));
   ```
   所以在 11-01 它会先自己 init（用 C6 端 sdkconfig 的 `static_rx_buf_num=20`），
   主机再发 WifiInit（NuttX shim 里是 `10`）时，`wifi_init_config_changed()` 命中 →
   `esp_wifi_stop(); esp_wifi_deinit();`（`:476-477`）→ 之后的调用全变 **0x3002 NOT_STARTED**（与 06:28 的 12290 吻合）。

**⇒ 结论（§3 小结）**：主机注释里的"the radio is already initialized and started by the slave application"
对 network-split 固件为真、对今天这颗 `eh_cp_wifi_sta` 为假。
**主机按 A 的契约跳过 init，从机 B 却在等 init** —— 死结就在这里。

---

## 4. 完整因果链（端到端，逐步带证据）

```
① C6 上电（被主机 GPIO54 断电重启，见 §8）→ eh_cp_wifi_sta 跑起来
     证据：C6 日志 `rst:0x1 (POWERON)`；`Project name: eh_cp_wifi_sta`
② app_main 只做 NVS + event loop 就返回，电台未 init
     证据：main.c:15-29；C6 日志 `I (664) main_task: Returned from app_main()`
③ SDIO 枚举成功，主机发 init_config(caps)，从机回 ESPInit 事件
     证据：C6 日志 `Slave init_config received from host` / `ehcp_event_handler: ESP init event`
           / `TX evt → host: event_id=1 msg_id=0x0301 len=0` / `ehcp_mcu_rpc_evt: event ESPInit`
④ 主机收到 ESPInit 后，按 eh_hosted_nuttx_entry.c:348-355 **直接发 set_mode，跳过 WifiInit**
     证据：源码；11-01 起 `logs/…/boot.txt:149` 的同一句
⑤ 从机把 set_mode 原样交给 IDF；IDF 因"从未 esp_wifi_init"返回 0x3001
     证据：CP handler `:108`；IDF `esp_wifi.h:63` `ESP_ERR_WIFI_NOT_INIT`；C6 日志 `= [12289]`
⑥ 后续 set_config **通过**、connect 被拒 12289 → 主机 netdev 把 connect 的失败翻成 -EIO(-5)
     证据：`eh_hosted_netdev.c:808-809`；`logs/20261104-deliver-verify/FINDINGS.md`
           `eh_hosted_netdev: connect failed: 12289` → `SIOCSIWESSID(...) failed: 5` → `DM-WIFI-CONNECT … rc=-5`
     注：FINDINGS 里**没有** `set_config failed` 行 ⇒ 从机对 `req_wifi_set_config` 回的是成功。
       （`esp_wifi_set_config` 在 IDF 的闭源 blob 里，本轮未核实它是否检查 init 状态；
         这条只作为观测事实记录，不作为论据。）
```

**rc=-5 的确切出处**（`official/port/os/nuttx/src/eh_hosted_netdev.c`）：
`case SIOCSIWESSID:`（:761）→ `eh_host_wifi_set_config()`（:796-801，失败才 -EIO）→
`eh_host_wifi_connect()`（:806-808，`syslog("%s: connect failed: %d\n", …, ret)`）→ **`:809 return -EIO;`**。
`-EIO` 在 NuttX 里是 `-5`，与应用打印的 `rc=-5` 一致。**主机没有做任何"错误码翻译"，它就是 -EIO。**

---

## 5. 为什么是 NOT_INIT(0x3001) 而不是注释预期的 NOT_STARTED(0x3002)

两者在 IDF 里是**相邻但语义不同**的两个状态（`/home/ez/esp/esp-idf/components/esp_wifi/include/esp_wifi.h:63-68`，
`ESP_ERR_WIFI_BASE = 0x3000` 见 `components/esp_common/include/esp_err.h:39`）：

| 值 | 宏 | IDF 原文注释 | 什么情况下出现 |
|---|---|---|---|
| 12289 / 0x3001 | `ESP_ERR_WIFI_NOT_INIT` | "WiFi driver was **not installed by esp_wifi_init**" | **从未**调用过 `esp_wifi_init`（今天） |
| 12290 / 0x3002 | `ESP_ERR_WIFI_NOT_STARTED` | "WiFi driver was **not started by esp_wifi_start**" | init 过、但被 stop/deinit 了（11-01 那次） |
| 12293 / 0x3005 | `ESP_ERR_WIFI_MODE` | "WiFi mode error" | — |
| 12294 / 0x3006 | `ESP_ERR_WIFI_STATE` | "WiFi internal state error" | 连接中拒绝 scan 之类（11-01 06:33） |

**今天给 0x3001 的，只能是"从未被 init"这一类从机固件** —— 也就是 §2 的 `eh_cp_wifi_sta`：
它的 app 不 init、它的 `ehcp_wifi` 特性扩展启动时也不 init（否则 18 秒后的 set_mode 不会报 NOT_INIT）、
而主机又不发 `Req_WifiInit` ⇒ 三条路全断，`s_mcu_wifi_initialized == false`，IDF 只能给 NOT_INIT。

注释预期的 0x3002 需要"曾经 init 过"这个前提；今天这颗固件**连这个前提都不存在**。
换句话说：**0x3001 vs 0x3002 的差别，恰好就是"从机固件换过"的指纹。**

---

## 6. 候选根因对照表

| # | 候选 | 支持证据 | 反对/存疑 | 判别实验（优先只读） | 判定 |
|---|---|---|---|---|---|
| **a** | 从机固件选型/角色与主机假设不配对（`eh_cp_wifi_sta`＝纯 RPC 从机 vs `eh_cp_network_split_station`＝自持电台） | §1 身份锚定；§2 app 不 init；11-01 日志显示当时那颗是自持的（set_mode→0）；10-30 从机横幅 = network_split | 单看 a 不足以解释"主机不发 init"这个主动行为 | 只读：对 `.bin` 取 `esp_app_desc` 比对（已做）；读 `examples/*/cp/main/main.c`（已做） | **成立（合取的一半）** |
| **b** | 主机初始化序列缺环：`Req_WifiInit` 被删、`Req_WifiStart` 被跳过 | `eh_hosted_nuttx_entry.c:348-355,372`；提交 `f62cab7` 的 diff 就是删 init；`eh_host_wifi_init()` 成了死代码 | 该改动**在当时那颗固件上是有依据的**（06:05 `wifi_init->0` 引发的 stop/deinit 伤害，06:28 的 0x3002 是它的后果） | 只读：`git log -S` + 三个 boot.txt（已做） | **成立（合取的另一半，且是"缺失的动作"）** |
| **c** | 版本/协议/握手不配对（EH-3.0.10 vs 主机、`wire_hdr=V1` vs `rpc=V2`、caps=44） | —— | **反证充分**：`EH_PRIV_RPC_VERSION 0x1A` 是 *strict-match required*，CP 侧不匹配会直接 `abort()`（`eh_cp_core.c:420-426`），实际打印 `RPC version negotiated: V2` ⇒ 双方都是 V2；`Req_SetWifiMode` 被**正确解码**（参数 `mode` 落到了 `esp_wifi_set_mode`）；`Host capabilities: 44` 是 CP 的**打印 bug**（见 §7.3/§9.2） | 只读：源码 + bin strings（已做） | **排除** |
| d1 | AP 凭据错/信号问题 | —— | 失败发生在**任何凭据被使用之前**（set_mode 就死了；set_config 虽过但 connect 直接 12289）；换 SSID/密码无效 | 只读：FINDINGS.md 已记录与凭据无关 | **排除** |
| d2 | 主机 SDIO 传输层（轮询/伴生复位） | 见 §8 | 与 12289 **不同层**：RPC 往返本身是通的（从机答了 12289、capability 协商完成） | 见 §8 判据 | **独立问题，非本问根因** |

---

## 7. 若要修：方向 A / 方向 B（只写方案，本轮不动手）

### 7.1 方向 A：主机把 `Req_WifiInit`（以及 `Req_WifiStart`）发回来 —— 针对今天这颗固件

- **前提 1**：从机是"纯 RPC 从机"（今天的 `eh_cp_wifi_sta`）。此时 `__wrap_esp_wifi_init` 走
  `:484 "First-time WiFi initialization"`，**不会**触发 `:472` 的 stop+deinit —— 注释担心的伤害不存在。
- **前提 2**：init 之后还要 `start`。`Req_WifiStart` 的"从机永远不回"是 **11-01 那颗 network-split 固件**的性质
  （它自己在连一个不存在的 SSID，事件流把响应挤在后面）；今天这颗固件没有自生事件流，
  **预期** `Req_WifiStart` 能正常返回 —— 但这是**推断，需要实测确认**（我未验证）。
- **前提 3**：`Req_WifiInit` 携带的配置要和从机"现用配置"一致，否则在某些组合下会触发
  `changed → stop+deinit`。从机比较的是**逐字段**（`wifi_init_config_changed()`，:174 起）。
  主机侧这份配置来自 `official/port/os/nuttx/include/sdkconfig.h` 的 shim（`static_rx_buf_num=10` 等），
  而 C6 自己的 sdkconfig 是 `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=20`
  （`tools/build_flash/c6_hosted/sdkconfig.esp32c6.sdio:1374`）。**这就是 11-01 "20 -> 10" 的来源。**
- **最小改动方案（按优先级）**：
  1. **首选**：把 `sdkconfig.h` shim 里的数值对齐到 C6 端 sdkconfig（至少 `static_rx_buf_num=20`、
     以及 `dynamic_rx_buf_num` / `tx_buf_type` / `ampdu_*` / `nvs_enable` / `mgmt_sbuf_num` 等被比较的字段）。
     这样即便将来刷回自持电台的固件，`wifi_init_config_changed()` 也会返回 false，
     从机回 `"WiFi already initialized with same parameters"` + `ESP_OK`（`:480-481`）—— **对两种固件都安全**。
  2. 在 `eh_hosted_nuttx_entry.c` 恢复 `eh_host_wifi_init(&init_cfg)` 调用，并**按顺序**：
     `wifi_init → set_mode(STA) → wifi_start`（start 用 best-effort 或带超时，禁止无条件阻塞）。
     恢复条件最好由**从机固件类别**决定（例如用一个 Kconfig/板级开关），而不是写死。
  3. 修好后重测判据（见 §10）。
- **风险**：若在**自持电台**的固件上无条件补发 init，就会回到 11-01 的 stop+deinit 伤害（0x3002）——
  这正是方案 1 要消除的。另一个风险：`Req_WifiStart` 若真的不返回，会再次卡住启动线程（当前代码把它注释掉的理由）。
  因此建议 start 走"带超时的 best-effort"，并在下一次 `get_mode`/`scan` 上做真实判据。

### 7.2 方向 B：刷回 `eh_cp_network_split_station`（自持电台 + 网络分离）

- **前提 1**：那颗固件的 app 会自己 `eh_example_sta_start()`（init+start），**并且**要连上 AP 就得在**构建时**
  带 `CONFIG_EH_EXAMPLE_WIFI_SSID/PASSWORD`（`examples/network_split/station/cp/main/main.c:33`）。
  盘上那颗 `.bin`（Oct 28 20:29）没有 `Hello_World` 之类的凭据字符串 ⇒ 直接刷上去它也**不一定连得上你的 AP**。
- **前提 2**：主机侧要能处理"CP 持有 IP"的语义（`feat_nw_split`）。**准确现状**（我逐项核过）：
  - vendored 树**有** `official/host/features/eh_host_feat_nw_split/{src/*.c,include/*.h}`（`_v2_req.c` / `_v2_resp_evt.c` 都在），
    只缺官方那一份 `host/features/eh_host_feat_nw_split/CMakeLists.txt`；
  - vendored 的 `official/port/os/nuttx/CMakeLists.txt:144-145` **已经定义**
    `CONFIG_ESP_HOSTED_HOST_FEAT_NW_SPLIT=1` / `..._READY=1`（注释指向 appendix BY.32），
    并配了 LWIP 端口范围；`1e473ab`（11-01 13:20）当时**还没有**这两行。
  - ⇒ **11-04 那次（`246bff2` / `e4b4e8f` / `f89f1d0` 三个提交的树）这个声明是开着的**
    （我 `git show <commit>:<CMakeLists>` 逐个确认过），所以"刷 network-split 卡在 MCUboot 之后"
    **不能**再用"主机没声明 network-split 能力"来解释 —— 那条路已经关上了，需要新的证据。
- **关于 `1e473ab` 的机制推断**：它写"`*_READY` 由主机声明的能力决定（从机日志 `Host capabilities: 44`）" —— **不成立**：
  - `44` 是 CP 打印的 **TLV tag**（见 §9.2），与任何能力位无关；
  - CP 侧的 353 号请求（`RPC_ID__Req_GetDhcpDnsStatus`）确实被 `#if EH_CP_FEAT_NW_SPLIT_READY` 包着
    （`coprocessor/features/eh_cp_feat_rpc_ext_v2/src/eh_cp_feat_rpc_ext_v2_dispatcher_req.c:261-269`），
    但这个宏来自 **CP 自己的构建配置**
    （`coprocessor/eh_cp_core/include/eh_cp_master_config.h:229-233`
     + `coprocessor/features/eh_cp_feat_nw_split/Kconfig.ext:18-20`，默认 y 的条件全是 CP 侧开关：
     `ESP_HOSTED_CP_FEAT_WIFI_READY && ESP_HOSTED_CP_WIFI_ENABLED && LWIP_ENABLE && ESP_NETIF_TCPIP_LWIP`）。
  - 主机侧那对 `CONFIG_ESP_HOSTED_HOST_FEAT_NW_SPLIT*` 管的是**主机自己**要不要编进这个特性
    （端口 CMakeLists 的注释说得很清楚：主机侧需要它才能**消费**从机的答复并自己套用地址）。
    vendored 端口 CMakeLists 里那句"The companion gates its req_get_dhcp_dns_status() on this capability"
    是**把主机侧开关当成了从机的门禁**，与 CP 源码不符 —— 建议改成"主机侧需要它来消费答复"。
  - ⇒ "353 无人应答"的真因需要**重新推导**（候选：那颗 CP 镜像的 `*_READY` 实际为 0（LWIP 未开）、
    请求发到了错 endpoint/ID、或响应淹没在事件流里）。**不要再拿 44 当依据。**
- **判定**：方向 B 是"回到 11-01 曾经跑通过（scan 25 AP / DHCP 192.168.1.9）的组合"，
  但需要 (i) 重编带凭据的 CP 固件 + (ii) 主机侧补 nw_split 能力，工作量与不确定性都大于方向 A，
  且**必须人工按键进 C6 下载模式**（`flash.sh` 前置条件，C6 的 IO9 没接到 P4）。

### 7.3 顺带：`Host capabilities: 44` / `RawTP Stop` / 流控 0% 的正确解读

- `Host capabilities: 44`：CP 端 `coprocessor/eh_cp_core/src/eh_cp_core.c:317-318`
  ```c
  317: 		if (*pos == HOST_CAPABILITIES) {
  318: 			ESP_LOGI(TAG, "Host capabilities: %2x", *pos);
  ```
  打印的是 `*pos`＝**tag 本身**（`HOST_CAPABILITIES = 0x44`，`coprocessor/eh_cp_transport/include/common/eh_transport.h:58`；
  主机侧同名 tag `EH_HOST_PRIV_HOST_CAPABILITIES 0x44u`，`official/common/eh_tlv/include/eh_tlv_tags.h:67`），
  **不是能力值**。主机实际发的能力字节是 **0**（`official/host/mcu/eh_host_mcu_transport/src/eh_host_mcu_transport_init.c:125-129`
  的第 3 实参是字面量 `0`；我 diff 过 vendored 与官方 3.0.10 的这个文件，**完全一致，非本仓库改动**）。
  ⇒ 这行日志**不能**用来推断"主机声明了/没声明什么能力"。
- `RawTP host request: Stop` → `RawTP unchanged: Stopped`：主机发 `raw_tp_direction = EH_HOST_PORT_TEST_RAW_TP_DIR`，
  在未开 `CONFIG_ESP_HOSTED_RAW_THROUGHPUT_TRANSPORT` 时它是 `ESP_TEST_RAW_TP_NONE = 0`
  （`official/host/eh_host_config/include/eh_host_port_master_config.h:981-998`）＝ Stop；
  从机日志里 `RawTP request received: cap=0x00` 正是这个 0。RawTP 是**吞吐基准测试**模式，Stopped 是正常运行态。**良性。**
- `ESP<-Host wifi flow ctl start thres [0%]` / `clear thres [0%]`：主机发的是
  `EH_HOST_PORT_WIFI_TX_DATA_THROTTLE_{LOW,HIGH}_THRESHOLD`，本树默认就是 **0/0**
  （同文件 `:976-977`）。**不是**本次失败的原因；但若将来 WiFi 通了，0% 阈值值得复核。

---

## 8. SDIO 轮询 与 伴生芯片复位：独立的传输层问题

### 8.1 `no card interrupt within 2000 ms, polling the companion (#N)` 是什么

源码：`nuttx/arch/risc-v/src/esp32p4/esp_hosted/official/port/os/nuttx/src/eh_host_port_sdio.c:1366`
（所在等待循环 `:1330-1390`）。它由 `esp_sdmmc_wait_card_intr()` 超时触发，**代码自己的注释说得很清楚**：

```
      /* Nothing inside the slice.
       *
       * A timeout here is not a failure: the transport's next step is to read
       * the companion's interrupt register, which answers the same question.
       * It is counted and reported, though, because "is this link
       * interrupt-driven or polled?" is exactly what was unknown before.
       */
```

- 它是 **LOG_INFO、且被限流**（`poll_falls <= 8 || poll_falls % 64 == 0`）——
  所以现场看到的是 `#1024 → #1088 → #1152 → #1216` 这种 64 递增的稀疏打印，不是"每 2 秒一条"。
- **它不是失败，是降级**：宿主退化成"主动读从机中断寄存器"来取数据。
  **RPC 往返本身是通的**，证据：C6 收到了 `init_config`、完成了 capability 协商、
  并且在 T=18757ms **回答了** `req_wifi_set_mode`（回 12289）—— 一条死的链路不可能回包。
- 与 WiFi 失败的关系：**不同层、不同因**。这条日志讲的是"中断线（DAT1）没拉起 → 改轮询"，
  WiFi 失败讲的是"从机射频从未 init"。**不能把两件事混为一谈**；也不要把 `#N` 的增长当成 WiFi 失败的证据。

### 8.2 伴生芯片被反复复位 —— 这才是传输层真正要盯的事

主机侧**每次 bring-up 都会给 C6 断电重启**（两颗代码点）：

- `transport/esp_hosted_sdio.c:535-544`：`GPIO54(C6_EN)` 拉低 `CONFIG_ESPRESSIF_HOSTED_SDIO_EN_RESET_MS`
  再拉高、settle `CONFIG_ESPRESSIF_HOSTED_SDIO_EN_DELAY_MS`，并打印
  `companion reset: GPIO%d held low %d ms` + `companion enable held high (GPIO%d), settling %d ms`。
  现场 P4 控制台（Tabby 会话 `00c17097…`，缓冲区开头）确实出现这两句的交错输出，
  且 defconfig 里 `CONFIG_ESPRESSIF_HOSTED_SDIO_EN_DELAY_MS=1000`（`board/esp32p4/…/configs/nsh/defconfig:495`）。
- `transport/esp_hosted_sdio.c:2155-2172`（`CONFIG_ESPRESSIF_HOSTED_OFFICIAL` 路径）：
  `SDIO_RESET` + 重新编程时钟 + **再复位一次伴生芯片**（"500 ms low, then 10 s to come up"）。

**从机侧的指纹吻合**：C6 日志里是 `rst:0x1 (POWERON),boot:0xc (SPI_FAST_FLASH_BOOT)` ——
**上电复位**（不是软复位/WDT，那些会是别的 rst 码），与"EN 脚被拉低再拉高"完全一致。
**这解释了为什么从机日志里会出现多次完整的启动横幅。**

对根因的影响：
1. **不改变结论**：每个复位周期结束后，主机走的还是 §4 的老路（收 ESPInit → 直接 set_mode），
   所以 `12289` 是**每次复位后必然重现**的确定性结果，而不是偶发。
2. **但它是修复 A 的前置条件**：任何"在从机里建立的状态"都会被下一次 EN 复位抹掉。
   好在方向 A 的 `init` 就挂在"每次握手之后"的序列里（`:333-355`），天然幂等；
   不过若复位循环非常频繁（Lead 现场观察：250 s 内约 14 次复位、仅 3 次走完握手），
   会出现"刚 init 完就被复位"的窗口 ⇒ **建议把传输层 bring-up 的稳定性与射频初始化分开跟踪、分别验收**。

---

## 9. 对现有文档/既有结论的更正（4 处，均有源码依据）

1. **`logs/20261104-deliver-verify/FINDINGS.md` 把 12289 写成 `ESP_ERR_WIFI_STATE`** —— 错。
   12289 = `0x3001` = **`ESP_ERR_WIFI_NOT_INIT`**；`ESP_ERR_WIFI_STATE` 是 `0x3006` = 12294。
   这个差别不是术语洁癖：NOT_INIT 直接把矛头指向"缺 `esp_wifi_init`"，
   而"STATE"会让人以为是在别的状态问题上打转。**建议更正 FINDINGS.md 与所有引用它的结论。**
2. **`eh_hosted_nuttx_entry.c:435` 的注释把 0x3006 写成 `ESP_ERR_WIFI_MODE`** —— 错。
   0x3006 = `ESP_ERR_WIFI_STATE`；`ESP_ERR_WIFI_MODE` 是 0x3005 = 12293。
   （`d58f379` 的标题写的是 WIFI_STATE，反而是对的。）
3. **文档 §5 对两颗固件角色的命名（"c6_hosted = station 示例/射频自持、c6_hosted_new = 纯网络适配器"）与实际相反。**
   按源码与镜像：
   - `c6_hosted/eh_cp_wifi_sta_esp32c6_sdio.bin` → app 只做 NVS+event loop，**不自持电台**＝**纯 RPC 从机**（本次事故的主角）；
   - `c6_hosted_new/eh_cp_network_split_station.bin` → app 调 `eh_example_sta_start()`（init+start，可能自连）＝**自持电台那颗**。
   11-01 的 `set_mode -> 0` / `scan -> 0x3006` / "STA is connecting, scan are not allowed" 全是**自持那颗**的表现，
   被当成了"c6_hosted 那颗"的行为写进了主机注释 —— 这是本次误配的**文档级根源**。
4. **`1e473ab` 的机制推断（"`*_READY` 由主机声明的能力决定"，依据 `Host capabilities: 44`）不成立**：
   44 是 CP 打印的 **TLV tag**（`eh_cp_core.c:317-318` + `eh_transport.h:58`），
   而 `EH_CP_FEAT_NW_SPLIT_READY` 是 **CP 构建期** Kconfig（`Kconfig.ext:18-20`，
   门禁在 `eh_cp_feat_rpc_ext_v2_dispatcher_req.c:261-269`）。
   而且：`CONFIG_ESP_HOSTED_HOST_FEAT_NW_SPLIT=1` 这两行在 **11-04 的三个提交里已经存在**
   （`246bff2`/`e4b4e8f`/`f89f1d0` 我逐个 `git show` 过；它是在 11-01 13:20 的 `1e473ab` **之后**才加进去的），
   所以 **11-04 试 network-split 时主机侧声明是开着的** —— "没声明能力"这条解释对那次失败**不适用**。
   "353 无人应答"和"11-04 卡住"都需要重新找原因。

---

## 10. 证据索引 / 可复核判据

### 10.1 一条命令复核身份锚定

```bash
python3 - <<'PY'
import pathlib
d = pathlib.Path("tools/build_flash/c6_hosted/eh_cp_wifi_sta_esp32c6_sdio.bin").read_bytes()
i = d.find(b"\x32\x54\xcd\xab")                     # esp_app_desc magic
print(d[i+16:i+32].split(b"\0")[0], d[i+48:i+64].split(b"\0")[0], d[i+80:i+96].split(b"\0")[0], d[i+144:i+176].hex())
PY
# 期望： b'v3.0.10-dirty' b'eh_cp_wifi_sta' b'15:25:32' 98147ff449618e91…（与从机横幅逐字对上）
```

### 10.2 关键 file:line 一览（仓库内）

| 位置 | 作用 |
|---|---|
| `…/esp_hosted/official/port/os/nuttx/src/eh_hosted_nuttx_entry.c:323-373` | 主机**故意不发** `Req_WifiInit`/`Req_WifiStart`，直接 set_mode（根因所在） |
| 同上 `:348-349` | 日志 `leaving the companion's Wi-Fi initialized/started (no RPC wifi_init)` |
| `…/official/host/features/eh_host_feat_wifi/src/eh_host_wifi.c:64-124` | 被架空的 `eh_host_wifi_init()`（发行 `Req_WifiInit`） |
| 同上 `:141-151` | `set_mode` 把从机状态原样透传（12289 直达上层） |
| 同上 `:458` | `eh_host_wifi_connect()` = 空请求 RPC（`Req_WifiConnect`） |
| `…/official/port/os/nuttx/src/eh_hosted_netdev.c:761,796-809` | `SIOCSIWESSID`：set_config→connect，失败 `return -EIO`（**rc=-5 的出处**） |
| `…/official/port/os/nuttx/include/sdkconfig.h` | 主机侧 init 配置 shim（`static_rx_buf_num=10`，与 C6 的 20 不一致） |
| `…/transport/esp_hosted_sdio.c:535-544` | bring-up 时 GPIO54 给 C6 断电重启（100 ms/1000 ms，Kconfig 可调） |
| 同上 `:2155-2172` | official 路径再复位一次（500 ms low / 10 s settle） |
| `…/official/port/os/nuttx/src/eh_host_port_sdio.c:1366` | `no card interrupt … polling the companion (#N)`（INFO/限流/非致命） |
| `…/official/common/eh_tlv/include/eh_tlv_tags.h:45,50,67,75,76` | `EH_PRIV_RPC_VERSION 0x1A`（strict-match）、`ESP_HOSTED_HDR_VERSION_V1=0x01`、`EH_HOST_PRIV_HOST_CAPABILITIES 0x44u` |

### 10.3 关键 file:line 一览（官方 esp-hosted 3.0.10，`/home/ez/p4official/managed_components/espressif__esp_hosted/`，仓库外只读）

| 位置 | 作用 |
|---|---|
| `examples/wifi/sta/cp/main/main.c:13-29` | `eh_cp_wifi_sta` 的 app：**只做 NVS+event loop，不 init 电台** |
| `examples/network_split/station/cp/main/main.c:17,31-33` | 另一颗的 app：`eh_example_sta_start()` **自持电台** |
| `coprocessor/features/eh_cp_feat_rpc_ext_v2/src/eh_cp_feat_rpc_ext_v2_handler_req_wifi.c:101-111`（**108**） | `req_wifi_set_mode` → 日志行号 108 的来源 |
| 同上 `:164-…` | `wifi_init_config_changed()` 逐字段比较（`changed: 20 -> 10` 的来源） |
| 同上 `:460-503`（472/476-478/484） | `__wrap_esp_wifi_init`：changed → stop+deinit；首次 → "First-time" |
| 同上 `:505-561`（556） | `req_wifi_init` → 唯一 `esp_wifi_init` 调用点 |
| `coprocessor/eh_cp_core/src/eh_cp_core.c:317-318` | `Host capabilities: %2x` **打印 tag 不是值**（44 的真相） |
| 同上 `:420-426` | RPC 版本 strict-match，不匹配 `abort()`（所以"握手成功"本身就是强证据） |
| 同上 `:469-473` | `Negotiation complete: wire_hdr=V%u (%s), rpc=V%u (%s)` |
| `coprocessor/eh_cp_transport/include/common/eh_transport.h:58` | `HOST_CAPABILITIES=0x44` |
| `coprocessor/features/eh_cp_feat_nw_split/Kconfig.ext:18-20` + `coprocessor/eh_cp_core/include/eh_cp_master_config.h:229-233` | `EH_CP_FEAT_NW_SPLIT_READY` 是 **CP 构建期**开关 |
| `/home/ez/esp/esp-idf/components/esp_wifi/include/esp_wifi.h:63-68`、`components/esp_common/include/esp_err.h:39` | IDF 错误码定义（编译该固件的同一份 IDF，HEAD=3ea97e75） |

### 10.4 修好之后的验收判据（方向 A）

1. 从机控制台应出现（且只出现一次每握手）：
   `=== __wrap_esp_wifi_init called ===` → `First-time WiFi initialization`（**不是** `changed, reinitializing`）
   → `__real_esp_wifi_init returned: 0`。
2. 主机侧：`RPC wifi_init -> 0`，随后 `RPC wifi_set_mode(STA) -> 0`（**不再是 12289**）。
3. `Req_WifiStart` 返回 0（若仍不返回，说明卡点回到 BY.17 的老问题，需另行处理）。
4. `SIOCSIWSCAN(wlan0)` 返回 0 且 `scan_get_ap_num > 0`；`SIOCSIWESSID` 不再 `rc=-5`。
5. 反向判据（防回归）：在**自持电台**的固件上验证 `wifi_init` 不会触发
   `WiFi init config changed, reinitializing`（即 §7.1 方案 1 的字段对齐生效）。

### 10.5 本轮**未**验证的事项（不要让它们被当成结论）

- 现场"250 s 内约 14 次伴生复位、仅 3 次走完握手"的**计数**来自 Lead 的现场观察；
  我验证的是**机制**（§8.2）与**从机侧 `rst:0x1 (POWERON)` 指纹**，没有自己数过。
- `Req_WifiStart` 在今天这颗固件上是否会正常返回 —— **推断**，需实测（判据 3）。
- 11-04 那次"刷 network-split 后卡在 MCUboot 之后"的**原始日志我未读到**，机制未复核。
  可以确定的只是：当时主机侧的 `CONFIG_ESP_HOSTED_HOST_FEAT_NW_SPLIT*` 声明**是开着的**（§9.4），
  因此旧的"没声明能力"解释不成立，这条待办会落到队友/后续轮次。
- 那颗 `c6_hosted_new` 是否带 AP 凭据（我只做了字符串抽查，未见 `Hello_World`），
  以及它 `EH_CP_FEAT_NW_SPLIT_READY` 的实际取值（只能从镜像里 `ehcp_nw_split` 字符串存在、
  以及 11-01 主机确实收到了 DHCP/DNS 事件这两点**间接**推断，未直接验证其构建 sdkconfig）。
