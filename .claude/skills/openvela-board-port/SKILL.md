---
name: openvela-board-port
description: openvela vendor 板级 BSP 移植工作流——新增 boards/<soc>/<board> 板级层 + chips/<soc> 芯片层（custom board/custom chip 机制），构建验证与推送。Trigger: 新增开发板支持、板级适配、BSP 移植、chip 移植、defconfig 编写。
---

# openvela vendor 板级 BSP 移植

在 vendor 仓（如 vendor_rockchip）内完成板级+芯片级移植，**不改 openvela nuttx 内核树**。
机制：`CONFIG_ARCH_BOARD_CUSTOM` + `CONFIG_ARCH_CHIP_CUSTOM`（参照 `vendor/bes` 先例）。

## 一、前置侦察（30 分钟内完成）

1. 抓 SoC dtsi（硬件权威数据）：
   ```bash
   curl -sL "https://ghfast.top/https://raw.githubusercontent.com/torvalds/linux/master/arch/arm64/boot/dts/rockchip/rk3576.dtsi" -o /tmp/soc.dtsi
   python3 - <<'EOF'
   import re
   t = open('/tmp/soc.dtsi').read()
   for m in re.finditer(r'(uart\d+): serial@([0-9a-f]+) \{.*?interrupts = <GIC_SPI (\d+)', t, re.S):
       n, a, i = m.groups(); print(f"{n}: 0x{a}  GIC_SPI {i} -> NuttX IRQ {int(i)+32}")
   EOF
   grep -B2 -A8 'interrupt-controller@' /tmp/soc.dtsi | head -12   # GIC 类型/基址
   ```
2. 板级 dts（厂商树）：k7 类板子在 Armbian 包 `patch/kernel/*/dt/` 里（见 `docs/KICKPI-K7速查.md`）
3. 找 openvela 内同族参考：`nuttx/arch/arm64/src/<同族chip>/`（注意 A64 残留）、`vendor/openvela/boards/vela/scripts/dramboot_armv8a.ld`

## 二、目录结构（生成到 vendor 仓）

```
<vendor>/
├── boards/<soc>/<board>/
│   ├── Kconfig                # ARCH_BOARD_<BOARD> + selects
│   ├── CMakeLists.txt         # add_subdirectory(src) + LD_SCRIPT 属性（必须！）
│   ├── include/board.h        # UART 基址/IRQ/时钟（会被链接为 arch/board）
│   ├── src/{CMakeLists.txt, <board>_boardinit.c, <board>.h}
│   ├── scripts/dramboot_armv8a.ld   # 直接复用 vela 的（加载地址标 TODO）
│   └── configs/<name>/defconfig     # 见第四节
└── chips/<soc>/
    ├── CMakeLists.txt         # target_sources(arch PRIVATE ...)
    ├── Kconfig                # ARCH_CHIP_<SOC> + 外设 menu + GIC 版本
    ├── Make.defs
    ├── chip.h                 # #include <arch/chip/chip.h>
    ├── <soc>_boot.c/h
    ├── <soc>_lowputc.S        # UART0 基址
    ├── <soc>_serial.c/h       # 16550 寄存器集驱动
    ├── include/chip.h         # ⚠️ 必须有 include/ 子目录！
    ├── include/irq.h          #   （构建链接为 include/arch/chip）
    └── hardware/<soc>_memorymap.h
```

## 三、芯片层移植清单（copy-adapt）

1. `cp -r` 同族芯片文件改名（`sed 's/<old>/<new>/g'`）
2. **串口驱动**：基址/IRQ 换新值（dtsi 数据）；**删 CCU 门控段**（u-boot 已开控制台时钟）；**删 PIO 引脚段**（A64 专属）；无定义函数的调用块删除换 TODO 注释；控制台改 UART0
3. **include/chip.h**：GICD/GICC（v2）或 GICD/GICR（v3）基址、RAM 布局、LOAD_BASE（标 TODO(hardware)）
4. **Kconfig**：`select ARMV8A_HAVE_GICv2`（v2 芯片）+ 外设 menu（UART0 default y 控制台）+ `config ARM64_GIC_VERSION default 2 if ARCH_CHIP_<SOC>`（v2 芯片）
5. lowputc.S 基址、Make.defs、CMakeLists 同步

## 四、defconfig 必填块（参照 vendor/bes 先例）

```ini
CONFIG_ARCH_BOARD_CUSTOM=y
CONFIG_ARCH_BOARD_CUSTOM_DIR="../vendor/<vendor>/boards/<soc>/<board>/"
CONFIG_ARCH_BOARD_CUSTOM_DIR_RELPATH=y
CONFIG_ARCH_BOARD_CUSTOM_NAME="<board>"
CONFIG_ARCH_CHIP_<ARCH>_CUSTOM=y            # 如 ARCH_CHIP_ARM64_CUSTOM
CONFIG_ARCH_CHIP_CUSTOM=y
CONFIG_ARCH_CHIP_CUSTOM_DIR="../vendor/<vendor>/chips/<soc>/"
CONFIG_ARCH_CHIP_CUSTOM_DIR_RELPATH=y
CONFIG_ARCH_CHIP_CUSTOM_NAME="<soc>"
CONFIG_ARCH="arm64" / CONFIG_ARCH_ARM64=y
CONFIG_ARCH_BOARD="<board>" / CONFIG_ARCH_BOARD_<BOARD>=y
CONFIG_ARCH_CHIP="<soc>"  / CONFIG_ARCH_CHIP_<SOC>=y
CONFIG_UART0_BAUD=<rockchip调试波特率，一般1500000>
```
其余从同族 defconfig 复制（RAM 布局值标 TODO(hardware)）。

## 五、构建验证循环

```bash
rsync -a <vendor>/boards/ <vendor>/chips/ 构建树/vendor/rockchip/   # 或对应 vendor 路径
cd openvela && rm -rf cmake_out && PATH=/usr/bin:$PATH ./build.sh vendor/<vendor>/boards/<soc>/<board>/configs/<name>/ --cmake -j4
```
（`PATH=/usr/bin:$PATH` 是绕过本机 /usr/local/bin/python=python2 的必需姿势）
常见错误速查：
- `arch/chip/irq.h No such file` → chips 缺 include/ 子目录
- `get_filename_component ... incorrect number of arguments` → LD_SCRIPT 未设置
- `defined but not used [-Werror]` → 控制台指向了未启用的 UART（CONSOLE_DEV 宏）
- `unknown mnemonic get_cpu_id` → include/chip.h 缺 `__ASSEMBLY__` 段的 get_cpu_id 宏（照 goldfish 抄）
- `'OK' undeclared` → **openvela 内核无 OK/ERROR 宏**，用 `return 0`
- `undefined reference to board_app_initialize / board_reset` → 板级补两个回调（board_reset 是 **int** 签名，见 nuttx/include/nuttx/board.h）
- `'RK3576_UART2_ADDR' undeclared` → rk3399 原版驱动 UART0 块复制粘贴的是 UART2 地址（照抄前先核对每个端口的 addr/irq）
- `LED_PANIC undeclared` → 没实现 LED 前别 `select ARCH_HAVE_LEDS`
- 404/`could not read Username` 推不动 → 见下节

## 六、推送（GitHub 直连慢，带凭据）

```bash
export GH_TOKEN=$(gh auth token)   # ⚠️ 别存临时文件
git -c http.extraheader="AUTHORIZATION: basic $(printf 'x-access-token:%s' "$GH_TOKEN" | base64 -w0)" \
    push https://github.com/<user>/<repo>.git <branch>
```
- 新 commit 必须走 git push（对象不在远端时 API 建 ref 会 422）
- 分支名不要用 `dev/xxx`（与已存在 `dev` 分支冲突，API 报 "Reference update failed"）
- 推不动就重试循环（网络间歇问题，间隔 20-30s）

## 七、坑文档

每个移植遇到的新坑追加到 `docs/踩坑笔记_0X_*.md`（M3 要求 ≥15 篇）。已积累：
- #01 openvela quickstart（repo 版本/gerrit 自举/python2 干扰/镜像软链）
- #02 vendor BSP 移植（custom board/chip 机制/A64 残留/LD_SCRIPT/include 目录）
- #03 原理图提取/语音模型/工程纪律（pdftotext 文字层、pkill 括号铁律、构建中断产物不可信、git 未推送直接重初始化）

通用铁律（本会话犯过多次的）：
- **`pkill -f` 会匹配自身命令行 → 用 `pgrep -f '[x]xx'` 或按 PID kill**
- 构建被杀后产物目录大文件不可信（objcopy 报 format not recognized → 删 cmake_out 重建）
- 模型/大文件提前进 .gitignore；未推送的仓库误入库直接 `rm -rf .git` 重来
