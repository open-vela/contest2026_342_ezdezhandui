# 04 — 处置与验证：把 C6 刷成 network-split 从机固件后，P4 首次真正连上 WiFi

> 执行：lead（主会话）· 时间：**2026-11-05 09:0x（Asia/Shanghai）**
> 授权：用户明确指示「可以通过 usb1 烧录正确的 c6 固件」
> 端口：`/dev/ttyUSB1`（C6 的 CP2102）· 烧录前该口空闲（仅 ttyUSB0 的 P4 控制台由 Tabby 持有）

---

## 0. 结论

**P4 现在能通过 C6 连上 WiFi 了**，且是**数据面级**的联通（不是 11-01 那种只有控制面 + 地址配置）：

```
eh_hosted_nuttx: RPC wifi_set_mode(STA) -> 0            ← 刷之前恒为 12289 (NOT_INIT)
eh_hosted_netdev: scan done, 13 AP(s)
eh_hosted_nuttx: scan_get_ap_num -> 0 count=14
eh_hosted_nuttx: AP[0] ssid="Hello_World_2.4G" rssi=-31 ch=6 auth=3
eh_hosted_nuttx: RPC wifi_set_config("TP-LINK_2.4G_7B6A") -> 0
eh_hosted_nuttx: RPC wifi_connect -> 0
eh_hosted_netdev: station MAC learnt 10:bd:a3:89:70:d0   ← 发送方向修复生效
eh_hosted_nuttx: DhcpDnsStatus iface=0 link=1 dhcp=1 ip=192.168.0.151
eh_hosted_netdev: wlan0 address 192.168.0.151 mask 255.255.255.0 gw 192.168.0.1
```

---

## 1. 飞行前：先证明「刷之前芯片里到底是什么」

在下载模式下先做只读校验（**不写**），把 02 号报告的「高置信推断」升级为 flash 级证明：

```
$ esptool.py --chip esp32c6 -p /dev/ttyUSB1 -b 460800 --before no_reset --after no_reset \
    flash_id
Chip is ESP32-C6FH4 (QFN32) (revision v0.2)
Features: Wi-Fi 6, BT 5 (LE), IEEE802.15.4, Single Core + LP Core, 160MHz, Embedded Flash 4MB
MAC: 10:bd:a3:ff:fe:89:70:d0   BASE MAC: 10:bd:a3:89:70:d0
Detected flash size: 4MB

$ esptool.py ... verify_flash --flash_mode dio --flash_size 4MB \
    0x10000 tools/build_flash/c6_hosted/eh_cp_wifi_sta_esp32c6_sdio.bin
Verifying 0x10b340 (1094464) bytes @ 0x00010000 ...
-- verify OK (digest matched)
```

⇒ **「现在 C6 里跑的是 station 版」从"app 描述符推断"变成"分区逐字节相同"**（消解了 01 号报告的 U-3）。

---

## 2. 写入（用户已人工置 C6 入下载模式：按住 BOOT → 点 RESET → 松开 BOOT）

```
$ bash tools/build_flash/c6_hosted_new/flash.sh /dev/ttyUSB1
0x0     bootloader.bin
0x8000  partition-table.bin
0xd000  ota_data_initial.bin
0x10000 eh_cp_network_split_station.bin
Wrote 1115248 bytes (638024 compressed) at 0x00010000 in 16.0 seconds
Hash of data verified.
Hard resetting via RTS pin...
```

> 附带确认了一个此前未定的事实：**RTS→EN 是接了的**（`Hard resetting via RTS pin` 生效，随后 esptool 再连就报
> `No serial data received` = 芯片已离开下载模式）。之前"无法自动进下载模式"是因为 **DTR→IO9(BOOT) 没接**，
> 与复位通路无关。以后**复位** C6 不必按键，**进下载模式**才需要按键。

## 3. 验证（三层，互相独立）

### 3.1 C6 自报（抓 ttyUSB1，18 s）

```
boot: compile time Oct 28 2026 20:28:17            ← c6_hosted_new/bootloader.bin
app_init: Project name:     eh_cp_network_split_station
app_init: Compile time:     Oct 28 2026 20:29:15
app_init: ELF file SHA256:  9e718d166...
ehcp_core: auto_feat_init_task: found 5 extension descriptor(s)
ehcp_core:   desc[2] name='feat_nw_split' prio=100      ← station 版是 4 个且无此项
nw_split_sta_cp: ESP-Hosted network_split station CP starting...
ehcp_wifi_rpc: === __wrap_esp_wifi_init called ===
ehcp_wifi_rpc: First-time WiFi initialization
ehcp_wifi_rpc: __real_esp_wifi_init returned: 0
wifi:Init static rx buffer num: 20                      ← 与 sdkconfig.esp32c6.sdio:1374 一致
wifi:mode : sta (10:bd:a3:89:70:d0)
```

**`First-time WiFi initialization` + `returned: 0` 就是此前一直缺失的那一步**：station 版从机 app_main
只做 NVS + event loop，射频永不 init；network-split 版从机自己在 `eh_example_sta_start()` 里 init+start。

### 3.2 主机侧（ttyUSB0）

枚举 → `companion connected, transport ready` → `wlan0 registered/up` → `set_mode(STA) -> 0`
→ 扫描 13 AP → `set_config/connect -> 0` → `DhcpDnsStatus ip=192.168.0.151`。

### 3.3 关键回归项

| 项 | 刷之前 | 刷之后 |
|---|---|---|
| `RPC wifi_set_mode(STA)` | **12289 = 0x3001 NOT_INIT**（恒失败） | **0** |
| 扫描 | 恒 12289 | **成功，13~14 个 AP** |
| `Req_WifiStart` | 从未发（怕卡死） | **`resp=yes`** —— 证实它**会返回**，此前"永不应答"的推断不成立 |
| 连接 + DHCP | 恒失败（`rc=-5`） | **`connect -> 0`，IP 192.168.0.151** |
| 11-04 的「卡在 MCUboot 之后」 | 曾复现 | **未复现**（正常进 `NuttShell (NSH)`） |

---

## 4. 仍未解决 / 需注意

1. **传输层的 `no card interrupt within 2000 ms, polling the companion (#N)` 依然在刷** ——
   与 WiFi 失败无关（协议分析队友已判定为独立问题），但它出现过 `bring-up timed out … TRANSPORT_FAILURE`
   并把 C6 复位；本次是靠主机线程自行重启恢复的。**这一层没修**。
2. **从机自己的 auto-connect 会失败**：`eh_example_sta: NO_AP_FOUND — SSID not seen`（重试 1..6/6），
   因为该固件**未内置 SSID/口令**（`strings` 无 `Hello_World`）。**连接由主机下发**，符合 network-split 设计。
3. **主机开机自测连的是开放 AP `TP-LINK_2.4G_7B6A`**（`auth=0`，entry 代码里挑开放 AP 的测试逻辑），
   不是用户的 `Hello_World_2.4G`。要连自己的 AP：
   `deskmate --wifi-connect Hello_World_2.4G 851285812`
4. **回退命令**（如需回到 station 版；同样需人工 BOOT+RESET）：
   `tools/build_flash/c6_hosted/flash_c6.sh /dev/ttyUSB1`

## 5. 文档更正的落地状态

此前 03 号报告提出的 4 处更正**尚未写入用户文档**（本次只做设备处置，未改 docs/）：
① `logs/20261104-deliver-verify/FINDINGS.md` 把 12289 写成 `ESP_ERR_WIFI_STATE` → 应为 `ESP_ERR_WIFI_NOT_INIT`；
② `eh_hosted_nuttx_entry.c:435` 注释把 0x3006 写成 WIFI_MODE → 应为 `ESP_ERR_WIFI_STATE`；
③ `docs/30_实现/连接_WiFi_SDIO.md` §5 对两颗固件角色**命名反转**（`c6_hosted` 才是纯 RPC 从机）；
④ 11-04 那次 network-split 卡住的归因（"主机没声明 nw_split 能力"）不成立——该配置当时已是 1。

## 6. 遵守的边界

- 只对 **C6**（ttyUSB1）做了写入；**未触碰 P4 固件**、未改任何源码或 docs/。
- 未写入 NVS（0x9000）：四个镜像的偏移为 0x0/0x8000/0xd000/0x10000，不覆盖 NVS。
- 读取 P4/C6 控制台一律经 `mcp__tabby__get_terminal_buffer`，未抢占用户的标签页。
