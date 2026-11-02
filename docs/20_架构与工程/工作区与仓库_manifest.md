# 工作区与仓库（manifest）

openvela 是 `repo` 多仓工作区：`nuttx`、`apps`、`vendor/espressif`、`packages/ai_agent` 等各是一个 git 仓，
靠 `.repo/manifests/` 下的 xml 组合。本项目的交付是一份**自己的 manifest**，把作品仓的文件映射回工作区位置。

## 本项目的交付映射

- 作品仓 `contest2026_342_ezdezhandui` 作为 self-ref 项目出现在 manifest 里；
- 仓内 4 棵文件树映射回工作区：

| 作品仓内 | 工作区位置 |
|---|---|
| `nuttx/**` | `nuttx/**` |
| `board/esp32p4/**` | `vendor/espressif/boards/esp32p4/**` |
| `app/apps/**` | `apps/**` |
| `app/ai_agent/**` | `packages/ai_agent/**` |

- 映射由 `<copyfile src="..." dest="..."/>` 表达（`src` 相对作品仓、`dest` 相对工作区根）。

### 混合策略（2026-11-01 起）

`<copyfile>` **只支持文件**（repo 工具对目录直接报错），所以：

- **整棵子树都属于本项目**（该目录在工作区里 0 个 tracked 文件）、文件数 ≥ 20、且
  `src`/`dest` 去前缀后逐文件一致 → 用**一条 `<linkfile>`**（symlink）代替逐文件条目；
- 其余仍用 `<copyfile>` 逐文件声明。

当前结果（2026-11-02 第二次收拢后）：**5 条 linkfile 替代 675 条 copyfile**，
清单为 **53 条 copyfile + 5 条 linkfile**：

| linkfile | 文件数 | 说明 |
|---|---|---|
| `nuttx/arch/risc-v/src/esp32p4` → 同名 | 528 | 芯片目录（含 `espressif/` 整层） |
| `board/esp32p4` → `vendor/espressif/boards/esp32p4` | 135 | **整棵 P4 板树**（function-ev-board + tab5 + pico + common）；该目录在上游 `vendor/espressif` 里 tracked=0，本来就全归本项目 |
| `app/apps/examples/audiotest` → `apps/examples/audiotest` | 5 | 音频自检应用 |
| `app/apps/system/v4l2ctl` → `apps/system/v4l2ctl` | 4 | 摄像头枚举/抓帧工具 |
| `nuttx/arch/risc-v/include/esp32p4` → 同名 | 3 | 芯片 include（chip.h / gpio_sig_map.h / irq.h） |

**linkfile 的判定标准（2026-11-02 收紧后）**：目标目录在工作区里 **0 个 tracked 文件**
（整棵都归本项目），且"会被符号链接遮住、但不在交付里"的文件集合**为空**——
判定用 `os.walk(followlinks=True)` 取集合相等（噪声 `.depend`/`Make.dep`/`CMakeCache.txt`
等忽略），并剪掉构建期链接（`vendor/espressif/boards/esp32p4/common/board`）。
默认阈值 `--linkfile-threshold 3`。

> 为什么 apps 侧只有两条：其余 48 个 app 文件是**改在已有上游目录里的文件**
> （`crypto/mbedtls`、`netutils/netinit`、`system/nxplayer`、`examples/camera`…），
> 把目录链成符号链接会遮住上游的同目录文件，只能逐文件交付。
> `app/ai_agent` 同理（`packages/ai_agent` 共 139 文件，我们只改 16 个）。

关闭方式：`gen_manifest_copyfiles.py --no-linkfile`（回落为全 copyfile）；阈值可调
`--linkfile-threshold N`。模拟部署校验：copyfile 内容全一致、linkfile 5 条解析正确。

## 两个必须知道的限制

1. **`<copyfile>` 不支持目录**：repo 工具对目录会直接报错（"copying from directory not supported"），
   所以"整文件夹替换"要靠逐个文件条目，或改用 `<linkfile>`（symlink，可指向目录）。
2. **manifest 不能表达"删除"**：交付只能新增/覆盖文件。所以 2026-11-02 的搬迁在评审端的
   表现是：基线的 `common/espressif/**` 仍在原处，但**不被任何构建文件引用**（死文件）；
   我们交付的是新位置 `arch/risc-v/src/esp32p4/espressif/**` 的整层。工作区里那批
   tracked 文件显示为 "删除"，属于"搬走"，不是"交付缺文件"。

## 只读保护区

另一条对话正在开发模拟器 UI，以下路径**不交付、不同步**：

```
apps/examples/deskmate
nuttx/drivers/net/wifi_sim.c
nuttx/boards/sim/sim/sim/configs/deskmate
vendor/openvela
```

对账与受控同步见 `tools/engineering/check_consistency.py` 与 `sync_and_restore.sh`。

## 通用层零写入：`arch/risc-v/src/common/` 只剩 1 个文件（2026-11-02）

**背景**：上一版做法是"公共层最小交付集"—— 从 113 个文件里挑出真正参与构建的 45 个交付，
其余留给评审端基线。但这样交付仍然**写进通用层**：Espressif 内容占着
`arch/risc-v/src/common/espressif/**`，通用部分被芯片内容污染。

**现在的做法**：整层搬进芯片目录，通用层不再被写入任何本项目文件。

| | 迁移前 | 迁移后（2026-11-02） |
|---|---|---|
| Espressif 层位置 | `nuttx/arch/risc-v/src/common/espressif/` | `nuttx/arch/risc-v/src/esp32p4/espressif/` |
| Espressif 层交付 | 45 条 `<copyfile>`（写在通用层） | 随 `esp32p4` 的 **1 条 `<linkfile>`**（528 文件） |
| 通用层交付条目 | 48 | **1**（仅 `common/CMakeLists.txt`） |

**唯一保留的通用层交付：`common/CMakeLists.txt`**（把 `riscv_mtimer.c` 改成
`if(CONFIG_ONESHOT)` 条件编译）。删不掉的理由是实测的：

1. 基线**无条件**编译 `riscv_mtimer.c`，而本板配置（`CONFIG_ONESHOT_COUNT=n`）下它编译必失败：
   `struct oneshot_operations_s` 没有 clkcnt 成员，实测报
   `'const struct oneshot_operations_s' has no member named 'start_absolute'`；
2. CMake 侧无法从芯片目录摘掉通用层的源文件 —— `arch/risc-v/src/CMakeLists.txt` 先
   `add_subdirectory(chip)` 再 `add_subdirectory(common)`，而源文件属性按目录隔离：
   `HEADER_FILE_ONLY`、`DIRECTORY`、`TARGET_DIRECTORY`、`set_property(TARGET ... SOURCES)`
   四组在 CMake 3.22 上实测全部无效；
3. Make 路径**可以**从芯片侧摘除，所以 `common/Make.defs` 已不再交付：
   `esp32p4/Make.defs` 里用 `CMN_CSRCS := $(filter-out riscv_mtimer.c,$(CMN_CSRCS))`
   （与该层既有的 `riscv_vectors.S` filter-out 同一套路）。

断言：`gen_manifest_copyfiles.py --assert-shared-max 1`（默认）与
`check_consistency.py` 的"通用层交付条目 ≤ 1"。

**另外两处通用层改动的去向**

| 原通用层改动 | 现在在哪 |
|---|---|
| `common/riscv_doirq.c` 的早启动 `g_running_task == NULL` 守卫 | 下沉到 `esp32p4/espressif/esp_irq.c` 的 `riscv_dispatch_irq()`（P4 全部 trap 的唯一入口：`esp_vectors.S → exception_common → riscv_dispatch_irq`），通用文件回到基线 |
| `common/Make.defs` 的 `riscv_mtimer` 条件编译 | 由芯片目录的 `filter-out` 取代 |

**回退**：把 `esp32p4/espressif/` 整目录搬回 `common/espressif/`，用
`git -C nuttx checkout HEAD -- arch/risc-v/src/common/{Make.defs,riscv_doirq.c}` 恢复被删的
通用层文件并重新部署旧副本，最后重跑 `gen_manifest_copyfiles.py`。

## 清空验证（clean-room，2026-11-01）

做法：把工作区**强制还原成评审端基线**（tracked 全部 `git checkout HEAD --`、untracked 全部删除、
linkfile 子树整棵移走），再**只按 manifest 部署**（`<copyfile>` 复制 + `<linkfile>` 建符号链接），
得到与评审端 `repo sync` 后一致的树。结果与经验：

- 部署 53 条 copyfile + 5 条 linkfile 后，工作区 `nuttx/arch/risc-v/src/esp32p4` 等五处变为
  指向作品仓的符号链接（与评审端一致），`check_consistency.py` ✅ 一致；
- 坑 1：**不要复用旧构建目录**再重跑 CMake —— 重跑时 `NUTTX_DEFCONFIG` 环境变量丢失，会在
  `cmake/nuttx_kconfig.cmake:48` 报 `string sub-command REGEX ... needs at least 5 arguments`。
  评审端是全新 configure，不会踩到；本机要重配就换一个全新构建目录；
- 坑 2：**全新构建目录需要重新 clone `esp-hal-3rdparty`（约 700 MB）**。本机今日实测
  ~30 MB / 4 分钟，无法在本轮内完成整个 clean-room 构建；hal 缓存已放回
  `cmake_out/esp32p4-function-ev-board_nsh/` 供下次配置复用；
- 内容层面的"编译 + 烧录 + 真机三项证据"已由共享层最小集实验完成（见上一节，证据目录
  `logs/20261101-211034_shared-minimal`），那次的树 = 交付内容（其余文件为基线版本）。

## clean-room 通过 + 补上的一个真实缺口（2026-11-02）

**结果**：在"评审端状态"的树上（工作区已对齐到交付内容）从零 configure 构建 → 烧录 → 真机三项证据全部复现：

```
MCUboot → NuttShell (NSH)
connected to "Hello_World_2.4G" rssi=-50 ch=6
DhcpDnsStatus iface=0 link=1 dhcp=1 ip=192.168.1.9
wlan0 address 192.168.1.9 mask 255.255.255.0 gw 192.168.1.1
```
证据目录：`logs/20261102-090754_cleanroom`（固件 1830912 字节，烧录两镜像 hash 校验通过）。

**排查过程中发现并修掉的一个真实缺口**：NuttX 架构层用 `arch/board/board.h` 找板级头文件，
而 `<build>/include/arch/board` 指向 `${NUTTX_DIR}/arch/risc-v/include/board` —— 这个链接**只有 make 流程
（`tools/configure.sh`）会创建，CMake 流程不会**。缺了它，`esp_idle.c` / `esp_i2c.c` / `esp_timerisr.c`
等会因为找不到 `arch/board/board.h` 编译失败（评审端全新树必踩）。

修法（改动落在我们自己的板级 CMakeLists，符合"公共层不放改动"的原则）：

```cmake
set(_board_inc  "${CMAKE_CURRENT_LIST_DIR}/include")
set(_board_link "${NUTTX_DIR}/arch/risc-v/include/board")
if(EXISTS "${_board_inc}" AND NOT EXISTS "${_board_link}")
  file(CREATE_LINK "${_board_inc}" "${_board_link}" COPY_ON_ERROR SYMBOLIC)
endif()
```
幂等：已存在就跳过。

**另一个坑（工作区脏数据）**：`nuttx/arch/risc-v/include/board` 曾经指向 **esp32c6-devkitc**（早期 C6 交叉
构建实验的残留），会让 P4 构建拿到错误的板级头文件（表现为 `BOARD_MIPI_DSI_* undeclared`）。重配前请确认
该链接指向 P4 板级 include。

## clean-room 复验：通用层零写入之后（2026-11-02 12:41）

搬迁（espressif 层搬进芯片目录 + 守卫下沉 + MCUboot 工作树移出交付树）之后，再走一次
"评审端状态"的完整链路：

**做法**：把工作区共享层还原成评审端形态（`common/espressif/**` 回基线、`Make.defs` 与
`riscv_doirq.c` 回基线、`CMakeLists.txt` 保留我们的版本），然后用**全新构建目录**
（`cmake_out/esp32p4-verify-move`，只预置 700 MB 的 `esp-hal-3rdparty` 缓存）从零 configure + build。

| 检查项 | 结果 |
|---|---|
| 全新 configure + build | ✅ 通过（`#### build completed successfully`） |
| 固件 | `nuttx.bin` **1,830,912 字节**，与搬迁前逐字节同尺寸；`cmp -l` 只差 43 字节，全部落在启动时间戳字符串（`Nov  2 2026 12:41:08` vs `09:04:28`）⇒ 功能等价 |
| 构建图 | 老路径源文件 **0** 处；新路径 `esp32p4/espressif/*` **37** 处；`riscv_mtimer.c` **未编译** |
| 烧录 | `usb_stable.sh` 两镜像 hash 校验通过（MCUboot 24608 B 从搬迁后的 `nuttx/bootloader-esp32p4` 重建） |
| 真机三项 | ✅ 见下（证据目录 `logs/20261102-124236_espressif-move`） |
| IRQ 守卫回归 | ✅ NSH 里执行 `exit`（8510a00 的复现场景）后串口**继续**输出 19 组 WiFi/SDIO 心跳 ⇒ 未复现"整机失聪"（同目录 `irq_guard_regression.txt`） |
| `CONFIG_ESPRESSIF_HOSTED_OFFICIAL` | ✅ 本次 nsh 构建已覆盖（`configs/nsh/defconfig` 里 `ESPRESSIF_HOSTED_SDIO=y` + `ESPRESSIF_HOSTED_OFFICIAL=y`，构建日志里 `esp_hosted/official/port/...` 全部编过） |
| **verify-drivers（全驱动）arch 目标** | ✅ 313 个 edge 全编过，`libarch.a` 3,552,682 B / 374 个成员；覆盖 `esp_adc/esp_csi/esp_dedic_gpio/esp_emac/esp_i2s/esp_lirc/esp_mipi_dsi/esp_touch/esp_rmt` + 新位置的 `esp_irq.c`/`esp_vectors.S`（同目录 `verify_drivers_arch_build.txt`）。2542 目标的**整机**构建（含 LVGL/应用）本轮没跑完，与上一轮结论一致 |
| **Make 路径 filter-out** | ✅ 在临时目录复现真实 include 链（`chip/Make.defs` 先 `include common/Make.defs`）跑 `make show`：`CMN_CSRCS` 里**没有** `riscv_mtimer.c`；阴性对照（临时删掉那三行）里它出现 ⇒ 基线的无条件加入确实被芯片侧清掉（同目录 `make_path_filter_check.txt`） |

```
[esp32p4] [INF] *** Booting MCUboot build v2.4.0-151-gcafbf800 ***
NuttShell (NSH)
connected to "Hello_World_2.4G" ssid="Hello_World_2.4G" rssi=-46 ch=6
DhcpDnsStatus iface=0 link=1 dhcp=1 ip=192.168.1.9
wlan0 address 192.168.1.9 mask 255.255.255.0 gw 192.168.1.1
```

> 与上一轮（12:41 之前）唯一的差别是**这次没有逻辑分析仪**（USB 上没插 Saleae），
> 所以只抓了串口；分析仪波形属于附加证据，不属于三项验收。

