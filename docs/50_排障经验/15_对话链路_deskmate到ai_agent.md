# 15_对话链路：deskmate 到后台 ai_agent

> 写于 2026-11-06。**"deskmate 一直显示离线 / 发出去没反应 / 只回一句
> 『让我查一下...』就没下文"** 读这篇。

## 一、链路长什么样

```
桌面 UI (dm_ui chat page)
   │  dm_agent_send()
   ▼
dm_agent.c  ── WebSocket 客户端 ──► 127.0.0.1:28789
                                      │
                              ai_agent 任务（同进程，--agent-spawn）
                                      │  ws_server.c 解帧
                                      ▼  message_bus_push_inbound()
                              agent_loop（同进程）
                                      │
                                      ├─► LLM (TLS HTTPS, amr-link.open-design.ai:443)
                                      └─► 本地工具 (get_weather 等)
                                      │
                              回帧 {"type":"response"}
                                      ▼
                              dm_agent_poll() → 聊天页气泡
```

**关键点：ai_agent 是同进程任务，不是另一台机器上的服务。**
所以 `127.0.0.1` 是对的目标，前提是本板 `CONFIG_NET_LOOPBACK=y`（本板已开）。

## 二、这条链上一共六个断点，逐个说

### 断点 1：agent 根本没被拉起来

症状：`[dm] agent: connect(127.0.0.1:28789) timed out or refused`，永远离线。

`deskmate` 交互式启动时靠 `DM_AGENT_AUTOSPAWN` 把 ai_agent 起成同进程任务。
这个开关来自 Kconfig：

```c
config EXAMPLES_DESKMATE_AGENT_AUTOSPAWN
	bool "Start the built-in ai_agent at startup"
	default y
	depends on EXAMPLES_AI_AGENT_VELA
```

**坑：`defconfig` 里有 `CONFIG_EXAMPLES_DESKMATE_AGENT_AUTOSPAWN=y`，
但构建用的 `.config` 里没有。**

原因：那个 `defconfig` 是在 `.config` 生成**之后**才被编辑的
（`Kconfig` 和 `defconfig` 的时间戳都比 `.config` 新）。构建流程不会因为
`defconfig` 变了就重新生成 `.config`，而缺一个 CONFIG 符号
**不会有任何报错** —— 只表现为"功能没生效"。

确认命令：

```bash
grep -c AGENT_AUTOSPAWN cmake_out/<board>/.config        # 0 = 没生效
grep -c AGENT_AUTOSPAWN cmake_out/<board>/include/nuttx/config.h
```

修法：把缺的符号补进 `.config`，删掉 `include/nuttx/config.h` 重新 configure。

### 断点 2：eth0 开机没有 IP

症状：ws 已经 `online`，但发消息后 LLM 永远失败。

```
[llm] OpenAI API with tools ...
[vela_tls] net_connect amr-link.open-design.ai:443 ret=0x52
[agent] LLM call failed (iter 0)
```

`-ret = 0x52` 就是 **`MBEDTLS_ERR_NET_UNKNOWN_HOST`** —— 这是 **DNS 解析失败**，
不是 TLS 问题、也不是防火墙问题。被这个错误码误导过一次。

根因：`ifconfig` 显示 `eth0 ... inet addr:0.0.0.0`。板子开机时没人给网口
跑 DHCP（`dhcpc` 只是库，没有对应的 NSH 命令；`netinit` 也没在这条路径上）。

修法：deskmate 启动时自己把有线口拉起来并请求 DHCP：

```c
dm_eth_autostart();    /* 找第一个非 lo、非 wireless 的接口 → ifup → DHCP */
dm_wlan_autostart(DM_WLAN_SSID, DM_WLAN_PSK);
```

有线优先是对的：eth0 插上就通，wlan0 还要先关联 AP（而且默认 SSID 是空的）。

### 断点 3：DHCP 状态只有一个全局变量

两个口都请求 DHCP 时，后发起的会把前一个的结果覆盖掉 —— 网络页显示"没地址"
而实际是通的，或者第二个请求看到 `state==1` 直接返回 0 假装已发起。

修法：状态按接口记账（`dm_net_if_dhcp_status_of()`），最多 4 个槽。

### 断点 4：ws 服务起得太晚（20 秒）

症状：`agent=offline` 持续 20 s，之后突然 online。

`ai_agent` 的 `network_watch_task()` 原来第一行是 `network_wifi_reconnect()` ——
它会同步等 WiFi 关联。本板没配 SSID 时这一步要耗很久，而 `ws_server_start()`
被排在它**后面**，于是端口 28789 在这 20 秒里根本没在监听。

```
端口没开 → deskmate 反复 connect refused → 看起来像"连不上 agent"
```

修法：`agent_loop_start()` + `ws_server_start()` 提到最前面，WiFi 重连挪到后面。

**ws 只服务本机回环与本网段，和 WAN 完全无关**，没有任何理由等网络。

### 断点 5：系统时钟是 1970，TLS 必然失败一次

```
[vela_tls] Handshake start: Host=amr-link.open-design.ai, UNIX=53
[vela_tls] Clock too old, forcing to 2026
[vela_tls] ssl_handshake ret=-0x004e
[vela_tls] POST ... failed (-2) — retrying on a fresh connection
[vela_tls] Handshake start: ..., UNIX=1772275215
[vela_tls] Handshake OK: TLSv1.2
```

本板没有 RTC 电池，上电后 `CLOCK_REALTIME` 从 0 起算。原代码在
`ctr_drbg_seed` **之后**才把时钟拨到 2026，但 mbedtls 的上下文已经按旧时间
建好了 —— 第一次握手仍然失败，要等整个 POST 失败、重建连接才成功。

代价：每条首问多 ~13 s（实测 `llm_ms` 27.7 s，其中一半是这一次白跑）。

修法：`vela_tls_ensure_clock()` 提到函数最开头，并在 agent 启动时也调一次，
让**第一次**握手就带着正确时间。改后 `llm_ms` 降到 13.6 s。

### 断点 6：探针等待期间没人发起重连

重连逻辑挂在 `dm_agent_poll()` 里（5 s 节流）。而 `--say` / `--selftest`
这类探针在**主线程**里 `usleep` 等状态，等待期间没有 poll，也就没人去连 ——
一路空等到 20 s 超时才在 `dm_agent_send()` 里顺带连上。

修法：新增 `dm_agent_retry_now()`，探针每 500 ms 主动踢一次。

## 三、改后实测

| 指标 | 改前 | 改后 |
|---|---|---|
| `DM-PROBE state=online` | **20000 ms** | **500 ms** |
| LLM 单次 `llm_ms` | 27700 ms | **13600 ms** |
| TLS 首次握手 | 必失败一次 | 一次成功 |
| eth0 地址 | 0.0.0.0（手动 ifconfig） | **DHCP 自动 192.168.1.110** |
| 工具调用 | — | `get_weather` 正常触发 |
| 对话 | 只有兜底文案 | **真模型回复（64~261 字节）** |

## 四、复现与验证

```bash
# 1) 确认 agent 自启开关真的进了 .config
grep -c AGENT_AUTOSPAWN cmake_out/<board>/include/nuttx/config.h   # 应为 1

# 2) 一次性端到端探针（走和界面完全相同的路径）
deskmate --say 你好

# 期待看到：
#   [dm] eth: dhcp on eth0 requested rc=0
#   [ws] client connected / Client connected
#   DM-PROBE state=online after 500ms
#   [vela_tls] Handshake OK: TLSv1.2
#   [llm] Response: NNN bytes text, finish=end_turn
#   [agent] Dispatching response → websocket:deskmate-ui

# 3) 网络层单独排查（绕开 deskmate）
ifconfig eth0                       # 必须有地址
ping -c 2 192.168.1.1               # 网关
ping -c 1 amr-link.open-design.ai   # DNS + 出网
```

## 五、容易误判的地方

| 看到的现象 | 直觉判断 | 实际原因 |
|---|---|---|
| `connect ... timed out or refused` | agent 崩了 | agent 还没监听（起得晚），或压根没自启 |
| LLM 失败 + `ret=0x52` | TLS/证书问题 | **DNS 解析失败**（网口没地址） |
| 只回"让我查一下..."就没下文 | 对话断了 | 那是**即时 ack**，真回复还在路上（10~20 s） |
| 网络页显示"无地址"但能上网 | 驱动问题 | DHCP 状态被另一个口的失败覆盖（断点 3） |
| `defconfig` 改了没效果 | 代码没编进去 | `.config` 没重新生成，缺的符号不会报错 |
