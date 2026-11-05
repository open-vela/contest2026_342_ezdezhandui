# 06 — 更正：panic 的真凶是 NSH 的后台命令重建循环（不是 `cmd_ask`）

> 调查：lead · 2026-11-05 09:37–09:50 · 本文**取代** 05 号报告的结论（05 的 `cmd_ask` 归因**已被实验证伪**）。
> 约束：全程只读设备（tabby 缓冲 + esptool `dump_mem` 读 ROM）；改动只落在两处代码，未动交付树。

---

## 0. 结论

**整机 panic 的真凶：NSH 在重建"后台命令"命令行时，把 `argv[]` 里的一个 NULL 交给了 ROM 的 `strlcat`。**

```
apps/nshlib/nsh_parse.c:599-607（np_bg 分支）与 :2896-2904（pipeline 分支）
      for (ret = 0; ret < argc; ret++)
        {
          strlcat(sh_arg2, argv[ret], CONFIG_NSH_LINELEN);   // ← argv[ret] 未判空
          ...
        }
```

- 这两处**只在 `&`（后台）命令**时执行 ⇒ 解释了为什么同一条命令**确定性**复现、而普通命令不崩。
- `strlcat` 在本镜像里解析到 **ROM**（`4fc00300 A __call_strlcat`）；
- 实测 `argv[3] == NULL` 而 `argc == 4` ⇒ ROM `strlcat` 读源指针拿到地址 0 → Load access fault → PANIC。

## 1. 决定性证据一：读**真实 ROM** 反汇编（推翻两次符号化猜测）

本机只有两套 P4 ROM ELF，且**都不是这颗芯片的 ROM**：

| 文件 | 内嵌 ROM 版本串 |
|---|---|
| `20241011`/`20260528` 的 `esp32p4_rev0_rom.elf` | `esp32p4-20230811` |
| `20260528/esp32p4_rev300_rom.elf` | `esp32p4-eco5-20250430` |
| **本板实测横幅** | **`esp32p4-eco7-20260109`** |

于是直接用 esptool 把 ROM 读出来反汇编（`dump_mem`，两次）：

```
# 0x4fc00300（链接器认作 strlcat 的入口）真实内容：
4fc00300: j 0x4fc1b658          # ← 跳到 0x4fc1b658

# 0x4fc1b658 真实函数体（节选）：
4fc1b658: addi sp,sp,-16
4fc1b65e: add  a3,a0,a2         # a3 = dst + size
4fc1b662: mv   a5,a0            # a5 = dst
4fc1b664: bne  a5,a3,0x4fc1b684
4fc1b668: sub  s0,a5,a0         # s0 = strlen(dst)（按 size 截断扫描）
4fc1b66c: sub  a2,a2,s0         # a2 = size - s0
4fc1b66e: beqz a2,0x4fc1b68e
4fc1b670: mv   a4,a1            # ★ a4 = src（第二个参数）
4fc1b672: li   a0,1
4fc1b674: lbu  a3,0(a4)         # ★★ 崩溃指令：读 src
...
4fc1b692: add  a0,a0,s0         # 返回值 = strlen(dst)+len(src)
```

⇒ 该函数**返回 `dst_len + src_len` 且按 size 截断**——这是 **`strlcat` 语义**，与链接器的绑定一致；
`0x4fc1b674` 正是"读源指针"那一条 ⇒ `MTVAL=0` 意味着 **`src` 就是 NULL**。

> 两次错误的符号化（先 `strncat`、后 `__sfvwrite_r`）都源于 ROM 版本不匹配；**只有把芯片 ROM 真读出来才对得上**——这条方法论值得写进排障文档。

## 2. 决定性证据二：加判空后复现，日志直接说出答案

在 `nsh_parse.c` 两处循环加判空 + 取证日志，重编重烧（两镜像均 `Hash of data verified`）后复现原命令：

```
nsh: bg rebuild hit argv[3]=NULL (argc=4)      ← 判空日志，出现 7 次
nsh: bg rebuild hit argv[3]=NULL (argc=4)
riscv_exception: EXCEPTION: Load access fault. MCAUSE: 00000005, EPC: 40012376, MTVAL: 00000000
```

- ROM `strlcat` 那一处**不再崩**（判空生效）✓
- 但**底层 argv/argc 不一致没有被修**，崩溃只是换到了本地代码：
  `0x40012376` → **`fdlist_get_by_index`（`fs_files.c`）** ⇒ 流程已被污染到文件描述符表。

⇒ **`argv[3]==NULL` 而 `argc==4` 是必须修的那个点**：命令 `deskmate --agent mock --camera &` 有 4 个实参，
解析却给出 `argv[0..2]` 有效、`argv[3]=NULL`，而 `argc` 仍计 4。**谁吞掉了 `--camera`、`argc` 何时被多计，尚未坐实**（见 §4）。

## 3. 本次改动（两处止血，非最终修复）

| 文件 | 改动 |
|---|---|
| `/home/ez/share/openvela/apps/nshlib/nsh_parse.c`（工作区，上游文件） | 两处后台/管道重建循环加 `argv[ret]==NULL` 判空 + `nsh_error` 取证日志 |
| `app/ai_agent/src/channels/nsh_commands.c`（交付树 + 工作区副本） | `cmd_ask` 判空 + `cli_thread` 每次 `tokenise` 前 `memset(argv,0,...)` + `ask` 分支逐参数日志（**事后证明与本 panic 无关**，但属正当加固） |

产物：`cmake_out/esp32p4-function-ev-board_nsh/nuttx.bin`（2026-11-05 09:47，1,830,912 B），已烧入。
备份：`packages/ai_agent/src/channels/nsh_commands.c.bak-20261105-0936`。

## 4. 还没坐实的一环（下一步的最小实验）

**为什么 `--camera` 变成 NULL、`argc` 为何计成 4。** 建议在 `nsh_parse_command()` 构造完 argv 之后
（`argv[argc] = NULL;` 之后，nsh_parse.c 约 :2502 与 :2978）加一段**全量 dump**：

```c
for (i = 0; i <= argc; i++)
  syslog(LOG_ERR, "nsh: parsed argv[%d]=%p \"%s\" (argc=%d)\n",
         i, (FAR void *)argv[i], argv[i] ? argv[i] : "(null)", argc);
```

一次复现即可区分三种可能：
1. `nsh_argument()` 对 `--camera` 返回了 NULL（解析器把某个 token 当成终止）；
2. `&` 的处理路径把 `argc--` 与 `argv[argc]=NULL` 的顺序/次数弄错（例如同时走了 `np_bg` 与 pipeline 两条计数）；
3. `argv[]` 本身被写坏（与 §2 的 `fdlist` 崩溃同源的内存损坏）。

## 5. 与 WiFi/0.0.0.0 的关系（闭环）

任何会走进这条后台重建路径的命令，都可能 panic → 重启 → MCUboot 经 GPIO54 复位 C6 → WiFi 关联丢失 →
`wlan0` 被 `DhcpDnsStatus(ip=0.0.0.0)` 清零 → 界面 0.0.0.0。
**也就是说：修 WiFi 的稳定性，绕不开先把这条 argv/argc 缺陷修掉。**

---

## 6. 收口（2026-11-05 09:56 完成）：真因、修复与验证（本节取代 §4）

### 6.1 真因：`nsh_execute()` 后台分支的**自递归 + argc 差一**

```c
apps/nshlib/nsh_parse.c  nsh_execute()
  ret = nsh_fileapp(vtbl, argv[0], argv, param);   // ① 先试"文件程序"；deskmate 是 builtin → 返回 -1
  if (ret >= 0) return ...;
  if (vtbl->np.np_bg) {                            // ② 后台命令才走这里
      ... sh_arg2 = join(argv[0..argc-1]) ...      //    拼出 "deskmate --agent mock --camera"
      sh_argv[0]="sh"; sh_argv[1]="-c"; sh_argv[2]=sh_arg2; sh_argv[3]=NULL;
      ret = nsh_execute(vtbl, 4, sh_argv, param);  // ③ ★ argc 传 4（应为 3）、np_bg 仍为真 → 递归
  }
```

内层调用再次进入 ②，拼接循环 `ret = 0..3` 走到 **`argv[3]`（NULL 终止符）** → `strlcat(sh_arg2, NULL, 80)`
→ ROM strlcat 读源指针拿到地址 0 → **Load access fault → PANIC**。
**与实测日志 `bg rebuild hit argv[3]=NULL (argc=4)` 逐字吻合。**

三个"为什么只有它崩"的解释：
- **为什么只有 `&` 崩**：这条分支只对 `np_bg` 为真的命令执行；
- **为什么 `deskmate` 崩**：它是 builtin（`builtin_list.c.o`），① 的 `nsh_fileapp` 必然失败，才会落到 ②；
- **为什么普通命令不崩**：无 `&` 时 `np_bg` 为假，根本不进这条分支。

### 6.2 本配置下这条路本就走不通

`board/.../configs/nsh/defconfig`：`CONFIG_NSH_BUILTIN_AS_COMMAND=y`（且无 `CONFIG_NSH_FILE_APPS`）
⇒ `nsh_parse.c:554` 的 `#if` 整块不编译 ⇒ **`nsh_fileapp()` / `nsh_builtin()` 根本没有编入**（链接器实证：
`undefined reference to nsh_fileapp / nsh_builtin`）。
也就是说：即使修好 argc 与递归，"经 `sh -c` 转派后台" 在本板上也无 `sh` 可用。

### 6.3 修复

`sh_argv` 构造之后不再递归，改为**明确报错**（`nsh_error` + `return ERROR`），并保留 §3 的判空止血：

```
nsh: cannot run "deskmate" in the background in this build (no sh/nsh_builtin); run it in the foreground
```

### 6.4 验证（重编 → 烧录 → 复现，均在真机）

| 步骤 | 结果 |
|---|---|
| `ninja` | `exit=0`；`nuttx.bin` 1,830,912 B @ 09:56 |
| `usb_stable.sh` 烧录 | `[flash] OK (try 1)`，两镜像 `Hash of data verified` |
| **原必崩命令** `deskmate --agent mock --camera &` | ✅ **不再 panic**，打印上面那行错误后回到 `nsh>`；WiFi 栈继续正常（`scan done, 13 AP(s)`、`set_mode(STA) -> 0`） |
| **前台方式** `deskmate --agent mock --camera --exit-after 6` | ✅ 完整启动：`SC2336: 1280x720 verify 8/8 match`、`CSI ctlr start rc=0`、`[dm] ui: home page built`、`DM-READY ... camera=v4l2 page=home`、`DM-EXIT run result=0`，干净回到 `nsh>` |

### 6.5 结论与使用建议

- **panic 已修**：`&` 不再是"整机重启 + WiFi 掉线"的开关，而是给出一句可执行的提示。
- **本板要跑 DeskMate**：用**前台**方式（`deskmate --agent mock --camera`；调试可加 `--exit-after N`）。
  内建命令的后台执行在本配置下不受支持 —— 这是 openvela NSH 的配置/实现限制，需要上游层面决定
  （要么开 `CONFIG_NSH_BUILTIN_APPS` 且去掉 `BUILTIN_AS_COMMAND` 以启用 `nsh_builtin`，
  要么给 NSH 加一条"内建命令直接 `task_create` 后台化"的正路）。
- **对 WiFi 的意义**：这条缺陷此前是"随手上一条 `&` 命令就把 WiFi 打断"的隐形来源；
  修掉后，`deskmate ... &` 不再触发 panic → 不再重启 → 不再经 GPIO54 复位 C6。

