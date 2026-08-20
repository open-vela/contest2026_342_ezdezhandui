# esp32p4_vela —— ESP32-P4 平台移植（新硬件适配赛道）

将 NuttX 上游 `esp32p4`（risc-v arch + esp32p4-function-ev-board 板级）移植到 openvela
大赛分支 `dev-ai-contest-2026`，实现 **ESP32-P4X-C5-Function-EV-Board 串口 console 启动**。

## 移植内容

| 位置（openvela nuttx） | 内容 |
|---|---|
| `arch/risc-v/Kconfig` | +`ARCH_CHIP_ESP32P4` 注册块（select RV32/ISA/BOOTLOADER，照 esp32c6 模式） |
| `arch/risc-v/src/esp32p4/` | 上游 6 文件 + 自写 `esp32p4_atomic.c`（工具链无 libatomic，RV32IMC 无 64 位原子指令 → up_irq_save 保护自实现）+ `patches/0001-openvela-compat-os.c.patch`（HAL API 差异固化） |
| `arch/risc-v/src/common/espressif/` | 上游共享层 127 文件 + debug.h 适配（openvela fork 无 `nuttx/debug.h`，68 处替换）+ HAL patch 自动应用机制 |
| `arch/risc-v/include/esp32p4/` | 上游 2 文件（include 目录易漏） |
| `boards/risc-v/esp32p4/` | 上游 113 文件 + 新写 `esp32p4_appinit.c`（openvela BOARDCTL 需要） |
| `tools/espressif/` | 补 6 个缺失文件（openvela 无 mkimage 等脚本） |
| `nuttx/CMakeLists.txt` | +`NUTTX_BINARY_DIR` 定义；LD_SCRIPT 多文件 foreach 支持 |
| `arch/risc-v/src/common/CMakeLists.txt` | `riscv_mtimer.c` 改 `if(CONFIG_ONESHOT)` 条件编译 |

## 应用移植补丁

```bash
cd <openvela>/nuttx
git am 0001-esp32p4-port.patch
```

## 构建

```bash
cd <openvela>
./build.sh nuttx/boards/risc-v/esp32p4/esp32p4-function-ev-board/configs/nsh/ --cmake -j8
```

产物：`cmake_out/esp32p4-function-ev-board_nsh/nuttx.bin`

> ⚠️ P4 的 ROM 只认 **ram-only-header 格式**镜像；本树 `tools/espressif/espressif_mkimage.cmake`
> 已支持 `CONFIG_ESPRESSIF_SIMPLE_BOOT=y` 时自动加 `--ram-only-header`（构建输出
> `Image has only RAM segments visible` 即正确格式）。

## 烧录与验证（JTAG）

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

## 已知环境坑

- **esptool 名字**：`espressif_mkimage.cmake` 的 `find_program(ESPTOOL esptool esptool.py)`
  在 CMake 3.23 下多 NAMES 找不到 → 需 `ln -sf ~/.local/bin/esptool.py ~/.local/bin/esptool`
- **HAL 版本锁定**：`ESP_HAL_3RDPARTY_VERSION` 需 ≥ 8d0a898（旧锁定无 esp32p4 组件）
- 工具链：riscv-none-elf 无 gdb / 无 libatomic（详见 docs/踩坑笔记_05_esp32p4移植.md）
