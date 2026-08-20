# 状态交接文档（2026-08-19 12:15）——新会话从这里继续

> 项目：openvela AI 硬件大赛（小米）· 桌伴 DeskMate · ESP32-P4X-C5-Function-EV-Board
> 新会话指令：**"继续 openvela 大赛项目，先读 docs/STATUS.md，然后执行 JTAG 烧录验证"**

## 一、已完成（全部验证）

1. **esp32p4 移植完成**：编译通过 + nuttx.bin 生成（踩坑笔记 #05：`docs/踩坑笔记_05_esp32p4移植.md`，9 个坑）
   - 命令：`cd /home/ez/share/rk3576-openvela/openvela && ./build.sh nuttx/boards/risc-v/esp32p4/esp32p4-function-ev-board/configs/nsh/ --cmake -j8`
   - 产物：`cmake_out/esp32p4-function-ev-board_nsh/nuttx`（ELF）+ `nuttx.bin`
   - **console 已切 USB Serial/JTAG**（defconfig：`# CONFIG_ESPRESSIF_UART0 is not set` + `CONFIG_ESPRESSIF_USBSERIAL=y`）→ **启动日志在 ttyACM0，不在 ttyUSB0**
2. **烧录链路**：esptool 5.3.1（`/home/ez/.local/bin/esptool.py`）+ 4.10（`python3 -m esptool`）都装了
3. **板卡信息**：芯片 **ESP32-P4 rev3.2**、flash **GD25Q128E（16MB Quad SPI）**、MAC e8:f6:0a:e3:a6:a5
4. **ROM 输出确认**（ttyACM0 读到）：
   ```
   ESP-ROM:esp32p4-eco7-20260109
   rst:0x7 (HP_SYS_HP_WDT_RESET), boot:0xf (SPI_FAST_FLASH_BOOT)
   invalid header: 0xc6b7c290   ← 旧镜像头问题（普通头 ROM 不认）
   ```
   **关键结论：P4 的 ROM 需要 `--ram-only-header` 格式镜像**（`esptool elf2image --ram-only-header -fs 16MB -fm qio -ff 80m`）——**已生成 nuttx_ramonly.bin**（`cmake_out/esp32p4-function-ev-board_nsh/nuttx_ramonly.bin`）
   - **注意：mkimage 构建产物的 nuttx.bin 不是 ram-only 格式**（cmake 变量 CONFIG_ESPRESSIF_SIMPLE_BOOT 未传到 mkimage）——**烧录必须用 nuttx_ramonly.bin**（或修 mkimage 脚本）
5. **ram-only 镜像被 ROM 接受并加载**（曾经观察到 app 启动 + 异常 dump：MCAUSE load access fault @0x25——app 早期 crash 但**加载链路通了**）

## 二、当前障碍（烧录/启动）

| 问题 | 状态 |
|---|---|
| esptool **stub 崩溃**（Guru Meditation Load access fault） | ❌ 与 rev3.2 不兼容（5.3.1 和 4.10 都崩）——**stub 不可用** |
| ROM 模式（--no-stub）USB 擦除/写入 | ❌ 擦除断流（Serial data stream stopped）、写入 Write timeout——**P4 ROM 的 USB flash 操作不稳定** |
| UART0 下载模式（ttyUSB0） | ❌ ROM 无响应 |
| **JTAG 烧录（OpenOCD）** | ⏳ **推荐路径**：openocd-esp32 在 `/tmp/openocd-esp32/bin/openocd`（v0.12.0-esp32-20260703），esp32p4 flash 支持确认（esp32p4.cfg 有 stub flasher workarea） |
| JTAG 连接 | ⚠️ 板子必须**运行模式**（下载模式 JTAG 关闭）——按 Reset（不按 BOOT）后连 |

## 三、下一步（JTAG 烧录，核心操作）

```bash
# 1. 板子按 Reset（运行模式）→ 启动 OpenOCD
cd /tmp/openocd-esp32/share/openocd/scripts
nohup /tmp/openocd-esp32/bin/openocd -f interface/esp_usb_jtag.cfg -f target/esp32p4.cfg > /tmp/openocd.log 2>&1 &
sleep 10; grep "Examination succeed" /tmp/openocd.log

# 2. telnet 4444 烧录（镜像 = nuttx_ramonly.bin）
#    flash probe 0 → flash write_image erase /home/ez/share/rk3576-openvela/openvela/cmake_out/esp32p4-function-ev-board_nsh/nuttx_ramonly.bin 0x0 → reset run

# 3. 停 OpenOCD（否则占用 USB，ttyACM0 消失）
# 4. 读 ttyACM0 看 nsh> 提示符（console 在 USB Serial/JTAG 口）
```

**若 JTAG flash 烧录也不支持**（openocd-esp32 对 esp32p4 flash 支持待实测）→ 备选：
- 修 esptool stub 兼容（查 espressif/esptool issue "esp32p4 rev3.2 stub crash"）
- 或找乐鑫官方 flash 工具（ESP-IDF 自带 esptool 版本可能更新）

## 四、环境/权限（已解决）

- **USB 权限**：udev 规则已写 `/etc/udev/rules.d/99-esp.rules`（idVendor 303a → 0666）——**插拔不用再 chmod**
- 串口：ttyACM0（USB-Serial/JTAG console）、ttyUSB0（USB-TTL → UART0 GPIO37/38，交叉接线 + 共地）
- 访问串口需 `sg dialout -c "..."`（ez 已加 dialout 组，新 shell 生效；当前会话用 sg）
- **esptool 能自动控制下载模式**（--before usb_reset / default_reset）——**用户不用按 BOOT+Reset**（除 ROM 模式特殊场景）

## 五、关键文件

| 路径 | 说明 |
|---|---|
| `openvela/cmake_out/esp32p4-function-ev-board_nsh/nuttx_ramonly.bin` | **正确格式镜像（烧这个）** |
| `docs/踩坑笔记_05_esp32p4移植.md` | 移植全记录（9 坑） |
| `docs/开发规划文档.md` | 方案 V5.1（赛题四点/评分/里程碑） |
| `docs/踩坑笔记_04_goldfish_AI_Agent.md` | goldfish 上 ai_agent 验证（T1 已完成部分） |
| `/tmp/openocd-esp32/` | OpenOCD（JTAG 烧录用） |
| 工具链 gdb | 无（riscv-none-elf 无 gdb）——调试用 OpenOCD telnet 4444 |

## 六、待办（烧录通过后）

1. 串口点亮验证（nsh> 提示符）→ **T1 完成**
2. T2：ETH → 屏/触摸 → codec → 摄像头；ai_agent 集成（MiMo 对话）
3. 里程碑：9/10 演示冻结、9/18 提交（专属仓 contest2026_<编号>_<队伍名>）
