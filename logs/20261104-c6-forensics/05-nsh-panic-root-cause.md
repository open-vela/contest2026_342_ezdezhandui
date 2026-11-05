> ⚠️ **本报告的结论已被实验证伪，请看 [06](06-nsh-bg-rebuild-null-argv.md)。**
> 我把崩溃点归到 `cmd_ask` 是错的：加了判空日志后复现，`cmd_ask` 的日志**一次都没出现**
> （说明它根本没被执行），而 NSH 自己的后台命令重建循环把 `argv[3]==NULL` 交给了 ROM `strlcat`。
> 错误的根源是**用错了 ROM 符号文件**（本机只有 base 与 eco5 两套，而本板 ROM 是 `esp32p4-eco7-20260109`）——
> 把芯片 ROM 真读出来反汇编后，该函数是 `strlcat` 而非 `strncat`。本文其余证据（异常码、
> 触发命令、任务归属、与 04 号报告的因果闭环）仍然成立。

# 05 — `deskmate` 启动引发的整机 panic：根因定位（结论已作废，见文首更正）


> 调查：lead（主会话）· 2026-11-05 09:1x–09:2x · 方法：`systematic-debugging`（先根因、后修复）
> 只读取证：未改任何源码、未烧录。符号化使用 `/home/ez/share/openvela/cmake_out/esp32p4-function-ev-board_nsh/nuttx`
> （内嵌版本串 `76354c63785-dirty Nov 4 2026 18:37:11`，与崩溃现场打印的版本**逐字一致**）。

---

## 0. 结论

**整机 panic 的直接原因：`strncat()` 被传入了一个 NULL 源指针，而 `strncat` 在本镜像里链接到的是 ESP32-P4 的 ROM 实现。**

崩溃链条：

1. 在 NSH 提示符下执行 `deskmate --agent mock --camera &`；
2. DeskMate 的 agent bridge 拉起 AI agent 的 CLI 通道（`cli_thread`，`agent_cli` 任务）；
3. CLI 解析一行后走 `ask` 分支 → 内联的 `cmd_ask()` 用 `strncat` 拼接参数
   （`app/ai_agent/src/channels/nsh_commands.c:588`）：
   `strncat(content, argv[i], sizeof(content) - strlen(content) - 1)`
4. 其中 **`argv[i] == NULL`**；
5. 镜像把 `strncat` 解析到 **ROM**（`4fc00310 A __call_strncat` → 真身 `0x4fc1b662`）；
6. ROM `strncat` 在 `+0x12` 处读源指针 → 地址 0 → **Load access fault**；
7. `riscv_exception → PANIC → 重启`；MCUboot 启动时经 GPIO54 给 C6 断电重启
   （`esp_hosted_sdio: companion reset: GPIO54 held low 500 ms`）→ **WiFi 关联随之丢失**
   → wlan0 地址被 `DhcpDnsStatus(ip=0.0.0.0)` 清零 → 界面显示 0.0.0.0（见 04 号报告的时间线）。

## 1. 现场原始证据（ttyUSB0 控制台缓冲 13394–13446 行）

```
eh_hosted_netdev: wlan0 address 192.168.0.151 mask 255.255.255.0 gw 192.168.0.1
deskmate --agent mock --camera &
riscv_exception: EXCEPTION: Load access fault. MCAUSE: 00000005, EPC: 4fc1b674, MTVAL: 00000000
riscv_exception: PANIC!!! Exception = 00000005
dump_assert_info: Current Version: NuttX  0.0.0 76354c63785-dirty Nov  4 2026 18:37:11 risc-v
dump_assert_info: Assertion failed panic: at file: :0 task: nsh_main process: nsh_main 0x4002892a
up_dump_register: PC: 40020d80          (= _assert)
... 任务表：nsh_main(Running) / eh_hosted ×6 / hpwork / lpwork / esp_timer / emac_rx（**无 agent_cli**）
```

## 2. 双侧反汇编（决定性证据）

**调用侧**：全镜像只有两处 `jalr → 4fc00310 <strncat>`，都在 `cli_thread` 内（`cmd_ask` 被内联）：

```
4006eb8c: lw   a1,4(s1)        # a1 = argv[i]
4006eb8e: addi a0,sp,560       # a0 = content（栈数组）
4006eb96: jalr 4fc00288 <strlen>
4006eb9a: lw   a1,12(sp)       # 恢复 a1 = argv[i]
4006eb9c: sub  a2,s6,a0        # a2 = 255 - strlen(content)
4006eba6: jalr 4fc00310 <strncat>   # ← 对应源码 588 行
4006ebb4: jalr 4fc00288 <strlen>
4006ebbc: addi a1,s11,-1148    # a1 = " " 字面量（非 NULL）
4006ebc6: jalr 4fc00310 <strncat>   # ← 对应源码 590 行
```

**被调侧**：ROM `strncat` 真身（rev300 ROM ELF）：

```
4fc1b662 <strncat>:
  4fc1b662: mv   a5,a0          # a5 = dest
  4fc1b664: lbu  a4,0(a5)       # 先扫 dest 末尾 —— 这一步**成功**（content 有效）
  ...
  4fc1b672: addi a1,a1,1
  4fc1b674: lbu  a4,-1(a1)      # ★ 崩溃点：读源指针（a1 自增后读 -1）
```

`EPC = 0x4fc1b674` 精确落在 `strncat+0x12`；`MTVAL = 0` ⇒ 该 `lbu` 的有效地址为 0
⇒ **原始 `a1`（源指针）就是 NULL**。dest 扫描先执行且未崩 ⇒ `content` 是有效缓冲区。

## 3. 被证伪的两个假设（避免后人重走）

| 假设 | 检验 | 结果 |
|---|---|---|
| **A. 链接用的 ROM 地址表与这颗芯片不匹配**（IDF 有 `esp32p4.rom.ld` 与 `esp32p4.rom.eco5.ld` 两套，235/450 个符号地址不同） | 用 `nm nuttx` 取 587 个 ROM 绝对符号，与两表逐项比对 | **证伪**：匹配 eco5 表 **208** 项、匹配 base 表 **0** 项、两边都不匹配 **0** 项 ⇒ 链接用的是正确那套 |
| **B. `0x4fc00310` 不是 `strncat`（符号错位）** | 用 `rev0` 与 `rev300` 两份 ROM ELF 交叉符号化 | **证伪**：两份都报 `__call_strncat` ⇒ ROM 绑定正确 |
| C. `tokenise()` 会返回大于实际写入项数的 `argc` | 读源码 `nsh_commands.c:83-92` | **证伪**：`while (tok && argc < max_argc)` + 只写非 NULL 的 `tok`，`argv[0..argc-1]` 必然全有效 |

## 4. 仍未确定的一环（诚实标注）

**`argv[i]` 究竟怎么变成 NULL 的**，尚未坐实。已知：
- `cli_thread` 的 `char* argv[MAX_ARGS];`（`nsh_commands.c:933`）**未初始化**；
- `cmd_ask` 对 `argv[i]` **无 NULL 检查**（588 行）；
- `cli_read_line()`（`:893`）**故意直接读 fd 0 原始字节**，注释写明理由是「NSH 把控制台置为 raw 模式」——
  也就是说 **agent CLI 与 NSH 同时读同一个控制台**，两边都可能拿到半行/交错的行；
- 崩溃时任务表里**没有 `agent_cli` 任务**，panic 归因到 `nsh_main`。

最可能的两个方向（待一次带日志的复现来区分）：
1. **同控制台双读者**：agent CLI 与 NSH 抢同一字节流，CLI 拿到一行残缺内容，`argc` 与实际 token 不一致；
2. **分发处 argc/argv 不同源**：`ask` 分支（`:1027-1028`）拿到的 `argc` 与 `argv[]` 的内容不匹配（例如经由另一条 NSH builtin 路径进入 `cmd_ask`）。

## 5. 建议的最小实验（待用户决定，不擅自动手）

1. **防御性加固**（3 行，零风险）：`cmd_ask` 的循环里 `if (argv[i] == NULL) break;`；
   `cli_thread` 在 `tokenise` 前 `memset(argv, 0, sizeof(argv));`；
2. **加一条证据日志**：进入 `ask` 分支时打印 `argc` 与每个 `argv[i]` 的指针值；
3. 复现命令仍是 `deskmate --agent mock --camera &`；若崩溃点/日志指向方向 1，
   则真正的修法是**不要让 agent CLI 与 NSH 共读 fd 0**（设计层问题）。

## 6. 与 04 号报告的闭环

本次 panic 正是「wlan0 变成 0.0.0.0」的**直接触发者**：panic → 重启 → MCUboot 复位 C6 →
WiFi 关联丢失 → 地址归零。**修 WiFi 的稳定性，绕不开先修这个 panic。**
