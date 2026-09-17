# 踩坑笔记 #04：从「RAM 执行」切到 Flash XIP（MCUboot 二级引导）

> 时间范围：2026-09-10 ~ 09-14。目标：把 ESP32-P4X 的应用从"整镜像搬进 SRAM 执行"
> 切换为"代码留在 16MB flash、由 MMU 映射就地执行（XIP）"，从而把 SRAM 让给 LVGL / 摄像头 / 堆。
>
> **结论：已完成并真机闭环** —— 一次 `build.sh` 产出双镜像；串口可见
> `Mapped IROM: vaddr=0x40000000 paddr=0x30000` → `start=0x4ff47e04` → `NuttShell (NSH)`。
> 更完整的背景/收益分析见 `docs/06_开发过程复盘与改进清单.md`；本文只记"坑"。

---

## 〇、先把术语对齐（很重要，历史上踩过认知坑）

| 说法 | 含义 | 本项目 |
|---|---|---|
| **RAM 执行**（本文旧称"RAM XIP"） | 镜像**整段**被引导器 memcpy 进 SRAM，CPU 从 `0x4FFxxxxx` 取指。flash 只当"装载盘" | 旧方案：`CONFIG_ESPRESSIF_SIMPLE_BOOT=y`（`esptool elf2image --ram-only-header`） |
| **Flash XIP** | 代码/只读数据**留在 flash**（`0x40000000` 窗口），由 flash MMU 映射后**就地执行**；只有数据段和中断相关代码进 SRAM | 现方案：`CONFIG_ESPRESSIF_BOOTLOADER_MCUBOOT=y` |
| **XIP 污染**（#03 坑 3 的旧词） | **旧方案下**误把代码链到 `0x40000000`，而镜像又是 ram-only 加载 → 执行到"空 flash" → crash | 现在这个映射是**必须的**，语义完全反转 |

> ⚠️ 所以读 `docs/踩坑笔记_03_烧录与启动.md` 时要注意：那一篇写于 8/18–8/24 的
> **RAM 执行**时代，其中"XIP"多指"要避开的东西"。本文记录的是后来**主动启用 XIP** 的整条路径。

---

## 一、切换前后（实测数据，同一条命令量出来）

| 维度 | 旧：RAM 执行（SIMPLE_BOOT） | 新：MCUboot + Flash XIP |
|---|---|---|
| 镜像 | `nuttx.bin` **462,816 B**，6 个段 load 地址全在 `0x4FFxxxxx` | 应用镜像 1,065,312 B（ESP 段）→ 签名+填槽后 `nuttx.bin` **1,835,008 B** |
| 代码 `.text` | **348,788 B @ `0x4ff49200`（SRAM）** | **670,334 B @ `0x40000000`（flash，XIP）** |
| 只读数据 `.rodata` | **69,884 B @ `0x4ff9fe00`（SRAM）** | **278,748 B @ `0x400b0020`（flash，XIP）** |
| IRAM 代码（中断/临界区） | 37,312 B | 32,768 B（`.iram0.text`） |
| `.data` | 6,408 B | 10,788 B |
| `.bss`（运行期） | ~46 KB | 54,380 B |
| **SRAM 实际占用** | ≈ **451 KiB**（`0x4ff40000`–`0x4ffb0fcc`） | ≈ **96 KiB**（iram 32.8 + data 10.8 + bss 54.4 + tcm/rtc） |
| 引导链 | ROM → 应用（无二级引导） | ROM → **MCUboot（24,672 B @ `0x2000`）** → 应用（`0x20000`） |
| 升级 | 只能整片烧写 | A/B 双槽 + 签名校验（可回滚） |

> 量法：`esptool image_info`（看段 load 地址与 `[DRAM,IRAM]`/`[DROM,IROM]` 属性）、
> `riscv-none-elf-size -A nuttx`（看段落点与大小）。
> 旧方案的基线固件仍保留在工作区根：`fw-simpleboot-good.bin`（462,816 B，
> `esptool image_info` 可见 6 个段全部 load 到 `0x4ffxxxxx`），可随时对照/回退。

**收益**：16MB flash 终于被当作**代码空间**使用；SRAM 释放约 **355 KB**，
从此 LVGL / 摄像头 buffer / 大堆才有地方放。

---

## 二、切换动作清单（改哪个文件）

| 动作 | 文件 |
|---|---|
| 打开 MCUboot 引导（SIMPLE_BOOT 自动关闭，Kconfig 里是 `depends on !ESPRESSIF_BOOTLOADER_MCUBOOT`） | `boards/risc-v/esp32p4/esp32p4-function-ev-board/configs/nsh/defconfig`：`CONFIG_ESPRESSIF_BOOTLOADER_MCUBOOT=y` |
| 钉 MCUboot 版本（必须含 p4 端口） | 同上：`CONFIG_ESPRESSIF_MCUBOOT_VERSION="cafbf800ddd727687075f26f9d69c9da4a515691"` |
| 定 OTA 槽位/scratch 布局（会被编译进引导） | 同上：`CONFIG_ESPRESSIF_OTA_{PRIMARY,SECONDARY}_SLOT_OFFSET` / `OTA_SLOT_SIZE` / `OTA_SCRATCH_*` |
| 代码/只读数据改落 flash | **无需改** —— `boards/risc-v/esp32p4/common/scripts/esp32p4_flat_memory.ld` 里已按宏自动切换（见坑 1） |
| 镜像封装：加 load header + 签名 | `nuttx/tools/espressif/espressif_mkimage.cmake`（MCUboot 分支） |
| load header 生成器（新增） | `nuttx/tools/espressif/mcuboot_mkloadhdr.py` |
| MCUboot 构建兼容 + XIP 映射补丁（新增） | `nuttx/tools/espressif/mcuboot_p4_fixup.sh`、`mcuboot_p4_loader_patch.py` |
| 引导构建进默认构建图 | `arch/risc-v/src/common/espressif/{Bootloader.cmake,CMakeLists.txt}`（见坑 6） |
| 烧录改双镜像 | 项目侧 `tools/usb_stable.sh` |

---

## 三、里程碑链路（按修复顺序）

| 阶段 | 现象 | 根因 | 修复 |
|---|---|---|---|
| 1. 代码落点 | 切了引导后代码仍在 SRAM（或反过来崩） | 链接脚本按 `CONFIG_ESPRESSIF_SIMPLE_BOOT` 二选一决定 `text_seg_low` 别名（坑 1） | 正确设置引导宏，让别名指向 `irom_seg`/`drom_seg` |
| 2. 引导报错 | `[ERR] Load header magic verification failed. Aborting` | P4 端口的 MCUboot 加载器要读 `esp_image_load_header_t`（magic `0xace637d3`），上游 NuttX 集成不生成（坑 2） | 新增 `mcuboot_mkloadhdr.py`，把 ESP 镜像重排为 `[MCUboot 头 0x20][load header 0x60][SRAM 区][pad][XIP 区]` 后再签名 |
| 3. 对齐约束 | 镜像里必须留空洞 | flash MMU 要求 `paddr % 64K == vaddr % 64K`（坑 3） | 生成器把 SRAM 区后的 padding 补到 64KB 边界，XIP 区落在槽内 `+0x10000` |
| 4. 跳转即崩 | MCUboot 校验通过、跳转后立刻 fault | MCUboot 只 memcpy IRAM/DRAM，**从不映射 flash**（坑 4） | `mcuboot_p4_loader_patch.py`：跳转前 `mmu_hal_map_region()` + `cache_hal_invalidate_addr()` |
| 5. 引导编不出来 | MCUboot 在 p4 上编译失败（5 类问题） | 锁定版无 p4 端口 / 子模块改名 / ABI 冲突 / HAL 门禁 / clang 风格参数（坑 5） | 全部收进幂等 `mcuboot_p4_fixup.sh`（挂 `PATCH_COMMAND`，避免被 update 步骤还原） |
| 6. 只产出应用镜像 | 文档里的构建命令产不出 `mcuboot-esp32p4.bin` | `bootloader` 目标不在 `all`；且内层 configure 拿不到 `CMAKE_MAKE_PROGRAM`（坑 6） | `add_dependencies(nuttx_context bootloader)` + 显式 `-GNinja -DCMAKE_MAKE_PROGRAM=<ninja>` |
| 7. 烧录流程失效 | 旧的"单镜像写 0x2000"起不来 | 现在是**双镜像**（`0x2000` 引导 + `0x20000` 应用）（坑 7） | `tools/usb_stable.sh` 两镜像 + 两次 hash 校验断言 |
| 8. 升级被拒 | `Cannot upgrade: more sectors than allowed` | `MCUBOOT_MAX_IMG_SECTORS=512`（esp 端口定义）< 3MB/4KB = 768 扇区（坑 8） | 走 `CONFIG_ESP_BOOT_UPGRADE_ONLY=1`（或调大该宏 / 缩小槽） |

---

## 坑 1：代码落在哪，全看链接脚本里那一个宏

`boards/risc-v/esp32p4/common/scripts/esp32p4_flat_memory.ld`：

```ld
#if CONFIG_ESPRESSIF_SIMPLE_BOOT
  REGION_ALIAS("text_seg_low",   sram_seg);    /* 代码进 SRAM —— 旧方案 */
  REGION_ALIAS("rodata_seg_low", sram_seg);
#else
  REGION_ALIAS("text_seg_low",   irom_seg);    /* 代码进 flash —— 现在这条 */
  REGION_ALIAS("rodata_seg_low", drom_seg);
#endif
```

- **现象**：只改引导宏却忘看这里，会得到"代码在 SRAM 但没人搬"或"代码在 flash 但没人映射"两种崩溃。
- **自检**（切换后必做）：`riscv-none-elf-size -A nuttx | grep -E "\.flash\.text|\.iram0\.text"`
  —— `.flash.text` 有几十万字节且地址 `0x40000000` 才算切对。
- **注意**：`.iram0.text`（中断向量、临界区、cache/MMU 操作、自旋锁）**必须留在 SRAM**，
  它不会因为切 XIP 而搬走；这是 XIP 下的硬约束，不是配置疏漏。

---

## 坑 2：MCUboot 的 P4 加载器要 load header，NuttX 集成不生成

- **现象**：MCUboot banner 正常打印，随后
  `[ERR] Load header magic verification failed. Aborting`。
- **根因**：P4 端口走的是 ESP-IDF 风格装载：应用镜像开头必须是
  `esp_image_load_header_t`（**magic `0xACE637D3`，0x60 字节 = 24 个 u32**），
  里面写清楚 entry、以及 IRAM/DRAM/IROM/DROM 各自的 vaddr / flash 偏移 / 大小。
  我们的镜像当时是 `[MCUboot 头 0x20][标准 ESP 镜像 0xE9 …]`，没有这一段。
- **修复**：`nuttx/tools/espressif/mcuboot_mkloadhdr.py` —— 读 esptool `elf2image` 产物，
  按段 load 地址分成「SRAM 区」和「XIP 区」，重排为
  `[MCUboot 头 0x20][load header 0x60][SRAM 区][pad 到 64KB][XIP 区]`，再交给 `imgtool sign`。
- **构建接线**：`espressif_mkimage.cmake` 的 MCUboot 分支三步走
  `elf2image → mcuboot_mkloadhdr.py → imgtool sign`。
- **实测输出**（`python3 tools/espressif/mcuboot_mkloadhdr.py <nuttx-app.bin> out.bin`，取值与下方真机日志一致）：
  ```
  load header: entry=0x4ff47e04
    SRAM  vaddr=0x4ff40000 size=0xaa24  (43556 B)   -> slot_off=0x80
    TCM   vaddr=0x30100024 size=0x88    (136 B)     -> slot_off=0xaaa4
    XIP   vaddr=0x40000000 size=0xf413c (999740 B)  -> slot_off=0x10000
  ```
  再看头里的字段（前 8 个 u32）：
  `magic=0xace637d3` / `entry=0x4ff47e04` / `iram_vaddr=0x4ff40000` / `iram_slot_off=0x80`
  / `iram_size=0xaa24` / `irom_vaddr=0x40000000` / `irom_slot_off=0x10000` / `irom_size=0xf413c`
- **教训**：这条错误信息很有误导性——它说"magic 校验失败"，像是镜像坏了，
  实际是**我们没按它的格式生成**。遇到陌生引导器的 `magic` 报错，先查它**期望的结构**，别先怀疑签名。

---

## 坑 3：XIP 区必须 64KB 对齐（paddr % 64K == vaddr % 64K）

- **根因**：flash MMU 按 64KB 页映射，映射后要求"虚拟地址与物理地址在页内偏移一致"。
  应用代码的 vaddr 是 `0x40000000`（64KB 对齐），所以它在槽内的 flash 偏移**也必须是 64KB 的整数倍**。
- **修复**：生成器在 SRAM 区之后补 padding，使 XIP 区的槽内偏移落在 64KB 边界：
  当前构建 padding ≈ **0x555C ≈ 21.9 KB**（随应用 SRAM 段大小变化）。它同时给出编译期断言，
  防止 SRAM 段大小变化后静默错位。
- **代价**：镜像里有约 20–27 KB 的"结构性空洞"，无法避免（这是 XIP 的固有开销）。
- **自检**：`esptool image_info nuttx-app.bin` 里 XIP 段的 `load` 地址必须是 `0x40000000`
  且紧跟其后的 padding 段长度合理。

---

## 坑 4：MCUboot 只搬 IRAM/DRAM，**从不映射 flash**（XIP 最致命的一条）

- **现象**：MCUboot 校验通过、`start=0x4ff47xxx` 打印出来，跳转后**立刻 fault**
  （对旧方案来说这永远不会发生，因为代码本来就在 SRAM）。
- **根因**：P4 端口的 `esp_app_image_load()` 只做两件事：把 IRAM/DRAM 段 `memcpy` 到目标地址、
  然后跳 entry。**它不碰 flash MMU**。而 XIP 应用的第一条 flash 指令页从未被映射进地址空间。
- **修复**：`mcuboot_p4_loader_patch.py`（幂等，挂在 MCUboot 的 `PATCH_COMMAND` 上）在跳转前插入：
  ```c
  mmu_hal_map_region(0, MMU_TARGET_FLASH0, load_header.irom_map_addr,
                     fap->fa_off + load_header.irom_flash_offset,
                     load_header.irom_size, &mapped_len);
  cache_hal_invalidate_addr(load_header.irom_map_addr, mapped_len);
  ```
  信息源正是坑 2 里那个 load header 的 `irom_*` 字段 —— 两件事是配套的。
- **真机证据**（每块板每次启动都能看到；下面这段是 2026-09-16 当前固件的实录，
  与上面的 load header 字段逐一对得上 —— 这正是"两件事配套"的最好证明）：
  ```
  [esp32p4] [INF] Loading image 0 - slot 0 from flash, area id: 1
  [esp32p4] [INF] DRAM segment: start=0x20000, size=0x0, vaddr=0x4ff00000
  [esp32p4] [INF] IRAM segment: start=0x20080, size=0xaa24, vaddr=0x4ff40000   ← = iram_size
  [esp32p4] [INF] LP_IRAM segment: paddr=0002aaa4h, vaddr=30100024h, size=00088h (136) load
  [esp32p4] [INF] Mapped IROM: vaddr=0x40000000 paddr=0x30000 len=0x100000   ← XIP 映射生效（len 按 64KB 向上取整）
  [esp32p4] [INF] start=0x4ff47e04                                          ← = entry
  ```
  > `len=0x100000`（1 MB）是把 `irom_size=0xf413c` 按 64KB 页向上取整的结果，
  > 它证明映射确实发生、且覆盖了全部 XIP 区。
- **教训**：给 MCUboot 这类"只搬数据"的引导器配 XIP 应用，**必须自己补映射**；
  这类补丁一定要挂成幂等的 `PATCH_COMMAND`（外部项目的 update 步骤会把源码还原）。

---

## 坑 5：MCUboot 在 p4 上的构建兼容（五个坑，一次收进脚本）

| # | 现象 | 修复（写进 `mcuboot_p4_fixup.sh`，幂等） |
|---|---|---|
| 1 | 锁定版 MCUboot 没有 p4 端口 | 换到唯一含 p4 的 master 提交 `cafbf800`（写进 defconfig `CONFIG_ESPRESSIF_MCUBOOT_VERSION`） |
| 2 | 子模块路径写死旧名 | `.gitmodules` 里已改名 `ext/mbedtls-3.6.0`，`PATCH_COMMAND` 同步该路径 |
| 3 | `rv32imc` 与工具链默认 `ilp32d` ABI 冲突 | p4 单独用 `-march=rv32imac_zicsr_zifencei -mabi=ilp32`（`mcuboot_toolchain_espressif.cmake`） |
| 4 | HAL 版本门禁（期望 6.0.0，实际 6.1.0） | 放宽门禁到 6.1.0 |
| 5 | `-specs=picolibc.specs`（我们只有 nosys）、`-std=gnu23`（GCC13 不支持）、esptool v5 的连字符参数名 | 统一替换/去重 |

- **关键点**：这些都挂在 **`PATCH_COMMAND`** 上而不是手工改源码 ——
  `ExternalProject` 每次 update 都会重置 checkout，手工改动会被静默还原（这是**最坑**的一点）。
- 产物：`nuttx/mcuboot-esp32p4.bin`（24,640 B）。

---

## 坑 6：一次 `build.sh` 产不出双镜像（两个接线缺陷）

1. **`bootloader` 目标不在 `all`**：干净树上跑文档里的构建命令，只出应用镜像，
   `nuttx/mcuboot-esp32p4.bin` 缺失 → 两镜像烧录流程必挂。
   → `arch/risc-v/src/common/espressif/CMakeLists.txt` 加 `add_dependencies(nuttx_context bootloader)`。
2. **内层 configure 找不到 ninja**：外层用的是 openvela **预置 ninja**（不在 `PATH`），
   而 ExternalProject 会继承父工程的 `CMAKE_GENERATOR=Ninja`，
   于是内层报 `CMake was unable to find a build program corresponding to "Ninja"`。
   → `Bootloader.cmake` 显式传 `-GNinja -DCMAKE_MAKE_PROGRAM=<ninja 绝对路径>`。

**回归验证**：`rm -rf cmake_out` 后单条
`./build.sh esp32p4-function-ev-board:nsh --cmake -j8`
应同时产出 `cmake_out/.../nuttx.bin` 与 `nuttx/mcuboot-esp32p4.bin`（干净树约 9–11 分钟）。

---

## 坑 7：烧录从"单镜像"变"双镜像"

```
0x002000  MCUboot 引导    nuttx/mcuboot-esp32p4.bin      ← 新增
0x020000  OTA_0 应用      cmake_out/.../nuttx.bin
```

- 旧的"单镜像写 `0x2000`"**完全失效**（会把应用写到引导位置）。
- **脚本竞态（真实踩到）**：不要写 `esptool ... | tee log | grep -q "Hash of data verified"` ——
  `grep -q` 命中第一个镜像就退出，`tee` 收到 SIGPIPE 挂掉，把正在写第二个镜像的 esptool 一起杀掉，
  表现为"烧录返回成功、应用槽只写了一半"，随后 MCUboot 报
  `Image in the primary slot is not valid!`。正确做法：**先落盘日志，进程结束后再校验**，
  并要求日志里出现**两次** hash 校验。
- 项目侧脚本：`tools/usb_stable.sh`（两镜像 + 断言 + 重试）。

---

## 坑 8：`Cannot upgrade: more sectors than allowed` 不是 scratch 太小

- **现象**：MCUboot 报这条警告，随后按旧镜像启动。
- **根因**：`MCUBOOT_MAX_IMG_SECTORS` 在 Espressif 端口里被定义为 **512**
  （`boot/mcuboot_config.h`），而 3MB 槽 / 4KB 扇区 = **768** 个扇区 > 512。
  **scratch 大小（256KB）远远够用**（它只需容纳一个扇区级别），之前"scratch 太小"的判断是错的。
- **修复（二选一）**：把该宏提到 ≥768；或按当前配置走 **overwrite-only**
  （`CONFIG_ESP_BOOT_UPGRADE_ONLY=1`），不做 A/B swap。

---

## 坑 9：HAL 兼容补丁重复应用 → configure 直接失败（切换期的构建链）

- **现象**：连续构建时 configure 报
  `git apply 失败：nuttx/esp32p4/include/sdkconfig.h:683/941` → `Configuring incomplete`。
- **根因三条**：① 打补丁前**不重置 HAL 树**（checkout 在构建目录里跨 configure 复用，
  树里已带补丁时再 apply 必失败）；② 两个补丁都改 `sdkconfig.h` 且上下文相邻，顺序敏感；
  ③ `0001` 里有两段过时 hunk（submodule 指针噪声、HAL 已自带的注释）。
- **修复**：`git reset --hard <钉定 HAL 版本>` + 新增 `vela_apply_hal_patch()`
  （`--check` 通过才 apply；反向 check 通过视为已应用；都不通过才 FATAL），顺序固定 **0002 → 0001**。
- **附带坑（同一条链）**：改完 `defconfig` **必须 `rm -rf cmake_out`** ——
  `lunch` 只在构建目录**不存在**时才跑 cmake；只删 `.config`/`CMakeCache.txt` 会得到
  `loading 'build.ninja': No such file or directory`。

---

## 坑 10：Flash XIP 的代价与约束（切换前必须知道）

| 约束 | 说明 | 本项目怎么处理 |
|---|---|---|
| **代码只读** | 运行时不能改自己在执行的 flash（不能自改代码 / 动态打补丁） | 不用自改代码；热路径与中断处理显式放 `.iram0.text` |
| **关键代码要留 SRAM** | 中断向量、临界区、cache/MMU 操作、自旋锁 | `.iram0.text` 32 KB 常驻 SRAM |
| **取指走 cache** | 冷启动/换页有 miss 开销 | 关键路径进 IRAM；实测调度与网络无异常 |
| **升级必须"写另一个槽 + 重启"** | 运行中的 app 不能覆盖自己正在执行的 flash | 本来就是 A/B 双槽设计，天然满足 |
| **镜像有结构性空洞** | 64KB 对齐约束产生 padding | ~21–27 KB，可接受 |
| **启动多一级** | 多一次引导 + 整镜像 hash 校验时间 | 实测启动到 NSH 无可感知差异（未做精确计时） |

---

## 四、验证方法（可复制）

```bash
cd <openvela 根>

# 1) 段落在哪（代码是否真的在 flash）
export PATH=$PWD/prebuilts/gcc/linux-x86_64/riscv-none-elf/bin:$PATH
riscv-none-elf-size -A cmake_out/esp32p4-function-ev-board_nsh/nuttx \
  | grep -E "flash\.text|flash\.rodata|iram0\.text|dram0\.(data|bss)"
#   期望：.flash.text/.flash.rodata 地址 0x40000000 段；.iram0.text/.dram0.* 地址 0x4ff4xxxx 段

# 2) ESP 镜像的段属性
python3 -m esptool --chip esp32p4 image_info cmake_out/esp32p4-function-ev-board_nsh/nuttx-app.bin
#   期望：代码/只读数据段标 [DROM,IROM]，地址 0x40000000+；数据段标 [DRAM,IRAM]，地址 0x4ff40000+

# 3) load header 生成结果（构建日志里就有）
grep -A3 "load header" <构建日志>

# 4) 真机：引导→映射→跳转→NSH 四连
python3 contest2026_342_ezdezhandui/tools/board.py reset --save /tmp/boot.log
grep -E "Loading image 0|IRAM segment|Mapped IROM|start=0x|NuttShell" /tmp/boot.log
```

---

## 五、回归清单（改链接脚本 / 改引导配置 / 升级 MCUboot 后必须重跑）

- [ ] `rm -rf cmake_out` 后单条 `build.sh --cmake -j8` 成功，且**双镜像**都在
- [ ] `size` 检查：`.flash.text` 在 `0x40000000`、`.iram0.text` 在 `0x4ff40000`
- [ ] 构建日志出现 `load header: ...` 且 XIP 段 `slot_off=0x10000`
- [ ] `tools/usb_stable.sh` 两镜像烧录，两次 hash 校验
- [ ] 串口出现 `Mapped IROM: vaddr=0x40000000 ...`（**这行是 XIP 生效的唯一凭据**）
- [ ] 进到 `nsh>`，`free` / `ifconfig eth0` 正常
- [ ] MCUboot 无 `Cannot upgrade: more sectors than allowed`

---

## 六、与踩坑笔记 #03 的关系（同一现象，方位反转）

| 主题 | #03（8/18–8/24，RAM 执行） | #04（9/10–9/14，Flash XIP） |
|---|---|---|
| 「XIP」一词的语义 | 要**避开**的东西：链接误指 `0x40000000` → 执行空 flash → `load access fault @0x25` | 要**主动启用**的东西：映射 `0x40000000` → 就地执行 |
| `map_rom_segments` | SIMPLE_BOOT 下**跳过**（否则整片 SRAM 被无效映射 = "XIP 污染"） | MCUboot 下**必须做**，且由我们的 loader patch 补上 |
| 烧录偏移 | 单镜像 `0x2000` | 双镜像 `0x2000`（引导）+ `0x20000`（应用） |
| 链接脚本分支 | 改的是 `CONFIG_ESPRESSIF_SIMPLE_BOOT` 那一支 | 走的是 `#else` 那一支（`irom_seg`/`drom_seg`） |

> 三篇笔记连起来读，就是这块板子"从把应用塞进 SRAM，到把 flash 变成代码空间"的完整轨迹：
> **#02 移植 → #03 用 RAM 执行跑通 → #04 切 Flash XIP 并把 SRAM 让出来**。
