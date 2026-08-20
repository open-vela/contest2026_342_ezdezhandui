---
name: codesize
description: Analyzes embedded system binary size using bin/elf/map files. Supports multi-core/multi-architecture (ARM/Xtensa/RISC-V). Use when optimizing firmware size, comparing versions, or identifying large code modules in Vela/NuttX projects.
---

# Codesize 分析 Skill

## ⚡ TL;DR 快速入门

```bash
# 方案1: 快速分析（推荐日常使用）
size -t path/to/*.a > /tmp/size.txt
python3 scripts/analyze_size_output.py /tmp/size.txt

# 方案2: 深度分析（符号级）
python3 scripts/analyze_map_file.py project.map --show-symbols

# 方案3: 版本对比
python3 scripts/compare_codesize.py before.txt after.txt
```

---

## 📋 目录

- [目标与适用场景](#目标与适用场景)
- [工具一览](#工具一览)
- [快速选择指南](#快速选择指南)
- [详细使用说明](#详细使用说明)
- [输出格式与CI/CD集成](#输出格式与cicd集成)
- [优化策略](#优化策略)
- [常见问题](#常见问题)

---

## 目标与适用场景

### 目标
分析嵌入式系统编译产物的代码大小，识别占用空间最大的模块，并提供优化建议。

### 核心能力
| 能力 | 说明 |
|------|------|
| ✅ 多架构支持 | ARM Cortex-M、Xtensa (Audio/DSP)、RISC-V |
| ✅ 自动过滤 | 排除 Discarded sections，只统计真正链接的代码 |
| ✅ 符号级分析 | 提取函数名/变量名，精确定位 |
| ✅ 版本对比 | 对比不同版本，高亮显著变化 |
| ✅ 多格式输出 | Markdown/JSON/CSV，便于 CI/CD 集成 |

### 适用场景
- 固件大小超标需要优化
- 分析不同模块的代码占用情况
- 对比不同版本的大小变化
- 识别意外链接的代码和数据
- 多核心系统分析 (AP + Audio 核心)
- CI/CD 自动化检查

### 前置条件

**必备文件**：
- `*.map` - 链接器生成的 map 文件（最关键）
- `*.elf` - 可执行文件
- `*.a` - 静态库文件（用于 size 分析）

**架构支持**：
- **ARM Cortex-M**：识别 `.ARM.*` sections
- **Xtensa**：识别 `.xt.*` sections  
- **RISC-V**：识别 `.riscv.*` sections

### Vela/NuttX 项目路径配置

#### 典型目录结构

Vela/NuttX 项目通常采用以下目录结构：

```
<project_root>/              # 项目根目录 (如 ~/ssd/vela_xxx/)
├── out/                     # 编译输出目录
│   ├── <platform>/          # 平台构建目录 (单核或主核心)
│   ├── <platform>_audio/    # Audio 核心 (多核系统)
│   ├── <platform>_cp/       # CP 核心 (多核系统)
│   └── <platform>_sensor/   # Sensor 核心 (多核系统)
└── prebuilts/               # 预编译工具链
    ├── clang-arm/           # LLVM ARM 工具链
    ├── clang-xtensa/        # LLVM Xtensa 工具链
    └── gcc/                 # GCC 工具链 (ARM/RISC-V)
```

#### 编译产物位置 (out/)

每个平台/核心的输出目录包含以下文件：

| 文件类型 | 文件名模式 | 用途 | 典型大小 |
|---------|-----------|------|---------|
| **ELF 可执行文件** | `vela_*.elf`<br>`nuttx` | 符号级调试、size 分析 | 10-50M |
| **链接器 Map 文件** | `vela_*.map`<br>`nuttx.map`<br>`System.map` | **深度分析（推荐）** | 10-50M |
| **二进制镜像** | `vela_*.bin`<br>`nuttx.bin` | 烧录镜像 | 2-10M |
| **静态库** | `apps/**/*.a`<br>`libs/**/*.a` | size 快速分析 | 各异 |

**静态库位置**：`out/<platform>/apps/**/*.a` 或 `out/<platform>/libs/**/*.a`

**示例路径**：
```bash
# 示例 1: 单核系统
out/qemu_platform/
├── nuttx.map           # 主 map 文件
├── nuttx.bin           # 二进制镜像
├── System.map          # 内核符号表
└── apps/               # 应用静态库
    └── **/*.a

# 示例 2: 多核系统
out/platform_ap/        # AP 核心 (ARM)
├── vela_ap.elf
├── vela_ap.map         # AP 核心 map 文件
├── nuttx.map
└── apps/**/*.a

out/platform_audio/     # Audio 核心 (Xtensa)
├── vela_audio.elf
├── vela_audio.map      # Audio 核心 map 文件
├── nuttx.map
└── apps/**/*.a
```

#### 工具链位置 (prebuilts/)

**通用路径模式**：`<project_root>/prebuilts/<toolchain>/<host>/bin/`

##### LLVM 工具链 (用于 ARM/通用)
```bash
# 设置环境变量（替换为实际项目路径）
export PROJECT_ROOT=~/ssd/vela_xxx
export TOOLCHAIN_LLVM="$PROJECT_ROOT/prebuilts/clang-arm/linux-x86_64/bin"

# 设置别名方便使用
alias llvm-size="$TOOLCHAIN_LLVM/llvm-size"
alias llvm-nm="$TOOLCHAIN_LLVM/llvm-nm"
alias llvm-readelf="$TOOLCHAIN_LLVM/llvm-readelf"
alias llvm-objdump="$TOOLCHAIN_LLVM/llvm-objdump"
```

**可用工具**：llvm-size, llvm-nm, llvm-readelf, llvm-objdump, llvm-objcopy

##### GCC ARM 工具链
```bash
export TOOLCHAIN_ARM="$PROJECT_ROOT/prebuilts/gcc/linux-x86_64/arm-none-eabi/bin"
alias arm-size="$TOOLCHAIN_ARM/arm-none-eabi-size"
alias arm-nm="$TOOLCHAIN_ARM/arm-none-eabi-nm"
```

**可用工具**：arm-none-eabi-size, arm-none-eabi-nm, arm-none-eabi-readelf

##### Xtensa 工具链 (用于 Audio/DSP 核心)
```bash
export TOOLCHAIN_XTENSA="$PROJECT_ROOT/prebuilts/clang-xtensa/linux-x86_64/bin"
alias xt-size="$TOOLCHAIN_XTENSA/xt-size"
alias xt-nm="$TOOLCHAIN_XTENSA/xt-nm"
alias xt-readelf="$TOOLCHAIN_XTENSA/xt-readelf"
alias xt-objdump="$TOOLCHAIN_XTENSA/xt-objdump"
```

**可用工具**：xt-size, xt-nm, xt-objdump, xt-readelf (及 xtensa-elf-* 前缀版本)

##### RISC-V 工具链
```bash
export TOOLCHAIN_RISCV="$PROJECT_ROOT/prebuilts/gcc/linux-x86_64/riscv-none-elf/bin"
alias riscv-size="$TOOLCHAIN_RISCV/riscv-none-elf-size"
alias riscv-nm="$TOOLCHAIN_RISCV/riscv-none-elf-nm"
```

**可用工具**：riscv-none-elf-size, riscv-none-elf-nm, riscv-none-elf-readelf

#### 快速分析命令模板

```bash
# 环境设置（根据实际项目调整）
export PROJECT_ROOT=~/ssd/vela_xxx              # 项目根目录
export PLATFORM=<platform_name>                 # 平台名称，如：qemu_platform, xxx_ap
export SKILL_PATH=~/.copilot/skills/codesize   # Skill 脚本路径

# 工具链
export LLVM_BIN="$PROJECT_ROOT/prebuilts/clang-arm/linux-x86_64/bin"
export XTENSA_BIN="$PROJECT_ROOT/prebuilts/clang-xtensa/linux-x86_64/bin"

# ===== 方案 1: 快速分析（使用 size 命令）=====
cd $PROJECT_ROOT/out/$PLATFORM
$LLVM_BIN/llvm-size -t apps/**/*.a > /tmp/${PLATFORM}_size.txt
python3 $SKILL_PATH/scripts/analyze_size_output.py /tmp/${PLATFORM}_size.txt --top 30

# ===== 方案 2: 深度分析（使用 map 文件）=====
# 分析主 map 文件（推荐）
python3 $SKILL_PATH/scripts/analyze_map_file.py \
    $PROJECT_ROOT/out/$PLATFORM/nuttx.map \
    --show-symbols --top 50

# 或分析核心专用 map 文件
python3 $SKILL_PATH/scripts/analyze_map_file.py \
    $PROJECT_ROOT/out/$PLATFORM/vela_*.map \
    --show-symbols --top 50

# ===== Xtensa 核心分析示例 =====
export PLATFORM_AUDIO=<platform>_audio
cd $PROJECT_ROOT/out/$PLATFORM_AUDIO
$XTENSA_BIN/xt-size -t apps/**/*.a > /tmp/audio_size.txt
python3 $SKILL_PATH/scripts/analyze_size_output.py /tmp/audio_size.txt --top 20
```

---

## 工具一览

本 skill 提供以下分析工具：

```
skills/codesize/
├── SKILL.md                    # Skill 主文档
└── scripts/                    # 可执行脚本
    ├── analyze_size_output.py  # size 命令输出分析 (快速)
    ├── analyze_map_file.py     # map 文件深度分析 (符号级)
    ├── compare_codesize.py     # 版本对比工具
    ├── codesize_utils.py       # 公共模块
    └── test_tool.sh            # 测试脚本
```

### 1️⃣ analyze_size_output.py - 快速分析 ⭐

基于 `size` 命令输出，快速统计模块大小。

```bash
# 采集数据
size -t path/to/*.a > /tmp/size.txt

# 分析
python3 scripts/analyze_size_output.py /tmp/size.txt

# 高级选项
python3 scripts/analyze_size_output.py /tmp/size.txt \
    --top 20 \
    --format json \
    --output report.json
```

**输出内容**：
- 总体 Flash/RAM 占用
- 按库文件分组统计
- Top N 最大对象文件
- 可视化占比图表
- 智能优化建议

### 2️⃣ analyze_map_file.py - 深度分析 ⭐⭐

解析 map 文件，提供符号级精确分析。

```bash
# 基本分析
python3 scripts/analyze_map_file.py project.map

# 显示符号名（函数名/变量名）
python3 scripts/analyze_map_file.py project.map --show-symbols

# 输出 JSON
python3 scripts/analyze_map_file.py project.map --format json --top 50
```

**输出内容**：
- 架构自动识别
- 各段详细统计 (.text, .data, .bss, .rodata, .dtcm.bss)
- **符号名解析**：显示函数名/变量名
- C++ 符号标记
- 按库/对象文件统计
- 大对象分析和优化建议

### 3️⃣ compare_codesize.py - 版本对比 ⭐⭐

对比两个版本的代码大小，高亮显著变化。

```bash
# 对比两个 size 输出
python3 scripts/compare_codesize.py before.txt after.txt

# 对比两个 map 文件
python3 scripts/compare_codesize.py before.map after.map --type map

# 输出 JSON（便于 CI/CD）
python3 scripts/compare_codesize.py before.txt after.txt --format json

# 设置显著变化阈值
python3 scripts/compare_codesize.py before.txt after.txt --threshold 10
```

**输出内容**：
- 总体变化摘要
- 显著变化的模块（高亮）
- 新增/移除的模块
- Top N 变化模块详情
- 增长/减少原因分析

---

## 快速选择指南

```
┌─────────────────────────────────────────────────────────────────┐
│                     你需要做什么?                                 │
└─────────────────────────────────────────────────────────────────┘
                              │
          ┌───────────────────┼───────────────────┐
          ▼                   ▼                   ▼
   ┌─────────────┐    ┌─────────────┐    ┌─────────────┐
   │ 日常检查    │    │ 定位大函数  │    │ 版本对比    │
   │ 快速概览    │    │ 深度优化    │    │ CI/CD检查   │
   └─────────────┘    └─────────────┘    └─────────────┘
          │                   │                   │
          ▼                   ▼                   ▼
   analyze_size_      analyze_map_        compare_
   output.py          file.py             codesize.py
          │                   │                   │
          ▼                   ▼                   ▼
   size -t *.a        project.map         两个版本
   快速、简单         符号级精度          差异分析
```

### 场景对照表

| 场景 | 推荐工具 | 命令示例 |
|------|----------|----------|
| 日常检查 | analyze_size_output.py | `size -t *.a > s.txt && python3 scripts/analyze_size_output.py s.txt` |
| 深度优化 | analyze_map_file.py | `python3 scripts/analyze_map_file.py proj.map --show-symbols` |
| 版本对比 | compare_codesize.py | `python3 scripts/compare_codesize.py v1.txt v2.txt` |
| CI/CD 集成 | 任意 + JSON | `python3 scripts/analyze_size_output.py s.txt --format json` |
| 阈值检查 | 任意 + --fail-on-threshold | `python3 scripts/analyze_map_file.py p.map --fail-on-threshold 512` |
| 定位大函数 | analyze_map_file.py | `python3 scripts/analyze_map_file.py p.map --show-symbols --top 50` |
| 多核心分析 | analyze_map_file.py | 分别分析 `ap.map` 和 `audio.map` |

---

## 详细使用说明

### 方案1: size 命令快速分析

适合日常检查、版本对比、CI/CD 集成：

```bash
# 1. 采集数据
cd out/your_platform/
size -t apps/path/to/*.a > /tmp/size_output.txt

# 2. 分析生成报告
python3 ../../skills/codesize/scripts/analyze_size_output.py /tmp/size_output.txt

# 3. 生成多种格式
python3 scripts/analyze_size_output.py /tmp/size_output.txt --format json > report.json
python3 scripts/analyze_size_output.py /tmp/size_output.txt --format csv > report.csv
```

### 方案2: map 文件深度分析

适合深度优化、问题定位、符号级分析：

```bash
# 1. 找到 map 文件
cd out/your_platform/
ls -lh *.map

# 2. 基本分析
python3 ../../skills/codesize/scripts/analyze_map_file.py your_project.map

# 3. 显示符号名（函数名/变量名）
python3 scripts/analyze_map_file.py your_project.map --show-symbols --top 30

# 4. 生成 JSON 用于程序处理
python3 scripts/analyze_map_file.py your_project.map --format json -o report.json
```

### 方案3: 版本对比分析

适合验证优化效果、跟踪代码膨胀：

```bash
# 1. 优化前采集基线
size -t apps/**/*.a > /tmp/before.txt

# 2. 实施优化并重新编译
# ... 修改代码、配置 ...

# 3. 优化后采集
size -t apps/**/*.a > /tmp/after.txt

# 4. 对比分析
python3 scripts/compare_codesize.py /tmp/before.txt /tmp/after.txt

# 5. 生成对比报告
python3 scripts/compare_codesize.py /tmp/before.txt /tmp/after.txt \
    --threshold 5 \
    --output diff_report.md
```

### 多核心系统分析

对于包含多个处理器核心的系统（如 AP + Audio + CP + Sensor），需要分别分析每个核心：

```bash
# 设置环境（根据实际项目调整）
export PROJECT_ROOT=~/ssd/vela_xxx
export SKILL_PATH=~/.copilot/skills/codesize

# ===== 分析 AP 核心 (ARM 主核心) =====
export PLATFORM_AP=<platform>_ap          # 如：xxx_ap, platform_ap
export LLVM_BIN="$PROJECT_ROOT/prebuilts/clang-arm/linux-x86_64/bin"

cd $PROJECT_ROOT/out/$PLATFORM_AP

# 快速分析
$LLVM_BIN/llvm-size -t apps/**/*.a > /tmp/ap_size.txt
python3 $SKILL_PATH/scripts/analyze_size_output.py /tmp/ap_size.txt --top 30

# 深度分析（推荐）
python3 $SKILL_PATH/scripts/analyze_map_file.py \
    nuttx.map \
    --show-symbols --top 50 \
    --output /tmp/ap_analysis.md

# ===== 分析 Audio 核心 (Xtensa DSP) =====
export PLATFORM_AUDIO=<platform>_audio    # 如：xxx_audio, platform_audio
export XTENSA_BIN="$PROJECT_ROOT/prebuilts/clang-xtensa/linux-x86_64/bin"

cd $PROJECT_ROOT/out/$PLATFORM_AUDIO

# 快速分析
$XTENSA_BIN/xt-size -t apps/**/*.a > /tmp/audio_size.txt
python3 $SKILL_PATH/scripts/analyze_size_output.py /tmp/audio_size.txt --top 20

# 深度分析
python3 $SKILL_PATH/scripts/analyze_map_file.py \
    nuttx.map \
    --show-symbols --top 30 \
    --output /tmp/audio_analysis.md

# ===== 多核心对比汇总 =====
# 生成所有核心的大小汇总
echo "=== 多核心代码大小汇总 ===" > /tmp/multi_core_summary.txt
for core in ap audio cp sensor; do
    CORE_DIR="$PROJECT_ROOT/out/<platform>_$core"
    if [ -d "$CORE_DIR" ]; then
        echo "" >> /tmp/multi_core_summary.txt
        echo "=== ${core^^} 核心 ===" >> /tmp/multi_core_summary.txt
        # 使用对应架构的工具
        case $core in
            ap) $LLVM_BIN/llvm-size $CORE_DIR/nuttx* 2>/dev/null ;;
            audio) $XTENSA_BIN/xt-size $CORE_DIR/nuttx* 2>/dev/null ;;
        esac >> /tmp/multi_core_summary.txt
    fi
done

cat /tmp/multi_core_summary.txt

# ===== 版本对比 (优化前后) =====
# 对每个核心进行版本对比
python3 $SKILL_PATH/scripts/compare_codesize.py \
    /tmp/ap_size_before.txt \
    /tmp/ap_size.txt \
    --threshold 5 \
    --output /tmp/ap_diff.md
```

**分析要点**：
- **AP 核心**：关注 `.text` (代码段)、`.data` (初始化数据)、`.bss` (未初始化数据)
- **Audio 核心**：关注 `.iram` (内部 RAM)、`.dram` (数据 RAM)、DSP 相关段
- **版本对比**：使用 compare_codesize.py 追踪每次优化的效果
- **架构差异**：不同核心使用对应架构的工具链（ARM 用 llvm-size，Xtensa 用 xt-size）

---

## 输出格式与CI/CD集成

### 支持的输出格式

| 格式 | 参数 | 用途 |
|------|------|------|
| Markdown | `--format markdown` (默认) | 人工阅读、文档 |
| JSON | `--format json` | CI/CD 集成、程序处理 |
| CSV | `--format csv` | Excel 分析、数据导入 |

### CI/CD 集成示例

```bash
#!/bin/bash
# ci_codesize_check.sh

set -e

BUILD_OUTPUT="out/${PLATFORM}"
MAX_FLASH_KB=512

# 1. 编译
./build.sh ${PLATFORM}

# 2. 采集大小数据
cd ${BUILD_OUTPUT}
size -t **/*.a > /tmp/size.txt

# 3. 生成 JSON 报告
python3 scripts/analyze_size_output.py /tmp/size.txt \
    --format json \
    --output artifacts/codesize.json

# 4. 阈值检查（超过则失败）
python3 scripts/analyze_size_output.py /tmp/size.txt \
    --fail-on-threshold ${MAX_FLASH_KB}

echo "✅ 代码大小检查通过"
```

### 版本对比自动化

```bash
#!/bin/bash
# ci_codesize_diff.sh

# 获取上一次成功构建的数据
wget ${CI_ARTIFACTS_URL}/previous/codesize.txt -O /tmp/before.txt

# 当前构建
size -t **/*.a > /tmp/after.txt

# 对比
python3 scripts/compare_codesize.py /tmp/before.txt /tmp/after.txt \
    --format json \
    --fail-on-growth 5 \
    --output artifacts/diff.json

# 如果增长超过 5%，脚本会返回非零退出码
```

---

## 优化策略

### 编译优化

```bash
# 推荐编译选项
CFLAGS += -Os                      # 优化大小
CFLAGS += -ffunction-sections      # 每个函数单独 section
CFLAGS += -fdata-sections          # 每个数据单独 section
LDFLAGS += -Wl,--gc-sections       # 移除未使用代码
LDFLAGS += -flto                   # 链接时优化
```

### 代码优化

| 优化类型 | 方法 |
|----------|------|
| 移除未使用代码 | 使用 --gc-sections，检查条件编译 |
| 减少内联 | 控制 inline 使用，使用 -finline-limit |
| 数据结构优化 | 减少 padding，使用紧凑类型 |
| 字符串优化 | 使用字符串池，减少重复 |
| 表驱动替代 | 用计算替代大查找表 |

### 数据优化

| 段 | 优化方法 |
|-----|----------|
| .rodata | 压缩嵌入资源、外部存储 |
| .bss | 动态分配替代静态缓冲区 |
| .data | 改为 const 或运行时初始化 |

---

## 常见问题

### Q1: size 命令输出格式不对？

确保使用 `-t` 参数：
```bash
# 正确 ✅
size -t path/to/*.a > output.txt

# 错误 ❌
size path/to/*.a > output.txt
```

### Q2: map 文件解析数据为空？

检查 map 文件格式，工具支持标准 GNU ld 格式：
```
.text           0x11580000   0x3e3e4c
 .text.function 0x11580000   0x1234  path/lib.a(obj.o)
```

### Q3: 如何分析 C++ 项目？

使用 `--show-symbols` 选项，C++ 符号会标记 🔷：
```bash
python3 scripts/analyze_map_file.py proj.map --show-symbols
```

对于需要反混淆的符号，使用 `c++filt`：
```bash
echo "_ZN5ClassMethod" | c++filt
```

### Q4: CI 中如何设置阈值？

使用 `--fail-on-threshold` 参数：
```bash
# Flash 超过 512KB 时返回非零退出码
python3 scripts/analyze_size_output.py size.txt --fail-on-threshold 512
```

### Q5: 如何只分析特定模块？

使用 shell 通配符：
```bash
# 只分析 multimedia 模块
size -t **/multimedia/**/*.a > multimedia.txt
python3 scripts/analyze_size_output.py multimedia.txt --module "multimedia"
```

---

## 参考命令速查

```bash
# ELF 文件信息
size <elf_file>
readelf -S <elf_file>

# 符号分析
nm -S <file>              # 显示符号大小
nm -u <file>              # 未定义符号

# C++ 符号
nm <file> | c++filt

# 对比工具
diff <file1> <file2>
bloaty <elf>              # 推荐的大小分析工具
```

---

## 更新日志

### v2.0 (2026-01-29)
- ✨ 新增版本对比工具 `compare_codesize.py`
- ✨ 增强符号名解析（提取函数名/变量名）
- ✨ 添加类型注解，提高代码质量
- ✨ 支持 JSON/CSV 输出格式
- ✨ 添加 `--fail-on-threshold` 支持 CI/CD
- 📝 优化 SKILL.md 文档结构
- 🔧 抽取公共模块 `codesize_utils.py`

### v1.0
- 初始版本
- 基础 size 和 map 分析
