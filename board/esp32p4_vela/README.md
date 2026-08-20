# esp32p4_vela —— ESP32-P4 平台移植（新硬件适配赛道）

将 NuttX 上游 `esp32p4`（risc-v arch + esp32p4-function-ev-board）移植到 openvela
大赛分支 `dev-ai-contest-2026`，实现 **ESP32-P4X-C5-Function-EV-Board 串口 console 启动**。

> 形态说明：本目录是**真实文件树**（非 patch 文件）——`nuttx/` 镜像 openvela nuttx 仓根目录，
> 评审可直接浏览全部源码；`deploy.sh`/`export.sh` 保证可复现、防漂移。

## 一、部署与构建（评委复现路径）

```bash
# 1. 拉取 openvela 工作区（官方 manifest，gitee 源）
repo init -u https://gitee.com/open-vela/contest2026_342_ezdezhandui.git \
  -b dev-ai-contest-2026 -m contest2026_342_ezdezhandui.xml
repo sync -c -j8

# 2. 部署移植文件树（rsync 覆盖，含删除清单处理）
cd contest2026_342_ezdezhandui/board/esp32p4_vela
./deploy.sh ../../nuttx          # 或 cd 回工作区后 ./deploy.sh

# 3. 构建
cd ../../..
./build.sh nuttx/boards/risc-v/esp32p4/esp32p4-function-ev-board/configs/nsh/ --cmake -j8
# 产物：cmake_out/esp32p4-function-ev-board_nsh/nuttx.bin（ram-only 格式，269KB）
```

> ⚠️ HAL 版本锁定：`ESP_HAL_3RDPARTY_VERSION` 需 ≥ `8d0a89891008`（更旧的锁定 commit
> 无 esp32p4 组件：gpio_sig_map.h/irq.h/rom.eco5.ld 缺失）。deploy.sh 会提示。
> ⚠️ esptool 名字软链：`ln -sf ~/.local/bin/esptool.py ~/.local/bin/esptool`
> （CMake `find_program(ESPTOOL esptool esptool.py)` 在多 NAMES 下找不到 esptool.py，
> 否则 mkimage 报 "esptool.py elf2image failed"）。

## 二、开发流程（防漂移）

```bash
# 工作区改代码 → 编译验证 → 同步回文件树 → 提交作品仓
cd <工作区>/nuttx          # 改动、验证
<作品仓>/board/esp32p4_vela/export.sh <工作区>/nuttx
cd <作品仓> && git add -A && git commit -m "..."
```

- `export.sh`：`git diff <上游dev-ai-contest-2026>..HEAD` 自动同步改动文件；
  删除文件记入 `nuttx/.deleted-files`（deploy.sh 据此删除）
- `deploy.sh`：幂等 rsync 覆盖 + 按 `.deleted-files` 删除

## 三、代码地图（改动总览：284 文件 = 201 新增 + 74 修改 + 8 删除）

### 1. 芯片注册（新增 36 行）

| 文件 | 改动 | 目的 |
|---|---|---|
| `arch/risc-v/Kconfig` | M | +`ARCH_CHIP_ESP32P4` 块（select ARCH_RV32/ISA_M/A/C/VECNOTIRQ/BOOTLOADER），照 esp32c6 模式注册 |

### 2. 芯片层 `arch/risc-v/src/esp32p4/`（新增 8 文件）

| 文件 | 说明 |
|---|---|
| `Kconfig` / `CMakeLists.txt` / `Make.defs` / `hal_esp32p4.cmake` / `hal_esp32p4.mk` / `esp_chip_rev.c` | 上游芯片层 |
| **`esp32p4_atomic.c`（自写）** | 工具链 riscv-none-elf 无 libatomic.a，RV32IMC 无 64 位原子指令 → `__atomic_fetch_or_8` 用 up_irq_save/restore 自实现（SMP 需升级 spinlock） |
| `patches/0001-openvela-compat-os.c.patch` | HAL API 差异固化（nxtask_init 9→7 参、fcntl.h include、nxsched_usleep→nxsig_usleep），HAL populate 后由 CMake 自动 `git apply` |

### 3. 共享层 `arch/risc-v/src/common/espressif/`（新增 127 文件 + 修改）

| 改动 | 说明 |
|---|---|
| 上游 127 文件整体并入 | openvela 无 espressif 共享层 |
| `debug.h` 适配 46 处 | openvela fork 只有 `include/debug.h`（无 `nuttx/debug.h`），批量替换 include |
| `CMakeLists.txt` 修改 | HAL populate 后自动应用 patches/ 的兼容 patch（先 --check 再 apply，参照上游 mbedtls patch 机制） |
| 删除 8 文件（esp_dma/esp_hr_timer/esp_timer/esp_wifi_init/esp_wlan） | 上游新版本移除，CMakeLists 相应去除引用（见 `.deleted-files`） |

### 4. 芯片头文件 `arch/risc-v/include/esp32p4/`（新增 2 文件）

| 文件 | 说明 |
|---|---|
| `chip.h` / `.gitignore` | **移植易漏的 include 目录**——上游 arch 下配套头文件 |

### 5. 板级 `boards/risc-v/esp32p4/`（新增 113 文件 + 修改）

| 文件 | 说明 |
|---|---|
| `esp32p4-function-ev-board/` 全套 | 上游板级（含 35 个 configs） |
| **`esp32p4-function-ev-board/src/esp32p4_appinit.c`（新写）** | openvela BOARDCTL=y 时 boardctl.c 调 board_app_initialize——上游无此文件，照 esp32c6 模式新建 |
| `esp32p4-function-ev-board/configs/nsh/defconfig` | +`CONFIG_MM_KERNEL_HEAP=y`（kmm_* 符号）；console 切 USB Serial/JTAG（`CONFIG_ESPRESSIF_USBSERIAL=y`，UART0 关闭） |
| 板级 `debug.h` 替换 22 处 | 同共享层原因 |

### 6. 构建系统 `tools/espressif/`（新增 6 文件）+ `CMakeLists.txt` 修改

| 文件 | 说明 |
|---|---|
| `espressif_mkimage.cmake` 等 6 文件 | openvela 缺失（mkimage 报 "Error processing file" 即此类缺失）；**mkimage 已支持 ram-only header**（CONFIG_ESPRESSIF_SIMPLE_BOOT=y 时自动加 `--ram-only-header`，P4 ROM 必需） |
| `nuttx/CMakeLists.txt`（M） | +`NUTTX_BINARY_DIR` 定义（HAL 路径解析）；LD_SCRIPT 多文件 foreach 支持（esp32p4 有 12+ ROM ld 脚本） |
| `arch/risc-v/src/common/CMakeLists.txt`（M） | `riscv_mtimer.c` 改 `if(CONFIG_ONESHOT)` 条件编译（CONFIG_ONESHOT 未开时 oneshot_operations_s 无该成员） |

## 四、烧录与验证（JTAG）

```bash
# 1. 板子按 Reset（运行模式，下载模式 JTAG 关闭）
cd /tmp/openocd-esp32/share/openocd/scripts
nohup /tmp/openocd-esp32/bin/openocd -f interface/esp_usb_jtag.cfg -f target/esp32p4.cfg \
  > /tmp/openocd.log 2>&1 &

# 2. telnet 4444
#    flash probe 0 → flash write_image erase <nuttx.bin> 0x0 → reset run

# 3. 停 OpenOCD（否则占用 USB，ttyACM0 消失）
# 4. 读 ttyACM0 看 nsh> 提示符（console 在 USB Serial/JTAG 口，115200）
```

## 五、已知环境坑（详见 docs/踩坑笔记_05_esp32p4移植.md）

1. HAL 锁定版本 ≥ 8d0a898（旧锁定无 esp32p4 组件）
2. `nuttx/debug.h` → `include/debug.h`（68 处替换，含板级）
3. HAL 在 cmake_out，rm 后重 clone 会丢手工修改——patch 机制必须经 CMakeLists 固化
4. riscv-none-elf 无 libatomic / 无 gdb（调试用 OpenOCD telnet 4444）
5. esptool 软链（见上）；CMake 3.23 `find_program` 多 NAMES 行为异常
6. `esp32p4_sections.rev3.ld` "contains output sections" warning 无害
7. 完整文件清单：`git diff-tree -r --name-status f67714ba` 或对照本文件树
