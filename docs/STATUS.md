# 状态交接文档（2026-08-20 12:00）——新会话从这里继续

> 项目：openvela AI 硬件大赛（小米）· 桌伴 DeskMate · ESP32-P4X-C5-Function-EV-Board
> 新会话指令：**"继续 openvela 大赛项目，先读 docs/STATUS.md"**
> 工作区已重做：**官方 manifest 布局**（openvela 树在顶层 + 专属仓在 `contest2026_342_ezdezhandui/`）

## 〇、工作区布局（2026-08-20 已重做）

```
/home/ez/share/rk3576-openvela/
├── nuttx/ apps/ vendor/ packages/ prebuilts/ ...   ← openvela 树（repo sync -c，官方 manifest）
├── build.sh  emulator.sh                           ← 构建入口（在顶层，不是 openvela/ 子目录！）
├── contest2026_342_ezdezhandui/                    ← 大赛专属仓（git 仓，作品代码）
│   ├── board/esp32p4_vela/                         ← 移植补丁 0001-esp32p4-port.patch + README
│   ├── docs/                                       ← 规划 V5.1 + 踩坑笔记 #01/#04/#05 + STATUS
│   ├── .claude/skills/                             ← 17 个官方 skill（已提交）
│   └── logs/                                       ← AI 日志（contest-log-collector 导出）
├── openvela/                                       ← 旧树（52GB，已验证废弃，待删除确认）
├── docs/  README.md  nuttx-apps-upstream/          ← 旧外围（整理中）
└── .repo/                                          ← repo 元数据（顶层）
```

- **构建命令已变**：`cd /home/ez/share/rk3576-openvela && ./build.sh nuttx/boards/risc-v/esp32p4/esp32p4-function-ev-board/configs/nsh/ --cmake -j8`
- 产物：`cmake_out/esp32p4-function-ev-board_nsh/nuttx.bin`（**mkimage 已自动生成 ram-only 格式**——新树官方脚本支持 `CONFIG_ESPRESSIF_SIMPLE_BOOT=y` 时加 `--ram-only-header`，构建输出 `Image has only RAM segments visible` 即正确；STATUS 旧版"需手动 elf2image"问题已消失）

## 一、已完成（全部验证）

1. **esp32p4 移植完成且在新树重放**：commit `f67714ba`（nuttx 仓，含 Kconfig 注册——8/20 补漏）+ 编译通过 + ram-only 镜像 269KB 生成（与旧树手动生成格式一致）
   - ⚠️ 移植改动在 nuttx 仓本地 commit（detached HEAD），备份：`/tmp/esp32p4-port.patch`（72552 行，含 Kconfig）
2. **作品仓已提交并 PR**：fork `ez-xu/contest2026_342_ezdezhandui` → PR #1（open-vela/contest2026_342_ezdezhandui）
   - ⚠️ **PR 卡在 CLA**：需在 https://openvela.com/#/community/cla 签署 → PR 评论 `/check-cla` 复检 → `gh pr merge 1 --repo open-vela/contest2026_342_ezdezhandui`
   - ⚠️ 远端 commit `4f959cc9` 与本地 `e221b054` 内容等价但 sha 不同（GitHub 时区规范化）——后续 push 需 force 或先 git fetch 对齐
3. **烧录链路**：esptool 5.3.1（`/home/ez/.local/bin/esptool.py`）+ 4.10（`python3 -m esptool`）；**已建软链 `~/.local/bin/esptool → esptool.py`（必须！CMake find_program 多 NAMES 找不到 esptool.py，mkimage 会失败）**
4. **板卡信息**：芯片 **ESP32-P4 rev3.2**、flash GD25Q128E（16MB Quad SPI）、MAC e8:f6:0a:e3:a6:a5
5. **ROM 输出确认**（ttyACM0）：ESP-ROM:esp32p4-eco7-20260109；ram-only 镜像曾被 ROM 接受并加载（app 早期 crash @0x25，加载链路通）

## 二、当前障碍（烧录/启动）

| 问题 | 状态 |
|---|---|
| esptool **stub 崩溃**（rev3.2 不兼容） | ❌ 5.3.1/4.10 都崩——stub 不可用 |
| ROM 模式（--no-stub）USB 擦写 | ❌ 断流/超时 |
| UART0 下载模式 | ❌ ROM 无响应 |
| **JTAG 烧录（OpenOCD）** | ⏳ openocd-esp32 在 /tmp/openocd-esp32（v0.12.0-esp32-20260703），esp32p4 flash 支持待实测 |
| **0x25 crash 分析** | ⏳ **优先做**：反汇编 nuttx ELF 定位 load access fault @0x25（不依赖板子，可能比 JTAG 更快） |

## 三、下一步

1. **签 CLA → PR 合入**（用户操作，5 分钟）
2. **分析 0x25 crash**：`riscv-none-elf-objdump -d cmake_out/esp32p4-function-ev-board_nsh/nuttx` 找 PC=0x25 附近代码
3. JTAG 烧录：板子按 Reset（运行模式）→ OpenOCD（esp_usb_jtag.cfg + esp32p4.cfg）→ telnet 4444 flash write → 停 OpenOCD → 读 ttyACM0 看 nsh>
4. 若 JTAG 不行：查 espressif/esptool issue "esp32p4 rev3.2 stub crash" 或 ESP-IDF 新版 esptool

## 四、环境/权限（已解决）

- USB 权限：udev 已写（303a → 0666）；串口：ttyACM0（console）、ttyUSB0（UART0）
- 访问串口需 `sg dialout -c "..."`；esptool 能自动控制下载模式
- gh 已登录（ez-xu，repo 权限）；github.com:443 直连间歇不通（api.github.com 可通——push 用 Git Database API 方案 /tmp/git-api-push.py）

## 五、关键文件

| 路径 | 说明 |
|---|---|
| `cmake_out/esp32p4-function-ev-board_nsh/nuttx.bin` | **正确格式镜像（烧这个，ram-only）** |
| `contest2026_342_ezdezhandui/board/esp32p4_vela/` | 移植补丁 + 构建/烧录说明 |
| `contest2026_342_ezdezhandui/docs/` | 规划 + 踩坑笔记 + STATUS（同步更新） |
| `/tmp/esp32p4-port.patch` | 移植补丁备份（72552 行） |
| `/tmp/port-backup/` | nuttx.bin + nuttx_ramonly.bin 备份 |
| `/tmp/openocd-esp32/` | OpenOCD（JTAG 烧录用） |
| `/tmp/git-api-push.py` | GitHub API push 脚本（github.com 不通时用） |

## 六、待办（烧录通过后）

1. 串口点亮验证（nsh> 提示符）→ **T1 完成**
2. T2：ETH → 屏/触摸 → codec → 摄像头；ai_agent 集成（MiMo 对话）
3. 里程碑：9/10 演示冻结、9/18 提交
4. 旧树 openvela/（52GB）删除确认；nuttx-apps-upstream/ 处置
