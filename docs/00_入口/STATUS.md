# 项目状态（STATUS）

> 最后更新：**2026-11-04** · **修掉"进不去 NSH / 整机失聪"的平台级缺陷（P0）**：
> 根因是 `up_exit()` 的换出 ECALL 被通用层 `riscv_doirq()` 的"无当前任务就丢弃"守卫吃掉
> ——`g_running_task == NULL` 在退出路径上是**合法状态**，那条 ECALL 被丢弃后换出永不发生，
> ECALL 被无限重放（真机实测 **~9e5–1.6e6 次/秒**），tick/网络/控制台全部饿死。
> 修复在**芯片层** `arch/risc-v/src/esp32p4/espressif/esp_irq.c`（进通用层前把当前任务补回
> 就绪队首；通用层零写入）。**真机验证**：PC 采样 trap 比例 8/8 → **0/8**；`NuttShell (NSH)`
> 正常；`deskmate --agent mock --camera --selftest` **PASS (27/27)**；5 分钟存活探测全通过。
> 诊断链与可复用方法论见 `docs/50_排障经验/11_ECALL换出风暴与异常被丢弃.md`，
> 现场与前后对照见 `logs/20261104-deliver-verify/`。
>
> 同轮附带：`esp_sdmmc.c` 的 HLE（电平型控制器错误）改为 ISR 内屏蔽 + 下个传输重新武装，
> 避免控制器级中断重入；`esp_irq.c` 增加 `ESP_WEDGE_DIAG` 的"风暴期冻结异常现场"
> （`g_dbg_exc_frozen_*`，默认关闭）。

> 最后更新：**2026-11-02** · **应用层中文化 + 真机移植**：DeskMate 中控 UI 已全中文
> （自绘 CJK 子集字体 `dm_font_16/24`，字形数据 15.4 KB + 29.2 KB，P4 镜像余量 491 KB；
> 新增确定性断言 `font:cjk-body/title` 逐字查轮廓，模拟器 **PASS 18/18**）。
> 真机（ESP32-P4）已烧录并跑起来：中文/触摸/三页快照/切页/网口 全 PASS；上板暴露并修掉
> 4 个真问题（快照目录无 hostfs、相机首帧空帧导致 canvas 断言、RGB565 start_capture 失败、
> 板级缺无线 ioctl 层导致看不到 wlan0）。详见 `docs/30_实现/应用层_DeskMate_UI.md` §八/§九。

> 最后更新：**2026-11-01** · **应用层（DeskMate 中控 UI）落地，并新增双模拟器验证轨**：
> 新增应用 `apps/examples/deskmate`（→ `app/apps/examples/deskmate/`）——把**摄像头 /
> WiFi / 网口 / 触摸屏**四条能力收进一个 LVGL 中控界面（首页 / 相机 / 对话 / 网络 / 设置
> 五页 + agent bridge + 帧差主动事件），并把 HAL（显示 / 触摸 / 相机 / 网络）做成
> 「真机后端 + 模拟器后端」可切换，于是**同一套代码可在真机与模拟器上跑通 UI 与逻辑**。
>
> 新增两条模拟器路径：`nuttx/boards/sim/sim/sim/configs/deskmate`（宿主二进制、无头、
> 一条命令 `tools/test/sim_deskmate.sh` 出证据）与 `board/vela/...`（goldfish 官方模拟器
> 验收配置，manifest 映射到 `vendor/openvela/boards/vela/`）。
> 模拟器实测：**`DM-SELFTEST RESULT: PASS (16/16)`**——5 页建成、注入点击命中真实按钮、
> 点 Tab 真切页（帧指纹不同）、`wlan0 UP`、**WiFi 扫描经 WAPI 返回真实 AP**、
> 相机合成源出帧并触发主动事件、每页快照 RAW RGB565 → PNG（色数 86~91、梯度 1）。
> 过程中修掉一处上游缺陷：`nuttx/drivers/net/wifi_sim.c` 调用 `nxmutex_*` 却缺
> `#include <nuttx/mutex.h>`（flat 构建链接失败），见
> `docs/30_实现/ai_agent与Skill.md` §4.2。
> 详细设计与证据判据见 **`docs/30_实现/ai_agent与Skill.md`**。

> 最后更新：**2026-10-23** · **ES8311 音频（I2S+GDMA）驱动链路已打通并编入固件**：
> 新增板级粘合 `board/esp32p4/esp32p4-function-ev-board/{include,src}/esp32p4_audio_es8311.*`
> （唯一入口 + 功放控制两个函数），`/dev/audio/pcm0` 经 `pcm_decode` 注册，
> 语音播放入口用 NSH 内置 `nxplayer`；引脚取自**官方原理图**
> （MCLK13/BCLK12/WS10/DOUT9/DIN11/PA53，⚠️ 两个数据脚按 codec 视角命名、与主机相反）。
> 过程中修复 **4 处上游潜伏缺陷**（`esp_i2s.c` 缺 wqueue/kmalloc/mutex 三个头、
> `es8311.c` 缺 mutex.h、板级 Kconfig `select` 穿透不了 `menuconfig`、
> 陈旧 `config.h` 引发的假象）——详见 `docs/30_实现/音频_ES8311.md`。
> 构建实测：0 error，`nuttx.bin` 1,835,008 B，音频 5 个符号 + `nxplayer_main` 全部落位，
> 调用链 `esp_bringup → esp32p4_audio_es8311_initialize` 已反汇编核实。
> **用标准播放器复核发现第二个缓存缺陷（10/26 下午，已修）**：`audiotest -w`
> 生成真 WAV → `nxplayer` 播放，暴露出 `esp_i2s.c` 的发送缓存回写长度
> （8148 字节）不是 cache line（64）的整数倍，`esp_cache_msync()` 直接拒绝 →
> DMA 可能读到未回写的陈旧数据。已修（分配与回写都按整条 line 对齐）。
> ✅ **`nxplayer` 崩溃已定位并修复（10/26 晚）**：上一轮"`device` 子命令崩溃"的
> 结论**是错的** —— 真因是**任何不带参数的命令**（`q`/`h`/`stop`/`pause`）都会崩：
> `strtok_r()` 在"令牌后无内容"时把 `*saveptr` 置 NULL，而
> `nxplayer_main.c` 紧接着 `while (*arg == ' ')` 直接解引用 → NULL 解引用
> （反汇编指认：`lbu a3,0(a5)`，`a5=arg=NULL`，MTVAL=0）。之前的 panic 全部来自
> 脚本末尾那个 `q`，被误读成 `device` 的问题。
> 已修 `nxplayer_main.c`；同一缺陷在 `nxrecorder_main.c` 也一并修了（未编入配置，
> 属预防性修复）。真机复验：`nxplayer` → `device pcm0` → `play` → `q`
> **0 panic / 0 error 干净退出**，`h` 也正常。这是 `apps/system/nxplayer` 的
> 通用缺陷，影响面远大于本项目。详见 `docs/30_实现/音频_ES8311.md` §B.2。
>
> **音频真机全链路跑通（10/26）**：`audiotest` 完整播放 192044 字节
> （2 秒 / 24 kHz / 16bit 立体声）并正常收尾，扫频/880Hz/16kHz/3 秒等变体全部通过。
> 收尾卡住的两个真因都在自检程序（`mq_msgsize` 设大导致每次 `mq_receive` 直接
> `EMSGSIZE`；发流循环永远只用 `buffers[0]` 重复入队）—— 驱动侧一直是对的。
>
> **第 16 轮 —— C6 已刷入 SDIO 固件，但枚举仍未通**：
> C6 进入下载模式（识别为 **ESP32-C6FH4 (QFN32) rev v0.2**），按
> `flasher_args.json` 的真实布局烧入 4 个镜像（bootloader@0x0、
> partition-table@0x8000、otadata@0xd000、app@0x10000），
> `Hash of data verified`。
> **固件确实换了**：刷写前 C6 的 UART 每 5 秒吐一次串口传输统计，刷写后
> **完全静默** —— 跑起来的正是 SDIO 版本。
> **但 P4 仍枚举不到**：探测窗口放宽到 20 次 × 250 ms = 5 秒，全部 `-110`。
> 剩余怀疑对象：① C6 侧 SDIO 引脚（固件用默认 18/19/20/21/22/23，
> 本板实际接线始终未获原理图级确认）；② 从机固件
> `CONFIG_EH_TRANSPORT_CP_SDIO_GPIO_RESET=-1`。详见 `docs/30_实现/连接_WiFi_SDIO.md` 附录 F。
>
> 顺带修掉一个方法论问题：原来只探一次，测的是"C6 启动多快"而非"链路通不通"。
>
> **第 13 轮 —— C6 的 SDIO 从机固件已构建完成**：
> 用 ESP-IDF 5.5.6 + `esp-hosted-mcu` 的 `examples/wifi/sta/cp` 构建出
> `eh_cp_wifi_sta.bin`（**1,094,464 B**，`CONFIG_IDF_TARGET="esp32c6"`、
> `CONFIG_EH_TRANSPORT_CP_SDIO=y`、`SW_AGGR` 模式，构建完整）——
> 这就是让 C6 从"串口传输"变成"SDIO 传输"所需的固件。
> 已随交付收录在 `tools/build_flash/c6_hosted/`（固件 + sdkconfig + 烧录脚本）。
> ⚠️ **烧录仍需一次人工操作**：按住 C6 的 BOOT → 按 RESET → 松开，
> 因为这颗 CP2102 的 DTR/RTS 没接 EN/BOOT，esptool 无法自动进下载模式。
> 详见 `docs/30_实现/连接_WiFi_SDIO.md` §E.6。
>
> **第 12 轮 —— 拿到 esp-hosted 上游源码并核对**：成功拉取
> `espressif/esp-hosted-mcu`（从机固件 + 主机驱动 + 协议）。
> **官方 P4 主机引脚表与本移植完全一致**（CLK18/CMD19/D0-17…/Reset 54），
> 当初从原理图独立推出的结论得到印证。
> 从机固件的构建路径已确认（`examples/wifi/sta/cp` + `eh.py set-target esp32c6`，
> 实测 `CONFIG_EH_TRANSPORT_CP_SDIO=y` 默认选中）；构建目前只卡在
> `protobuf-c` 子模块因网络 TLS 中断未拉全。
> 已抄录 esp-hosted 主机的 **SDIO 卡功能合约**（CCCR 使能/就绪轮询/中断/
> FN0·FN1 块大小 512/4-bit），并据此把 `esp_hosted_sdio.c` 从"只做 sdio_probe"
> 补成**完整的卡功能 bring-up**（复用 NuttX 的 `sdio_enable_function` /
> `sdio_set_blocksize` / `sdio_set_wide_bus`），编译 0 error。
> 详见 `docs/30_实现/连接_WiFi_SDIO.md` 附录 E。
>
> **第 11 轮 —— WiFi 阻塞原因查明**：`/dev/ttyUSB1` 就是**板载 ESP32-C6-Mini-1
> 的串口**（CP2102）。直接读它发现 C6 跑的是 **esp-hosted 从机，但走「串口」传输**：
> `stats: ... serial : in: 0 rsp: 0 evt: 0`。
> **这就是 SDIO 永远 -110 的原因 —— C6 根本不在 SDIO 传输模式上。**
> 原理图核实：P4→协处理器只有 `P4_WAKEUP_C5` 一条信号，C6 的串口只到编程排针/
> CP2102（**没接到 P4**），所以 P4↔C6 的数据通道只有 SD2_* 这组 SDIO。
> 结论：**必须把 C6 刷成 SDIO 传输的 esp-hosted 从机固件**。
> 但 esptool 在 ttyUSB1 上无法自动进入下载模式（DTR/RTS 未接 EN/BOOT，
> 三种极性组合均失败）→ **需要人工按 BOOT+RESET**。详见 `docs/30_实现/连接_WiFi_SDIO.md` 附录 D。
>
> **第 10 轮**：麦克风采集定位到具体环节 —— 加 `syslog` 打点（`-DNDEBUG` 会编掉
> `mcerr`/`audinfo`）后确认真机走的是 `mode=1`（ADC，方向正确）、4 个缓冲区全部
> 入队，但 **`inflight` 再不回落** → `I2S_RECEIVE` 的完成回调从未触发、**RX DMA
> 从未完成**。已排除配置（RX=1、DIN=11）、方向、入队逻辑、`rxdma_start` 未启动通道；
> 缩小到 master 模式下 RX 通道拿不到时钟（本板 TX/RX 同时使能，走"两者都开"的
> 配置路径）。仍**未注册** `/dev/audio/pcm_in0`（会卡 console），播放复测正常。
> 详见 `docs/30_实现/音频_ES8311.md` §C.5。
>
> **本轮（第 8 轮）**：WiFi 侧继续排除 —— `C5_BOOT` 未连到 P4（原理图确认）、
> 使能复位脉冲无效（真机三次仍 -110），软件侧手段已用尽。
> 音频侧尝试**麦克风采集**：修掉 `es8311.c` INPUT 分支同款 `ERANGE` 缺陷
> （`input CONFIGURE failed: 34` → 通过），但**采集数据流仍不通**（入队缓冲区
> 不归还、会占住 console），因此**未注册** `/dev/audio/pcm_in0`
> —— 播放路径复测正常，非回归。详见 `docs/30_实现/音频_ES8311.md` 附录 C。
>
> **SDIO 卡时钟未提交 —— -EBUSY 真根因已定位并修复（10/26 晚）**：
> DesignWare MSHC 的 CLKDIV/CLKSRC/CLKENA 只写进 BIU，必须再发一条
> `update_clk_reg=1` 的命令才提交到卡时钟域 —— 原实现漏了这一步，
> **卡时钟从未真正启动**，命令发不出去、START 位永不自清 → 恒 -EBUSY。
> 欺骗性在于三个寄存器读回来全都"正确"（`clkena=0x2`、`clkdiv=0x9f00`、
> `verid=0x5342270a` 正常），但 `rintsts=0` 一次完成都没有。
> 修复后真机返回码 **-16 → -110**：命令通道已通，变成"对端不应答"。
> 另排除使能极性（高低都试）、使能时长（3 次尝试含 +800ms）。
> 新增 `esp_sdmmc_debug()` / `esp_sdmmc_bus_reset()` 与三次带极性对调的枚举探测。
> 详见 `docs/30_实现/连接_WiFi_SDIO.md` 附录 C。
>
> **SDMMC/SDIO 驱动首次上板（10/26）**：P4 SDMMC 控制器在真机**成功初始化**
> （`SDIO host ready on slot 1`），协处理器使能 GPIO54 可正常驱动。
> 上板暴露并修复了 2 个问题：探测前未使能卡时钟/未挂中断（→ 命令永远完不成，
> 表现为 -EBUSY）、协处理器需先上电。**枚举仍未成功**（仍 -16），
> 最可能的原因是 C5 模组上没有 esp-hosted 从机固件（这是阶段 B 的既定前置条件）。
> 详见 `docs/30_实现/连接_WiFi_SDIO.md` §B。
>
> **音频已上板验证（10/23）**：ES8311 在真机注册成功（`/dev/audio/pcm0`），
> I2C 0x18 应答、I2S 时钟正确（MCLK 6.144 MHz = 24000×256）、`AUDIOIOC_CONFIGURE`
> 成功、DMA 传输完成并归还缓冲区（`AUDIO_MSG_COMPLETE` 实测出现）。
> 上板过程中定位并修复了 **4 个只在真机上才暴露的驱动缺陷**（详见 `docs/30_实现/音频_ES8311.md` 附录）：
> ① `es8311.c` 用 `ERANGE` 拒绝合法配置（三元表达式丢弃 `OK`，INPUT/OUTPUT 两处）
> ② `esp_i2s.c` cache 对齐循环 uint32 下溢到 ~13 亿（**任何 WAV 播放必踩**）
> ③ `esp_i2s.c` carry 缓冲区仅 4 字节却最多拷 64 字节，**写坏自己的长度字段**
> ④ DMA 缓冲区按 `nbytes` 分配却按 `samp_size` 写入 → 堆越界
> 另新增板端自检程序 `audiotest`（NSH 内置，在 RAM 里合成 PCM 直接喂 codec）。
> ⚠️ 自检程序本身仍在收尾记账上卡住（不影响音频通路，详见 `docs/30_实现/音频_ES8311.md` §A.4）。
>
> **WiFi 第 1 层已实现**：板载为 **ESP32-C5-MINI-1 + SDIO + ESP-Hosted**（**非 C6**，此前记载有误）。
> 新增 P4 SDMMC/SDIO host 驱动 `esp_sdmmc.c`（完整 `sdio_dev_s` 方法表 + IDMAC 描述符链 +
> CLIC 中断 + 双 slot 引脚路由）与枚举层 `esp_hosted_sdio.c`；
> 开启开关后实测 **0 error**、5 个符号落位、调用链
> `esp_bringup → esp_hosted_sdio_initialize → esp_sdmmc_initialize → sdio_probe` 已反汇编核实。
> 顺带修掉 `drivers/mmcsd/CMakeLists.txt` 漏编 `sdio.c` 的上游 CMake 移植缺陷。
> ⚠️ **第 2/3 层（ESP-Hosted 协议 + 远端 netdev）未实现**，故默认关闭、打开也上不了网；
> 联网仍靠 eth0。详见 `docs/30_实现/连接_WiFi_SDIO.md`。
> **摄像头出帧已修复（10/22）**：根因 = ESP32-P4 的 **CSI bridge 与 ISP 共享**
> （`SOC_ISP_SHARE_CSI_BRG=1`）而 openvela 从未初始化 ISP 输入模块；修复见
> `nuttx/arch/risc-v/src/esp32p4/drivers/esp_csi.c`（`esp_csi_isp_bypass_init()`），
> 验收 `tools/test/camera_verify.sh` **PASS 13/13**、抓帧 15/15（逐段结论修正史见 §四.4）。
> 新增板端 V4L2 工具 `v4l2ctl`（`app/apps/system/v4l2ctl/`，manifest → `apps/system/v4l2ctl/`）。
> 2026-09-18 提交前基线（保留）：**「agent 一启动整机失聪」已定位并修复**：`riscv_doirq()` 的早期引导保护
> 吞掉了 `up_exit()` 的 `SYS_restore_context` ECALL（任何任务退出都会死机），修复后 agent 启动完成
> 板子照常在线（ping/TCP 28789 均通）—— 根因、证据、验证见 `docs/90_归档/开发过程复盘与改进清单.md` §26.7 与 `logs/verify-2026-09-17/wedge_fix.txt`；
> LVGL 截图在最终固件上复采（同一工具 `tools/test/fb2png.py`）；
> §二/§三 收拢到**无部署步骤**的交付路径（manifest copyfile 自动映射，见 `docs/90_归档/开发过程复盘与改进清单.md` §24）。
> §五 外设状态按 9/14–9/15 的**平台级驱动修复**（显示/触摸/DNS 已打通，见 `docs/90_归档/开发过程复盘与改进清单.md` §22/§23）；
> 演示视频（`提交材料/6360b48….mp4`，27.7s）+ 真机复现证据日志 5 份在 `提交材料/`；
> 逐项结果见 `docs/40_验证与证据/测试体系与方法.md` §七；收尾计划见 `docs/10_需求与方案/项目规划与方案.md` §十。

## 一、主线达成状态（真机闭环验证）

| 里程碑 | 状态 | 证据 |
|---|---|---|
| ESP32-P4X 平台移植 | ✅ | 4 棵文件树 + **329 条 `<copyfile>`** 自动落位（不删除任何上游文件），编译通过 |
| **MCUboot 二级引导 + flash XIP** | ✅ | 引导 24,672 B；text+rodata 迁入 flash XIP，**`sram_seg` 静态占用 120.3 KB / 512 KB（23.5%）**（9/17 干净树重建实测；9/11 首测为 83 KB）；一次 `build.sh` 产出双镜像 |
| 烧录链路 | ✅ | `tools/build_flash/usb_stable.sh` 两镜像：`0x2000` 引导 + `0x20000` 应用，均 hash 校验 |
| console（UART0/CP2102） | ✅ | **`/dev/ttyUSB0`**（须 assert DTR/RTS）；`/dev/ttyACM0` 仅烧录 |
| eth0 RJ45 联网 | ✅ | DHCP `192.168.1.105` RUNNING（主机同网段 192.168.1.119，ICMP 通）|
| PSRAM 32MB heap | ✅ | `free`：Umem total ≈ 33.9 MB |
| ai_agent 完整启动 | ✅ | P0→P6 全 rc=0，36 tools / **12 skills**，WebSocket 板端监听 28789、`All network services started!` |
| **ai_agent 端到端对话** | ✅ | WS 28789 → 消息 → LLM → 回答（真机实测 `你好`；证据 `logs/verify-2026-09-17/agent_llm_ws.txt`）。链路修复见 `docs/90_归档/开发过程复盘与改进清单.md` §27（端点 401 / TCPBACKLOG RST / 计时被 TLS 改时钟）|
| LVGL demo（显示+触摸）| ✅ | 最终固件上运行 `lvgldemo`（GT911 上电 + `/dev/fb0 1024x600 RGB565`），JTAG 帧缓冲导出核实（色数 1265/1266、梯度 <4，与首采一致；存档 `logs/verify-2026-09-17/lvgl_demo_final.png`）|
| **摄像头出帧（赛题③前置）** | ✅ | SC2336 RAW8 1280×720@30（2 lane/336 Mbps）经 V4L2 稳定出帧：**PASS 13/13**、抓帧 15/15、连跑两轮一致（`tools/test/camera_verify.sh`）。根因 = CSI bridge 与 ISP 共享（`SOC_ISP_SHARE_CSI_BRG=1`）而 openvela 未初始化 ISP 输入模块 → 修复 `esp_csi_isp_bypass_init()`（`nuttx/arch/risc-v/src/esp32p4/drivers/esp_csi.c`）；结论三次修正的全过程见 §四.4 |
| **自定义 Skill ×2（赛题②）** | ✅ | `center-assistant` / `quick-note`；`Skills system ready (12 built-in)` |
| **ES8311 音频 + 语音播放** | ✅ **驱动链路已编入固件** | 板级粘合 `esp32p4_audio_es8311.c`（唯一入口 + 功放控制），经 `es8311.c` + `esp_i2s.c`(GDMA) + `pcm_decode` 注册为 `/dev/audio/pcm0`；NSH `nxplayer` 播 WAV。构建 0 error、5 个音频符号 + `nxplayer_main` 落位、调用链反汇编核实。**上板出声待复测**（见 `docs/30_实现/音频_ES8311.md`）|
| **cron 定时主动（赛题③）** | ✅ | `cron_service`/`tool_cron` 真实编入，`[cron] Cron started` |
| AI Coding 日志 | ✅ | **29 会话 / 37,968 事件**（`logs/ez-xu/`，汇总见 `logs/ez-xu/manifest.json`；claude-code 18 + dsh 11，覆盖 8/21–10/23）|

## 二、当前交付

- **通用层零写入（2026-11-02）**：espressif 共享层整层搬进 `nuttx/arch/risc-v/src/esp32p4/espressif/`，`arch/risc-v/src/common/` 只剩 1 个交付文件（`CMakeLists.txt`，riscv_mtimer 条件编译）；早启动 IRQ 守卫下沉到 `esp32p4/espressif/esp_irq.c`；MCUboot 工作树移到 `nuttx/bootloader-esp32p4/`（构建产物）。见 `docs/60_交付与提交/上游改动审计.md`
- **交付形态**：仓根 4 棵文件树（`nuttx/` + `board/esp32p4/` + `app/apps/` + `app/ai_agent/`）+ `contest2026_342_ezdezhandui.xml` **53 条 copyfile + 5 条 linkfile** 自动映射（2026-11-02 起 Espressif 层整层随 `esp32p4` 的 linkfile 交付，通用层只剩 1 个文件）（清单由 `tools/engineering/gen_manifest_copyfiles.py` 与文件树一一对应校验/再生）
- **复现路径**：根 `00_入口/README.md` §四（init → sync → build → **两镜像烧录** → console，**无部署步骤**；copyfile 在 `repo sync` 时自动落位，且移植不删除任何上游文件）
- **闭环工具**：`tools/test/board.py`（串口 harness/断言）、`tools/build_flash/usb_stable.sh`（两镜像烧录）、`tools/test/verify_aiagent.sh`（ai_agent 启动里程碑断言）、`tools/test/camera_verify.sh`（**摄像头出帧验收断言 13/13**）、`tools/test/camera_diag.sh`（摄像头链路一次性判定）、`tools/test/fb2png.py`（JTAG 帧缓冲→PNG 截图）、`tools/test/wedge_diag.sh`（agent 失聪现场 JTAG 取证）
- **板端工具**：`v4l2ctl`（`app/apps/system/v4l2ctl/` → manifest 落位 `apps/system/v4l2ctl/`）—— NuttX 无 `v4l2-ctl`，用它做枚举/开流/抓帧（`v4l2ctl -d /dev/video0 --capture 3 --timeout 20`）
- **真机证据存档**：`docs/40_验证与证据/截图与对照日志/验证截图_LVGL界面_1024x600.png`（LVGL 帧缓冲首采）与 `logs/verify-2026-09-17/`（`lvgl_demo_final.png` 最终固件复采、`camera_final.log` 摄像头实录、`wedge_jtag.txt` 失聪现场、`wedge_fix.txt` 修复前后、`agent_llm_ws.txt` 端到端对话）
- **随提交材料**：`提交材料/` —— 演示视频 `6360b48….mp4`（27.7s）+ 9/18 真机复现证据日志 5 份（`boot_nsh.log` / `aiagent_milestones.log` / `aiagent_live.log` / `lvgldemo.log` / `ws_llm_endtoend.log`）；AI Coding 日志 **29 会话 / 37,968 事件**（`logs/ez-xu/manifest.json`）

## 三、演示脚本

```bash
tools/build_flash/demo_aiagent.sh            # 烧录 + 复位 + 启动 ai_agent + 关键输出
tools/test/verify_aiagent.sh          # P0→P6 里程碑 + 12 skills + eth0 + cron 断言
python3 tools/test/board.py run "free" "ps"    # 任意 NSH 命令并断言
```

## 四、已知限制（诚实记录）

1. ~~**ai_agent 启动后「外部输入全哑」**~~ —— **2026-09-17 已修复**。
   真因（JTAG 埋点一轮定位）：`nuttx/arch/risc-v/src/common/riscv_doirq.c` 的早期引导保护
   `if (g_running_task == NULL) return regs;` 把**所有** trap 都吞掉，而 `up_exit()` 正是
   **故意**先置 `g_running_task = NULL`、再发 `ECALL(SYS_restore_context)` 去切换上下文 →
   该 ECALL 进不到 `riscv_swint()`，被 `.S` 里的 `j` 无限重发（实测 ≈49 万次/秒），
   CPU 100% 卡在 trap 进出路径，节拍中断不再被服务 → 网络/串口一起"哑"。
   **与 agent 无关：子 shell 里敲 `exit` 同样死机**（判别实验，已复现）。
   修复：保护只对硬件中断生效（`irq > RISCV_MAX_EXCEPTION`），异常/系统调用照常派发。
   修复后实测：`ai_agent` 启动完成 → ping 5/6（16.7%，与空闲基线一致；修复前 **0/5 = 100% 丢**）、
   TCP 28789 连接成功；子 shell `exit` 后系统照常运行。详见 `docs/90_归档/开发过程复盘与改进清单.md` §26.7、
   证据 `logs/verify-2026-09-17/wedge_fix.txt`、复测工具 `tools/test/wedge_diag.sh`。
   仍待办（独立问题）：对 28789 的 WS/HTTP 请求会在 ~10ms 内被 RST（TCP 通、端口在听，
   REST 接口未编入），见 `docs/90_归档/开发过程复盘与改进清单.md` §26.7 末。
2. **console 输出突发会被截断** —— `esp_lowputc_send_byte()` 原先未等 TX FIFO 空间即写，
   超长突发丢字节（启动日志常见 ~1.5KB 丢失）。已在 `esp_lowputc.c` 修复（等待 FIFO 空间）
   并将 `CONFIG_UART0_TXBUFSIZE` 提到 2048。
3. ~~**MiMo API key 无效（HTTP 401）**~~ —— **2026-09-17 晚更正**：key 本身有效，401 是**端点错了**（key 属于 Token Plan 网关 `token-plan-cn.xiaomimimo.com`，默认的 `api.xiaomimimo.com` 会拒绝）。改端点后 **LLM 端到端对话已真机实测成功**（WS 28789 → 消息 → 真实回答；证据 `logs/verify-2026-09-17/agent_llm_ws.txt`，链路三处修复见 `docs/90_归档/开发过程复盘与改进清单.md` §27）。
   ⚠️ **但该 key 已随 AI 日志公开泄露过一次（见 §六），必须轮换**；轮换后 key 是编译期注入的，需重新构建烧录。
4. **屏 ✅ / 摄像头 ✅（2026-10-22 修复；改造期文档 docs/08、docs/09 已于 10/23 删除，结论与证据全部收进本节）**：显示（DSI EK79007 + LVGL）、
   触摸（GT911）已真机闭环；**摄像头结论是「驱动链路通、物理链路无数据」**：最终固件
   `camera 3` 实测 —— `esp_csi_data_start_capture: ctlr start rc=0 (DMA+bridge enabled)`、
   `SC2336 chip ID: 0xcb3a`、`CSI: 1920x1080, 2 lane(s) @ 405 Mbps, fb=2592000 bytes`、
   `start_capture -> 0x0100=0x01, read back 0x01 (ret=0)`，三帧全部等不到数据
   （应用阻塞在 DQBUF 不会自己退出：`CONFIG_TTY_SIGINT=n`，要复位才能回 NSH）。
   诊断固件（`ESP_CSI_CAPTURE_DIAG=1`）直读硬件：CSI 桥 `csi_en=1 dtype=0x2f12 flow=0x3c0`、
   DMA `sar` 静止、`buf_depth=0`、D-PHY 仅时钟 lane 活（`rxclkactivehs=1`，数据 lane
   `stopstate=0`）→ 故障收敛到 **MIPI 数据 lane 物理通路**（模组/排线/连接器）。
   一条命令可复测：`tools/test/camera_diag.sh 35`（见 `docs/90_归档/开发过程复盘与改进清单.md` §25.3）。
   - **2026-10-20 复核（重要修正）**：原 `sc2336.c` 的 1080p30 表缺少 `0xffff`
     终止符，`sc2336_write_table()` 会越界读表并把后续 `.rodata` 当寄存器写入传感器；
     该缺陷已修复（改为显式表长），并补上 `ENUM_FRAMESIZES`、互斥、回读校验与
     `v4l2ctl` 目标侧工具。修复后实测：编译 0 告警、枚举全通过、`table verify 8/8`、
     `0x0100` 回读 1，但抓帧仍 0/2；诊断构建下时钟 lane HS 有效、数据 lane 非 stop、
     零 packet 零错误、DMA `dar` 静止 → 结论仍指向 MIPI 数据 lane 物理通路，
     但“硬件损坏”尚缺换模组/示波器证据。
   - **2026-10-22 复核（结论再次修正）**：换用一块**原厂 ESP-IDF 固件下相机正常**的板子，
     烧同一 openvela 镜像后仍 0 帧（枚举/开流/表回读全通过，host 有时钟 lane HS、
     零 packet 零错误、DMA 静止）。因此“摄像头/模组硬件故障”的结论**撤回**：
     硬件可用，故障在 openvela 的 CSI 接收集成。原厂固件平台为
     ESP-IDF v5.5.2 + esp_brookesia_demo 1.2.0 + esp_video 1.4.1（启动日志存档
     `docs/40_验证与证据/ref_stock_boot_esp-idf_v5.5.2.log`）。
   - **2026-10-22 参考固件实测（方案一）**：安装 IDF 5.5.6（release/v5.5）并构建
     esp_video 的 `capture_stream` 例程，在同一块板上 **30 fps 出帧**（SC2336 PID
     0xcb3a，RAW8 1280×720，2 lane/336 Mbps）。由此确认原厂运行时用的是 **RAW8
     720p**（1080p 表只是编译进固件的备选）。openvela 已切到该模式并逐项对齐 CSI
     寄存器（`frame=0x015002d0`、`dtype=0x2f12`、`hctrl=3`），但抓帧仍 0；
     差异已收敛到 D-PHY 模拟/PHY 层（或 lane 映射），需 JTAG/示波器继续。
   - **2026-10-22 16:10 ✅ 修复完成**：根因 = ESP32-P4 的 **CSI bridge 与 ISP 共享**
     （`SOC_ISP_SHARE_CSI_BRG=1`）；参考 esp_video 无条件初始化 ISP 输入模块（RAW8 走 bypass），
     openvela 从未启动 ISP → 共享桥数据通路未建立（表现为 D-PHY 有活动、host 零 packet 零错误、
     桥 FIFO 恒空、DMA 空转，而 CSI host/bridge/GDMA 寄存器与参考逐位一致）。
     修复：`esp_csi.c` 新增 `esp_csi_isp_bypass_init()`（模块时钟/复位/时钟源+分频、`isp_ll_init`、
     时钟使能、输入源=CSI、行/帧尺寸按 bpp、`isp_en=0` bypass、`mipi_csi_brg_claim(SHARE)`，
     整机只初始化一次）；另保留“删除模式表重复软复位”“CSI 控制器延迟到 STREAMON”两处修复。
     验收：`camera_verify.sh` **PASS 13/13**、抓帧 15/15、连跑两轮一致；`--preview` 可见真实画面。
   - **2026-10-22 源码级彻底比对**：运行期抓取参考侧 167 次 SCCB 写，与 openvela
     驱动序列逐条一致（已修掉我们多写的一次软复位）；`esp_cam_ctlr_csi.c` / `mipi_csi_hal.c`
     全文件 diff 仅颜色 API、日志与参考新增的错误 ISR 差异；CSI host 全窗口、bridge 窗口、
     DW-GDMA 通道（`sar/ts/ctl0/chen0`）、PHY 内部 `0x44=0x0a`、LDO 2500 mV、CPU 400 MHz /
     PSRAM 200 MHz 全部逐位一致；控制器创建已延迟到 STREAMON 与参考完全对齐。JTAG 在本 SoC
     无法读 HP 外设区（sysbus/abstract/progbuf 均失败，RAM 正常）。彩条测试图案同样 0 帧。
     下一步：把 IDF `esp_driver_cam`/`esp_video` 的启动路径原样搬进 openvela 验证。
5. ~~**WebSocket 28789 主机侧不通**~~ —— 2026-09-17 更新：改 DHCP 后主机与板子已同网段
   （192.168.1.119 ↔ 192.168.1.105），**agent 未启动时 TCP/ICMP 均通**；agent 一启动即随
   §四.1 的「外部输入全哑」一起失联。故本条不是独立问题，归入 §四.1。
6. ~~**DNS 不可用**~~ —— **2026-09-14 已修复**（真因三项：`CONFIG_NET_UDP` 未开、
   `CONFIG_NETINIT_DHCPC` 未开、netinit 不等链路；详见 §五 与 `docs/90_归档/开发过程复盘与改进清单.md` §23.2），
   现状为 **DHCP 全自动**：`eth0 192.168.1.105`、`nslookup → 202.69.4.22`。
7. **构建健壮性**（2026-09-14 已修）—— 干净树连续构建会因 HAL 兼容补丁重复应用/顺序
   问题 `config fail`；已改为"应用前 `git reset --hard` 钉定版本 + 幂等 apply + 固定顺序
   0002→0001"，并删除补丁中两段过时 hunk（详见 `docs/40_验证与证据/测试体系与方法.md` §八.3）。

8. **本地 manifest 仓副本与官方 gitee 分叉** —— `.repo/manifests` 本地领先 36 / 落后 28（非快进），
   直接 `repo sync` 会停在 `manifests rebase … 冲突`。绕开方式：`repo sync --no-manifest-update`（`--nmu`）。
   已同时修正 `tools/engineering/lock-revision.sh`：它原来只 `sed revision`，会让 `.repo/manifests` 副本在其它条目上
   悄悄漂移（2026-09-18 实测副本仍是 326 条 + 两个废弃条目）。详见 `docs/20_架构与工程/工程框架与日常开发.md` §3.2。
   评审侧不受影响：评审是全新 clone，`.repo/manifests` 直接来自所 init 的分支。

## 五、硬件外设进展（2026-09-16 更新；本节已按 9/14–9/15 的修复结果重写）

> ⚠️ 9/14 那份"显示/触摸失败、疑似模组硬件问题"的判断**已被推翻**：
> 真因是三个**平台级驱动缺陷**（见 `docs/90_归档/开发过程复盘与改进清单.md` §22、§23）。

| 外设 | 状态 | 根因 / 证据 |
|---|---|---|
| MIPI-DSI 7" EK79007 + LVGL | ✅ **已点亮** | 真因：`drivers/video/mipidsi/mipi_dsi_device.c` 8 处 `struct mipi_dsi_msg` **未零初始化**（`rx_len`/`rx_buf` 残留 → 一条 DCS **写**被当成**读**发出去 → 等 BTA 应答 → `-110`）。修复后真机：`/dev/fb0 ready 1024x600 RGB565 @ 0x48000040`、`/dev/fb0 registered (EK79007)` |
| GT911 触摸 | ✅ **已修** | 真因：**产品 ID 校验吃掉 NUL 补齐字节**，控制器一直被误判为"未检测到"（commit `6499860`）。显示修好后需复测触摸是否随之恢复（`docs/90_归档/开发过程复盘与改进清单.md` §23.5(c)） |
| MIPI-CSI SC2336 → `/dev/video0` | ✅ **已出帧（2026-10-22）** | 早期两个坏因已修：`VIDIOC_S_FMT: errno=22` 真因是 `v4l2_cap.c` 在驱动未声明 `frmintervals` 时**回退写死 15 fps**，而 SC2336 只接受 1/30 → 已显式声明 `g_sc2336_frmintervals[]`；随后暴露的 DW_GDMA 中断分配失败也已修（改 `ESP_INTR_FLAG_SHARED` + 补 `up_enable_irq` 的 CLIC IE，commit `3d7b1f25`）。最终出帧根因与修复见 §四.4（CSI bridge 与 ISP 共享 → `esp_csi_isp_bypass_init()`）|
| 网络 / DNS | ✅ **已打通且开机全自动** | 三个叠加原因：① `CONFIG_NET_UDP` 未开（`dns_*` 符号数为 0）② `CONFIG_NETINIT_DHCPC` 未开（注意 `NETUTILS_DHCPC` 只是"编进来"、`NETINIT_DHCPC` 才是"去跑它"）③ netinit 不等链路就发 DHCP。修复后真机：`eth0 192.168.1.105 DRaddr 192.168.1.1`、`nslookup api.xiaomimimo.com → 202.69.4.22`；顺带把 MTU 从 576 对齐到 1500 |
| **ES8311 音频（I2S0 + GDMA）** | ✅ **驱动链路通（10/23）** | 板级只做粘合：`I2C0@0x18` 控制、`I2S0` 数据（MCLK13/BCLK12/WS10/DOUT9/DIN11）、`GPIO53` 使能 NS4150B 功放；`pcm_decode` 注册 `/dev/audio/pcm0`，NSH `nxplayer` 播 16bit 立体声 WAV。⚠️ 原理图两个数据脚按 **codec 视角**命名、与主机方向**相反**（`I2S_DSDIN`=主机出、`I2S_ASDOUT`=主机入），填反则完全无声且无任何报错；MCLK 必须开（codec 按 MCLK÷采样率 查分频表，24kHz 为语音推荐值）。详见 `docs/30_实现/音频_ES8311.md` |
| **WiFi 第 1 层（SDIO host + 枚举）** | ✅ **已实现并编入固件（10/23）** | 板上是 **ESP32-C5-MINI-1**（**非 C6**）；P4 无射频（`SOC_WIFI_SUPPORTED` 不存在），故 WiFi 只能经 SDIO 到协处理器。新增 `esp_sdmmc.c`（~700 行：方法表 + IDMAC + CLIC + 双 slot 路由）与 `esp_hosted_sdio.c`。**开关默认关闭**（`ESPRESSIF_SDMMC` / `ESPRESSIF_HOSTED_SDIO` / `MMCSD_SDIO`）；开启后 0 error、符号落位、调用链核实。⚠️ 真机未跑、第 2/3 层未实现 —— 打开也上不了网，eth0 仍是联网路径。详见 `docs/30_实现/连接_WiFi_SDIO.md` |
| ✅ ~~ai_agent 启动后整机冻结~~ **已修复（9/17 晚）** | 真因：`riscv_doirq()` 的早期引导保护吞掉了 `up_exit()` 的 `SYS_restore_context` ECALL —— **任何任务退出都会死机**（子 shell `exit` 可复现）。修复：保护只对硬件中断生效；实测 agent 启动完成后 ping/TCP 28789 均正常。证据 `logs/verify-2026-09-17/wedge_fix.txt`，详见 §四.1 与 `docs/90_归档/开发过程复盘与改进清单.md` §26.7 |
| 运维通道 | ⚠️ 注意 | CP2102（`/dev/ttyUSB0`）不在时可用 USJ 口：`python3 tools/test/board.py run "…" --port /dev/ttyACM0`；但 USJ 的 **DTR/RTS 直接控制复位/下载模式**，被串口软件占用会造成"板子假死"的假象（`docs/90_归档/开发过程复盘与改进清单.md` §23.6） |

## 六、安全提醒

- ⚠️ **MiMo API key 两次误入公开仓历史 —— 必须轮换、且只当它已作废**：
  1) 早先一次：commit `4d7f318`；
  2) **2026-09-18 发现**：导出的 AI Coding 日志把会话里贴过的 key 原样带出，
     `logs/ez-xu/2026-09-11/dsh__96802734-….jsonl` 里有 4 处完整 key（该文件由 commit `85c17a6` 引入）。
     已于 commit `5663907` 全部脱敏为 `tp-***REDACTED…***`（JSONL 结构校验通过：6948 行可解析）。
  **仅从仓库改写/删除不能使其失效** —— 它已经在公开仓出现并被 fork/缓存，唯一处置是**在平台侧吊销/轮换**。
  根因是导出链路对流式会话的凭据掩码不完整（同一条日志里有的位置被掩码、有的没有）→
  **提交前必须对 `logs/` 做一次凭据扫描**（示例：`grep -rEo 'tp-[A-Za-z0-9]{40,}|sk-[A-Za-z0-9]{20,}' logs/`）。
  新 key 一律经本地 `ai_agent/include/agent_secrets.h`（.gitignore 忽略）注入，不入仓。
- ⚠️ 本地环境坑：`openocd` 运行期间占用 USB，用完务必 `pkill -x openocd`；
  `pkill -f openocd` 会连自己的 shell 一起杀掉（模式自匹配）。

---

## 2026-10-28 · 第 85 轮复核（在 WiFi 调试大量改动之后）

WiFi 排查期间对 `esp_sdmmc.c` / `esp_hosted_sdio.c` 动了十几处，
本轮重新确认**音频与语音播放这两条已交付链路没有受影响**：

```
$ audiotest -t 1
audiotest: queued 96044 bytes
audiotest: done

$ audiotest -w /tmp/t.wav      ->  -rwxrwxrwx  192044 /tmp/t.wav
$ nxplayer                     ->  device /dev/audio/pcm0 -> play /tmp/t.wav -> q
                                 干净返回 nsh>（此前那个无参命令 NULL 解引用会崩）
```

| 复核项 | 结果 |
|---|---|
| ES8311 音频（I2S0 + GDMA）| ✅ `queued 96044` / `done` |
| WAV 生成 | ✅ 192,044 B |
| 语音播放（`nxplayer`）| ✅ device→play→q 全程无崩溃 |
| 帧层往返测试 | ✅ all checks passed |
| 交付清单 | ✅ 354 条一致 |

**结论：WiFi 侧的改动没有影响音频/语音这两条路径。**

---

## WiFi（C6 / SDIO + ESP-Hosted）—— **传输层已打通** · 2026-10-28

**主机侧完成并逐条仪器验证；从机侧零响应，已实验排除固件、只剩物理层。**

### 做到了什么

| 项 | 证据 |
|---|---|
| 时钟 | 逻辑分析仪 **25 MS/s 实测 403 kHz**（SD 规范 100–400 kHz）|
| 时钟连续性 | 关低功耗后连续运行，单次采集 714,543 次跳变 |
| CIU 执行命令 | `CTRL.command.start_command` 自清 |
| 初始化时钟 | 线上实测 **81 个**（规范 ≥74）|
| `CMD0` / `CMD5` | **均 CRC7 校验通过**（自写 48 位解码器，所有对齐都试过）|
| 从机复位 | GPIO54 脉冲实测有效（C6 console 每次复位新增完整启动日志）|
| 引脚/slot | 官方原理图确认：C6 在 `SD2` = GPIO18/19/14-17 = **slot 1** |

### 两个已修复的真实根因

1. **SDIO PLL 从未被 PMU 上电**（`sdmmc_ll_enable_sdio_pll()` 从未调用）
   → CIU 一条命令都不执行，表现为"寄存器全对却什么都不动"；
2. **`waitresponse()` 忽略命令字里的响应标志** → 无响应的 CMD0 也去等响应、
   超时退出，**CMD5 从来没被发出去过**。此前几十轮的 `-110` 都不是"等不到回应"。

### 已实验排除

时钟频率/连续性/初始化时钟数 · 引脚/slot/矩阵路由/信号索引 · 两种时钟源 ·
低速与高速路径 · `reg_sdio_ls_clk_div_num` · 等待时间(1000/2500 ms) ·
**时钟边沿四种组合(drv/sam 的 0/1 全试)** · C6 是否被复位 ·
**C6 固件内容**（用 esp-hosted-mcu v3.0.10 从源码重建、hash 校验通过、
配置逐项一致，烧入后行为与原先完全相同）

### 唯一剩下的环节

**物理层**。C6 收到合法命令却零响应，且其日志显示从未收到主机的任何数据。
需要：
1. 万用表量 P4 ↔ C6 六根线通断
   （GPIO18/19/14/15/16/17 ↔ IO19/18/21/22/23/20）；
2. 或示波器探到 C6 一侧，确认其引脚上是否真有信号。

详见 `docs/60_交付与提交/WiFi交付说明.md` 与 `docs/30_实现/连接_WiFi_SDIO.md` 附录 T。

---

### 🎉 2026-10-28 深夜更新：**SDIO 传输层打通**

长期卡住的 WiFi 链路本轮通了。真机日志：

```
esp_hosted_sdio: companion enumerated (sdio_probe returned 0)
esp_hosted_sdio: card function ready (FN1, 512-byte blocks)
esp_hosted_sdio: frame layer up (hdr v1, 12-byte headers, checksum on)
```

**连续 3 次复位，3 次全部成功** —— 稳定可复现。

每条命令都收到真实响应：

```
CMD03 resp -> 00010000 CHANGED (RCA = 0x0001)
CMD07 resp -> 00001e00 CHANGED
CMD52 resp -> 00001040 CHANGED
```

**四个互相掩盖的 bug（缺一不通）：**

1. `waitresponse()` 末尾缺 `return OK`（整块替换时被删）→ 返回不确定值 53；
2. 上层成功判据写成 `ret < 0` → 把 53 当成功，"枚举成功"假象掩盖了 #1；
3. 完成位没在发命令前清掉 → 轮询看到残留位，响应"迟到"、读到旧 `RESP0`；
4. 长期依赖中断事件 → 该中断从未送达，改轮询后才开始工作。

**结论：C6 从机自始至终完全正常。** 问题全部在主机侧驱动。
详见 `docs/30_实现/连接_WiFi_SDIO.md` 附录 U/V/W/X。

**尚未实现**：ESP-Hosted 的 SDIO 数据通路（CMD53 包长/token 协议）与协议层
（RPC → 远端 `wlan0`）。`ifconfig` 目前只有 `eth0`。

---

### WiFi 最终状态（2026-10-28 收尾）

**交付核实：构建 0 error、工作树干净、清单 355 条一致、文档 20 篇、
音频 `queued 96044 bytes` 正常、WiFi 传输层多次复位稳定。**

#### 已实现并逐项验证（主机侧驱动）

| 组件 | 验证方式 |
|---|---|
| SDIO 枚举（`CMD0/5/3/7`）| 多次复位稳定；波形解码 `cmd0/cmd5×4/cmd3/cmd7` 清晰可辨 |
| 卡功能（FN1、512 字节块）| `card function ready` |
| ESP-Hosted 帧层 | `frame layer up (hdr v1, 12-byte headers, checksum on)` |
| CMD52 双向寄存器 | 写 `0x5A` 读回 `0x5A` |
| CMD53 数据搬运（DMA + cache）| 预填 `0xAA` 被 DMA 覆盖 |
| 命令帧正确性 | 波形逐位解码，`arg=0x1fef8801` 与参考实现一致 |
| `OPEN_DATA_PATH` 握手 | CP 内部计数 20 → 21 |
| 从机开放 TX 缓冲区 | `TOKEN` 0 → 2 |
| 从机登记主机的 CMD53 | `TOKEN` 在读取前后变化 |

#### 本次定位到的 6 个真实缺陷

1. `waitresponse()` 末尾缺 `return OK`（整块替换时被删，返回不确定值）
2. 上层成功判据写成 `ret < 0`（把 53 当成功，**掩盖了 #1**）
3. 完成位没在发命令前清掉（响应"迟到"，读到旧 `RESP0`）
4. `eventwait()` 依赖中断事件（该中断从未送达，改轮询后才工作）
5. DMA 没有 cache 维护（接收不作废、发送不回写）
6. **`widebus()` 把 D1/D2/D3 配成普通输出且未设值 → 驱动为低**
   —— **直接解释了"命令通、数据不通"**

#### 仍未打通

| 现象 | 状态 |
|---|---|
| 从机排队的 60 字节负载 | 🔴 读出来为零（试过 5 种地址/长度 × 2 种模式 + 相位试探）|
| 数据阶段的 D0 | ⚠️ 修复 #6 后有活动（519 个 0 / 9999 位），但解出的不是帧 |
| D1 空闲电平 | ⚠️ 为低（可能是从机当中断线驱动，属正常）|
| 协议层（RPC → `wlan0`）| ⏸ 未实现 |

#### 两侧链条都完整，缺口在中间

**从机侧**：收到 `OPEN_DATA_PATH`（计数 +1）→ `datapath = 1`（日志时序确认，
中间无 `Close`）→ 调用 `sdio_slave_transmit()` 把帧交给 SDIO 从机外设 →
宣告 `transfer size 1536`。

**主机侧**：命令帧与参考实现逐位一致 → 时钟连续（7999 个沿）→ 数据线空闲电平正确 →
DMA 与 cache 正确 → 从机登记了 CMD53。

**两边各自都走到了"应该能传数据"的状态，中间却没有数据。**

#### 结论与后续

**主机侧驱动已经完整实现并逐项验证，每一条都有波形或日志证据。**

剩下的问题需要**主机侧之外的观测手段**才能区分：

1. 用示波器探到 C6 一侧，看它的数据线在数据阶段是否真的被驱动；
2. 或者用一个已知良好的 SDIO 从机做对照。

**在此之前，继续在主机侧改代码只能是猜测。**

详见 `docs/30_实现/连接_WiFi_SDIO.md` 附录 AA.62–AA.78。

---

### WiFi 收尾（2026-10-29）

**交付核实：构建 0 error、工作树干净、清单 355 条一致、文档 20 篇、
WiFi 传输层多次复位稳定。**

#### 已实现并逐项验证（主机侧驱动）

| 组件 | 验证方式 |
|---|---|
| SDIO 枚举（`CMD0/5/3/7`）| 多次复位稳定；波形解码 `cmd0/cmd5×4/cmd3/cmd7` 清晰可辨 |
| 卡功能（FN1、512 字节块）| `card function ready` |
| ESP-Hosted 帧层 | `frame layer up (hdr v1, 12-byte headers, checksum on)` |
| CMD52 双向寄存器 | 写 `0x5A` 读回 `0x5A` |
| CMD53 数据搬运（DMA + cache）| 预填 `0xAA` 被 DMA 覆盖 |
| 命令帧正确性 | 波形逐位解码，`arg=0x1fef8801` 与参考实现一致 |
| `OPEN_DATA_PATH` 握手 | CP 内部计数 20 → 21 |
| 从机开放 TX 缓冲区 | `TOKEN` 0 → 2 |
| 从机登记主机的 CMD53 | `TOKEN` 在读取前后变化 |

#### 本次定位并修复的 7 个真实缺陷

1. `waitresponse()` 末尾缺 `return OK`（整块替换时被删，返回不确定值）
2. 上层成功判据写成 `ret < 0`（把 `53` 当成功，**掩盖了 #1**）
3. 完成位没在发命令前清掉（响应"迟到"，读到旧 `RESP0`）
4. `eventwait()` 依赖中断事件（该中断从未送达，改轮询后才工作）
5. DMA 没有 cache 维护（接收不作废、发送不回写）
6. `widebus()` 把 D1/D2/D3 配成普通输出且未设值 → **驱动为低**
7. `configpins()` 从未把 `DAT1..3` 接回控制器 → **4-bit 在结构上不可能**

#### 主机侧已穷尽的变量

时钟频率（100/400/403 kHz）· 时钟边沿（`drv`/`sam` 四种 + 数据阶段单独翻转）·
引脚映射（四种信息核对 + 三组实机实验 + 脉冲计数核对）· 总线宽度（1-bit / 真正接通的
4-bit）· 命令地址（`0x00000/0x1F200/0x1F600/0x1F7C4`）· 传输长度（60/512/1536）·
读取时机（立即/100 ms/300 ms/6 s）· 中断与寄存器握手 · DMA 与 cache · 数据线驱动电平

**每一项都有实验记录，每一项都有明确的观测结果。**

#### 仍未打通

从机排队的 60 字节负载读出来**全零**；波形显示数据阶段四条数据线**没有可解析的内容**。

从机侧已确认：收到 `OPEN_DATA_PATH`、`datapath = 1`（日志时序）、
调用 `sdio_slave_transmit()` 把帧交给 SDIO 从机外设、持续用 `DAT1` 通报"有新包"。

**两侧各自都走到了"应该能传数据"的状态，中间没有数据。**

#### 后续需要的条件

1. 用示波器探到 C6 一侧，看它的数据线在数据阶段是否真的被驱动；
2. 或用一个已知良好的 SDIO 从机做对照。

**在此之前继续在主机侧改动只能是猜测。**
