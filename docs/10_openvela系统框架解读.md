# openvela 系统框架解读（面向本项目）

> 适用对象：刚接手本工程、需要快速建立"openvela 到底是怎么搭起来的"全局认知的人（含评审）。
> 所有路径均为本工作区实测存在；文中给出的自查命令可直接复制执行。
> 最后更新：2026-09-14

---

## 〇、一句话定位

**openvela 是小米开源的、面向 AIoT 操作系统发行版，内核是 Apache NuttX（实时内核，Apache-2.0），
外面包了一层小米自己的框架 / 包 / 运行时，再往下是厂商板级与官方 HAL。**

因此可以这样理解：

```
openvela ≈ NuttX 内核  +  frameworks（框架）  +  packages（自有包）
         +  vendor/boards（厂商与板级）  +  repo/manifest（多仓工程）  +  统一构建系统
```

在本项目里，NSH、`ai_agent`、LVGL 都是"NuttX 上的应用"，只是被 openvela 的工程体系组织起来了。

---

## 一、工程组织：repo + manifest（264 个仓拼出一个工作区）

工作区不是一个 git 仓，而是 **264 个仓按 manifest 拼出来的**：

```
contest2026_342_ezdezhandui/openvela.xml          ← 官方 manifest：264 个 <project>
   <default remote="openvela" revision="dev-ai-contest-2026"/>
   <project path="apps"  name="nuttx-apps"/>
   <project path="apps/graphics/lvgl/lvgl"  name="apps_graphics_lvgl"/>
   ...（其余 261 个）

contest2026_342_ezdezhandui/contest2026_342_ezdezhandui.xml   ← 本项目 manifest
   self-ref <project> + 324 条 <copyfile>（把本仓文件树映射回工作区各路径）
```

自查：

```bash
cd <openvela 根>/contest2026_342_ezdezhandui
grep -c "<project" openvela.xml          # → 264
grep -c copyfile contest2026_342_ezdezhandui.xml   # → 324
```

### 顶层目录一览

| 目录 | 体量（实测） | 是什么 |
|---|---|---|
| `nuttx/` | 1.9 G | **内核 + 架构 + 板级**（NuttX 主干，openvela fork） |
| `apps/` | 896 M | **应用与中间件**（nshlib、system、examples、graphics/LVGL、netutils、crypto/mbedtls…） |
| `packages/` | 296 M | **小米自有包**（本项目 `ai_agent` 在此） |
| `frameworks/` | — | **系统框架**（连接、图形、多媒体、安全、运行时、系统服务） |
| `vendor/` | 1.4 G | **厂商板级/产品**（xiaomi、espressif、beken、bes、rockchip、st、infineon…） |
| `external/` | 1.5 G | **200+ 第三方库**（curl、ffmpeg、eigen、harfbuzz…） |
| `prebuilts/` | **13 G** | **预置工具链**（gcc/clang/qemu/emulator/cmake/build-tools）→ 构建不依赖宿主环境 |
| `build/` | 228 K | **构建入口**（`envsetup.sh`、cmake 自定义模块、config_check） |
| `tools/` `tests/` | — | 工具与测试套件 |

> **关键小细节**：`apps/packages` 是**指向 `../packages` 的符号链接**。
> NuttX 的 apps 构建只认 `apps/` 目录树，openvela 便把自有 `packages/` 折进 apps 构建
> （`apps/CMakeLists.txt`: `add_subdirectory(packages)`）。
> 证据：构建目录里出现 `apps/packages/ai_agent/libapps_ai_agent.a`。
>
> 自查：`ls -la apps/packages` → `apps/packages -> ../packages`

---

## 二、分层架构（从硬件往上）

```
┌────────────────────────────────────────────────────────────────────────────┐
│ 应用层      apps/nshlib + apps/system/nsh(NSH)  examples/  packages/ai_agent│
│             LVGL 界面 / 自定义 Skill / WebSocket 通道                        │
├────────────────────────────────────────────────────────────────────────────┤
│ 框架层      frameworks/connectivity(蓝牙/telephony) graphics(uikit)          │
│             multimedia(media) runtimes(quickapp/wasm/typescript/ash)        │
│             security(ca/permission/optee) system(binder/healthd/ota)        │
├────────────────────────────────────────────────────────────────────────────┤
│ 系统服务    apps/system  apps/netutils  apps/crypto(mbedtls)                │
├────────────────────────────────────────────────────────────────────────────┤
│ NuttX 内核  sched(调度/SMP)  mm(堆/IOB)  fs(VFS)  net(lwIP)  drivers/       │
│             libs/libc  binfmt/  syscall/  openamp(异构多核)                 │
├────────────────────────────────────────────────────────────────────────────┤
│ BSP 层      arch/risc-v/src/esp32p4（芯片层）                               │
│             boards/risc-v/esp32p4/esp32p4-function-ev-board（板级 bringup）  │
│             arch/risc-v/include/esp32p4（芯片头）                           │
├────────────────────────────────────────────────────────────────────────────┤
│ HAL 层      arch/risc-v/src/common/espressif/esp-hal-3rdparty               │
│             （Espressif 官方 3rd-party HAL，钉定 8d0a8989100）              │
├────────────────────────────────────────────────────────────────────────────┤
│ 硬件        ESP32-P4（RISC-V 双核）16MB Flash / 32MB PSRAM                  │
│             EMAC+PHY(RJ45)  MIPI-DSI+CSI  USB-Serial/JTAG  UART0(CP2102)     │
└────────────────────────────────────────────────────────────────────────────┘
```

### 1) HAL 层：官方驱动，NuttX 只做薄封装

- 实体位置：`nuttx/arch/risc-v/src/common/espressif/esp-hal-3rdparty/`
  （构建时由 CMake 拉取到构建目录，钉定版本 `8d0a8989100`）
- 内含：`components/esp_hal_cam`、`components/upper_hal_isp`（ISP！）、`esp_rom`、`hal`、`efuse`、
  `bootloader_support` 等
- NuttX 侧用 `esp_*.c`（`esp_gpio.c` / `esp_i2c.c` / `esp_emac.c` / `esp_csi.c` …）把 HAL 包成
  NuttX 的驱动接口
- **本项目相关**：`esp_csi.c` 目前是 "CSI 直通 RAW10"，要出彩色帧需要把 ISP 串进来

### 2) BSP 层：芯片 + 板子

```
nuttx/arch/risc-v/src/esp32p4/           芯片层（中断、时钟、atomic、启动）
nuttx/arch/risc-v/include/esp32p4/       芯片头（chip.h、irq.h、gpio_sig_map.h）
nuttx/boards/risc-v/esp32p4/
  ├── common/scripts/*.ld                 ★ 链接脚本：内存布局定义在这里
  ├── esp32p4-function-ev-board/          ★ 本项目的板子
  │     ├── configs/nsh/defconfig         ★ 板级配置（改行为先改这里）
  │     ├── src/esp32p4_bringup.c         ★ 板级初始化总入口
  │     ├── src/esp32p4_{display,lcd_ek79007,touch_gt911,camera,mipi_dpi,ethernet}.c
  │     └── src/esp32p4_{boot,reset,gpio,hmi_power}.c
  ├── esp32p4-tab5/  esp32p4-pico-wifi-wareshare/（同芯片其它板）
```

板级 bringup 决定"硬件怎么活过来"。本项目里这些日志全部出自这一层：
`MIPI-DSI host registered` / `EK79007 panel initialized` / `GT911 … registered` /
`SC2336 chip ID: 0xcb3a` / `Camera registered on /dev/video0`。

> 注：早期日志里的 `EK79007 DCS 0xb2 failed: -110`、`GT911 not detected`、`VIDIOC_S_FMT errno=22`
> 看着都像硬件问题，实际根因分别在 **NuttX 驱动层**（`drivers/video/mipidsi/mipi_dsi_device.c`
> 未零初始化 `mipi_dsi_msg`）、**传感器驱动**（GT911 产品 ID 校验）与 **V4L2 上层**
> （`v4l2_cap.c` 在驱动未声明 `frmintervals` 时回退写死 15 fps）。三处均已修复，
> 详见 `docs/05_开发过程复盘与改进清单.md` §22/§23。

### 3) 内核层：NuttX 本体（按目录读）

| 目录 | 职责 | 本项目对应现象 |
|---|---|---|
| `sched/` | 调度、任务/线程、信号、工作队列 | `ps` 里的 `CPU0 IDLE` / `lpwork` / `emac_rx` |
| `mm/` | 堆管理、IOB 缓冲 | `free` 显示的 `Umem`（PSRAM 堆 33.9 MB） |
| `fs/` | VFS 与文件系统 | `mount` 显示 `/proc`、`/tmp`(tmpfs) |
| `net/` | 网络栈（lwIP） | `eth0 10.0.0.2 RUNNING`、WebSocket 监听 |
| `drivers/` | 驱动框架（serial/adc/i2c/spi/video(V4L2)/input/lcd/net…） | `/dev/*` 节点、`camera`(V4L2) |
| `libs/libc/` | C 库 | `libs/libc/netdb` ← **DNS 客户端就在这里没被编进去** |
| `binfmt/` | 可执行文件格式与 builtin | NSH 里能直接敲 `ai_agent` |
| `arch/` | 架构与芯片 | `arch/risc-v/src/esp32p4` |
| `openamp/` | 异构多核通信（A 核 + M 核类场景） | 本板未用 |

**三种构建模式**（`nuttx/Kconfig` 顶层 menu）：

| 模式 | 含义 | 本项目 |
|---|---|---|
| `CONFIG_BUILD_FLAT` | 内核与应用同地址空间（MCU 常用，最简单） | ✅ 我们在用 |
| `CONFIG_BUILD_PROTECTED` | 内核/用户态分离（MPU） | — |
| `CONFIG_BUILD_KERNEL` | 完整 MMU 多进程 | — |

自查：`grep -E "^CONFIG_BUILD_" cmake_out/<build-dir>/.config`

### 4) 框架层：openvela 相对"原生 NuttX"最大的增量

```
frameworks/
├── connectivity/   bluetooth、telephony
├── graphics/       uikit、gpu_tools
├── multimedia/     media
├── runtimes/       quickapp（快应用）、wasm、typescript、ash、feature、services
├── security/       ca、permission、optee_vela
└── system/         binder、healthd、charger、ota
```

对照本项目：`frameworks/system/ota`（OTA 框架）+ 我们打通的 **MCUboot A/B 槽位**是同一条能力线的两端；
`frameworks/runtimes/quickapp` 对应用户态快应用（本项目 `packages/ai_agent/src/quickapp` 有对接代码）。

### 5) 应用层

| 位置 | 说明 |
|---|---|
| `apps/nshlib/` | **NSH shell 实现**（`nsh_alias.c`、`nsh_builtin.c` …） |
| `apps/system/nsh/` | NSH 的构建目标与入口（`nsh_main.c` → `libapps_nsh.a`） |
| `apps/system/` `apps/netutils/` `apps/examples/` | 系统命令、网络工具、示例（`camera` 例子在此） |
| `apps/graphics/lvgl/` | LVGL（本项目 `lvgldemo` 用它） |
| `packages/ai_agent/` | ★ **本项目作品**：AI Agent 应用，注册成 NSH 内置命令 |

### 6) 厂商层

```
vendor/xiaomi/vela/{pyxis,sysmon,fbdebug}     小米的板级/产品组件
vendor/espressif/boards/esp32s3/...           厂商维护的板子
vendor/{beken,bes,rockchip,st,infineon,sifli,...}
```

> **板子可以放在两处**（`lunch` 两处都会找）：
> - `vendor/<厂商>/boards/<板子>/configs/<配置>/defconfig`（openvela 风格）
> - `nuttx/boards/<架构>/<芯片>/<板子>/configs/<配置>/defconfig`（上游 NuttX 风格）← **本项目走这条**

---

## 三、配置体系：Kconfig 多层 → defconfig → .config → config.h

```
nuttx/Kconfig（总入口）
 ├─ menu "System Type"      → source arch/Kconfig → arch/risc-v/Kconfig → …/src/esp32p4/Kconfig
 ├─ menu "Board Selection"  → source boards/Kconfig → …/esp32p4-function-ev-board/Kconfig
 ├─ menu "RTOS Features"    → source sched/Kconfig、syscall/Kconfig
 ├─ menu "Device Drivers"   → source drivers/Kconfig
 ├─ menu "Networking"       → source net/Kconfig
 └─ fs/ mm/ libs/ binfmt/ crypto/ graphics/ openamp/ … 各自的 Kconfig
apps/Kconfig、packages/Kconfig、frameworks/*/Kconfig
```

数据流：

```
defconfig（人写的最小集合）
   └─ kconfig-conf 解析 ─→ .config（全部符号最终值，本项目约 3500 行）
        └─ 生成 ─→ include/nuttx/config.h（C 宏；代码里 #ifdef CONFIG_xxx 控制编译）
```

**本项目改过的典型符号**（都在 `boards/risc-v/esp32p4/esp32p4-function-ev-board/configs/nsh/defconfig`）：

| 符号 | 作用 |
|---|---|
| `CONFIG_ESPRESSIF_BOOTLOADER_MCUBOOT=y` | 走 MCUboot 二级引导（替代 SIMPLE_BOOT） |
| `CONFIG_ESPRESSIF_OTA_{PRIMARY,SECONDARY}_SLOT_OFFSET` / `OTA_SLOT_SIZE` | OTA 槽位布局，**会写进 MCUboot 的 bootloader.conf** |
| `CONFIG_ESPRESSIF_MIPI_CSI_H_RES/V_RES/LANES/LANE_BITRATE_MBPS` | CSI 接收窗口，摄像头 `VIDIOC_S_FMT` 就是拿它校验 |
| `CONFIG_UART0_SERIAL_CONSOLE` / `CONFIG_ESPRESSIF_USBSERIAL` | console 走哪条物理通道 |
| `CONFIG_VIDEO_SC2336` / `CONFIG_ESPRESSIF_MIPI_CSI` | 摄像头驱动 |
| `CONFIG_EXAMPLES_LVGLDEMO` / `CONFIG_LV_USE_DEMO_WIDGETS` | LVGL 示例 |

⚠️ 两点经验（本项目实际踩到）：
1. **`depends on` 会挡住你以为能开的开关**：想开 `CONFIG_DEBUG_VIDEO_ERROR` 必须先开
   `CONFIG_DEBUG_FEATURES` + `CONFIG_DEBUG_ERROR`。
2. **改完 defconfig 必须 `rm -rf <构建目录>`**：`lunch` 只在构建目录**不存在**时才跑 cmake，
   删 `.config` / `CMakeCache.txt` 都不会触发重新配置（会以 `loading 'build.ninja': No such file` 失败）。

自查：
```bash
grep -n "MCUBOOT\|OTA_SLOT" nuttx/boards/risc-v/esp32p4/esp32p4-function-ev-board/configs/nsh/defconfig
grep -n "CONFIG_BUILD_FLAT\|CONFIG_NETDB_DNSCLIENT" cmake_out/<build-dir>/.config
```

---

## 四、构建系统：`build.sh` → `envsetup.sh` → CMake/Ninja

```
./build.sh nuttx/boards/risc-v/esp32p4/esp32p4-function-ev-board/configs/nsh/ --cmake -j8
    │
    ├─ build.sh（工作区根，symlink → nuttx/tools/build.sh）
    │     └─ source build/envsetup.sh        # 提供 lunch / m / mm / mgrep
    ├─ lunch <board-config> <cmake_binary_dir>
    │     └─ cmake -B <构建目录> -S nuttx \
    │            -DBOARD_CONFIG=<board>/configs/nsh/ \
    │            -DCUSTOM_MODULE_PATH=build/cmake -GNinja
    └─ m -j8                                 # ninja 封装
```

| 要点 | 说明 |
|---|---|
| 构建目录 | `build.sh` 默认 `cmake_out/<board>_<config>`；openvela 原生 `lunch` 默认 `out/<vendor>_<board>_<config>` |
| 产物 | `nuttx`（ELF）→ 平台镜像 `nuttx.bin` |
| CMake 组织 | 顶层 `nuttx/CMakeLists.txt`；各子目录 `nuttx_add_subdirectory()` |
| **应用注册** | 每个应用调用 **`nuttx_add_application(NAME … SRCS … STACKSIZE … PRIORITY …)`** |
| 平台镜像 | `nuttx/tools/espressif/espressif_mkimage.cmake`（本项目在此做了 MCUboot 化改造，见 §六） |
| 烧录 | `nuttx/tools/espressif/espressif_flash.cmake`（`0x2000` 引导 + `0x20000` 应用） |
| kconfig↔cmake 桥 | `nuttx/cmake/nuttx_kconfig.cmake`（本项目在此踩过解析坑，见 §七） |

**最终链接**（本项目构建目录 `build.ninja` 里的真实清单）：

```
nuttx = apps/system/nsh/libapps_nsh.a + apps/system/nsh/libapps_sh.a
      + apps/packages/ai_agent/libapps_ai_agent.a
      + apps/examples/camera/libapps_camera.a + apps/examples/lvgldemo/libapps_lvgldemo.a
      + apps/examples/touchscreen/libapps_tc.a + apps/libapps.a
      + apps/graphics/lvgl/liblvgl.a + apps/external/android/libandroid.a
      + apps/builtin/libapps_builtin.a
      + arch/libarch.a + boards/libboard.a
      + drivers/libdrivers.a + fs/libfs.a + libs/libc/libc.a + mm/libmm.a
      + net/libnet.a + sched/libsched.a + binfmt/libbinfmt.a
      + apps/crypto/mbedtls/libmbedtls.a + libgcc.a + libm.a
```

> 读法：**内核（arch/boards/drivers/fs/mm/net/sched/libs）与应用（apps/*）被链进同一个 ELF**
> ——这就是 flat build（`CONFIG_BUILD_FLAT`）的直接体现。

---

## 五、启动流程（结合本项目真机日志）

```
① 芯片 ROM
② 引导层：MCUboot（本项目）/ 或 SIMPLE_BOOT（旧方案）
     [esp32p4] [INF] Loading image 0 - slot 0 from flash, area id: 1
     [esp32p4] [INF] IRAM segment: start=0x20080, size=0x9488, vaddr=0x4ff40000   ← 只把 SRAM 段搬进 SRAM
     [esp32p4] [INF] Mapped IROM:  vaddr=0x40000000 paddr=0x30000 len=0xe0000    ← 代码段 MMU 映射到 flash（XIP）
     [esp32p4] [INF] start=0x4ff475c4
③ NuttX 汇编入口 __start（arch/risc-v）
④ nx_start()（sched/）→ 依次初始化：时钟/中断/内存/文件系统/驱动框架/网络
⑤ board_app_initialize() → boards/…/src/esp32p4_bringup.c
     console(UART0) → EMAC(eth0) → PSRAM heap → LCD/DSI → GT911 → MIPI-CSI(SC2336)
⑥ nsh_main（apps/system/nsh + apps/nshlib）→ 出现 nsh>
⑦ 用户敲 nsh> ai_agent → packages/ai_agent 主流程 P0…P6 全服务启动
```

**应用如何变成"命令"**：

```cmake
# packages/ai_agent/CMakeLists.txt
nuttx_add_application(NAME ai_agent SRCS … STACKSIZE … PRIORITY …)
```
→ 构建期生成 builtin 注册表（`apps/builtin/`）→ NSH 直接执行。

本机实测 `help` 里的内置应用：`nsh`、`sh`、`ai_agent`、`camera`、`lvgldemo`。

---

## 六、内存 / 存储 / 镜像（本项目改造重点）

### 6.1 内存分区（链接脚本 `boards/risc-v/esp32p4/common/scripts/esp32p4_flat_memory.ld`）

| 区域 | 地址/大小 | 说明 |
|---|---|---|
| SRAM 窗口 | `0x4FF40000 – 0x4FFC0000`（512 KB） | ROM 用锁死的 PMP 保护；**改造前整个应用必须塞进这里** |
| Flash 执行窗口 | `0x40000000`（IROM）/ `0x40060020`（DROM） | 经 MMU 映射后**就地执行（XIP）** |
| PSRAM | 32 MB | NuttX 堆（`free` → `Umem total 33,985,952`） |

**改造效果**：`SIMPLE_BOOT`（整镜像 memcpy 进 SRAM）→ `MCUboot + XIP`（只搬数据段，代码留 flash），
SRAM 占用从 ~451 KB 降到 ~83 KB。

### 6.2 Flash 布局（MCUboot 视角）

```
0x000000 – 0x002000  ROM
0x002000 – 0x020000  MCUboot 引导（mcuboot-esp32p4.bin，24,640 B）
0x020000 – 0x1E0000  OTA_0 主槽（应用镜像，填充到槽大小 1,835,008 B）
0x1E0000 – 0x3A0000  OTA_1 副槽
0x3A0000 – 0x3E0000  scratch（256 KB）
```

### 6.3 镜像封装链（本项目在 `espressif_mkimage.cmake` 里做的三步）

```
nuttx (ELF)
  └─ esptool elf2image      → nuttx-app.bin       （ESP 段式镜像，代码段落在 0x40000000 等）
      └─ mcuboot_mkloadhdr.py → nuttx-payload.bin  （生成 MCUboot 需要的 load header，携带 XIP 映射信息）
          └─ imgtool sign     → nuttx.bin          （签名 + 填充到槽大小，烧到 0x20000）
```

对应 MCUboot 侧需要两处适配（本仓 `nuttx/tools/espressif/`）：
`mcuboot_p4_fixup.sh`（构建兼容修复）、`mcuboot_p4_loader_patch.py`（**跳转前把 flash 页映射进 MMU，否则 XIP 应用首次取指就 fault**）。

---

## 七、本项目在整张图里的位置

```
frameworks/                     ← 基本未改（用到 runtimes/quickapp 接口）
      ↑
packages/ai_agent               ← ★ 主战场之一：P0~P6 服务、12 skills、cron、WS 通道
      ↑
apps/                           ← 改了 4 个文件（examples/camera 兼容板级已注册设备等）
      ↑
nuttx/                          ← ★ 主战场之二
  ├─ boards/risc-v/esp32p4/…         板级：显示/触摸/摄像头/EMAC/PSRAM bringup
  ├─ arch/risc-v/src/esp32p4/…       芯片层 + HAL 兼容 patch（patches/）
  ├─ arch/risc-v/src/common/espressif/  HAL 薄封装（含 esp_csi.c）+ Bootloader.cmake
  └─ tools/espressif/…               mkimage / MCUboot fixup / load header / 烧录
      ↑
HAL（esp-hal-3rdparty，钉定 8d0a8989100，通过 patch 适配 openvela）
```

**交付形态**：不是 patch 文件，而是**真实文件树** `board/esp32p4_vela/{nuttx,apps,ai_agent}`
+ manifest 的 **324 条 `<copyfile>`**，`repo sync` 时自动铺回工作区（评审零手工拷贝）。

**开发闭环**：

```
工作区改代码 → build.sh 编译验证 → board/esp32p4_vela/export.sh（工作区 → 文件树）
            → 作品仓 commit → lock-revision.sh（锁 manifest revision）
反向：评审 repo sync → board/esp32p4_vela/deploy.sh（文件树 → 工作区）→ 构建
```

---

## 八、速查表

| 我想… | 去哪里 |
|---|---|
| 改板子行为（console/外设/分区/驱动开关） | `nuttx/boards/risc-v/esp32p4/esp32p4-function-ev-board/configs/nsh/defconfig` |
| 改板级初始化顺序 | `boards/…/src/esp32p4_bringup.c` |
| 改内存布局 / 段落点 | `boards/risc-v/esp32p4/common/scripts/esp32p4_flat_memory.ld`、`esp32p4_sections.rev3.ld` |
| 改镜像封装 / 签名 / 槽位 | `nuttx/tools/espressif/espressif_mkimage.cmake`、`espressif_flash.cmake` |
| 改引导（MCUboot） | `nuttx/arch/risc-v/src/common/espressif/Bootloader.cmake`、`arch/risc-v/src/esp32p4/patches/` |
| 改摄像头底层（CSI/ISP） | `arch/risc-v/src/common/espressif/esp_csi.c`、`drivers/video/sc2336.c` |
| 改 AI Agent | `packages/ai_agent/src/{core,infra,channels,tools,llm}/` |
| 加一个 NSH 命令 | 在对应 `CMakeLists.txt` 里 `nuttx_add_application(NAME …)` |
| 构建 | `./build.sh <board>/configs/nsh/ --cmake -j8` |
| 构建报"配置没生效" | `rm -rf cmake_out`（`lunch` 不重建已有构建目录） |

---

## 九、自查命令合集（复制即用）

```bash
cd <openvela 根>

# 1) 工程组织
grep -c "<project" contest2026_342_ezdezhandui/openvela.xml
ls -la apps/packages                      # → ../packages 符号链接

# 2) 分层证据
ls frameworks/                            # connectivity graphics multimedia runtimes security system
ls vendor/                                # 厂商列表
ls prebuilts/                             # gcc clang-wasm cmake emulator qemu tools build-tools

# 3) 配置链
grep -n "MCUBOOT\|OTA_SLOT" nuttx/boards/risc-v/esp32p4/esp32p4-function-ev-board/configs/nsh/defconfig
grep -E "^CONFIG_BUILD_|^CONFIG_SMP_NCPUS" cmake_out/esp32p4-function-ev-board_nsh/.config

# 4) 构建链
readlink -f build.sh                      # → nuttx/tools/build.sh
sed -n '436p' build/envsetup.sh           # lunch()
grep -n "nuttx_add_application" packages/ai_agent/CMakeLists.txt

# 5) 链接构成
grep -o "LINK_LIBRARIES = .*" cmake_out/esp32p4-function-ev-board_nsh/build.ninja | head -1

# 6) 启动链（板子起来后）
python3 contest2026_342_ezdezhandui/tools/board.py reset --save /tmp/boot.log
grep -E "Mapped IROM|start=0x|NuttShell" /tmp/boot.log
```

---

## 十、术语表

| 术语 | 含义 |
|---|---|
| **NuttX** | Apache 实时内核；openvela 的内核基础 |
| **openvela** | 小米基于 NuttX 的 AIoT 操作系统发行版（含框架/包/厂商层） |
| **repo / manifest** | Google repo 工具 + XML 清单，用多仓拼工作区 |
| **copyfile** | manifest 里的文件重定向指令，把作品仓文件树映射到工作区路径 |
| **Kconfig / defconfig / .config** | 配置描述语言 / 人写的最小配置 / 解析后的最终配置 |
| **lunch / m** | openvela 构建函数：选板生成构建目录 / 调用 ninja |
| **flat / protected / kernel build** | 三种内核-应用关系模式（本项目用 flat） |
| **BSP** | Board Support Package：芯片层 + 板级 |
| **HAL** | 硬件抽象层（此处指 Espressif 官方 esp-hal-3rdparty） |
| **XIP** | eXecute In Place：代码在 flash 就地执行（本项目核心改造） |
| **MCUboot** | 二级引导 + 安全启动 + A/B OTA 框架 |
| **slot / scratch** | OTA 主/副槽与交换暂存区 |
| **NSH** | NuttX Shell（`nsh>`） |
| **builtin app** | 被编译进固件、可直接在 NSH 执行的"应用" |

---

> 相关文档：`docs/03_框架模块设计.md`（本项目的模块设计）、
> `docs/04_功能闭环测试.md`（五层测试体系与真机结果）、
> `docs/09_问题处理方案.md`（未决问题的处理方案）、
> `board/esp32p4_vela/README.md`（移植代码地图与复现步骤）。
