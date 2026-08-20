---
name: memdump
description: Analyzes heap memory usage from NuttX/Vela runtime memdump logs, detects memory leaks, and identifies high-consumption modules. Use when debugging memory issues, tracking heap allocations, comparing memory usage between versions, or optimizing embedded system memory footprint.
metadata:
  version: "1.0"
  target-system: "NuttX/Vela"
---

# Memdump Analysis Skill

## 目标
分析嵌入式系统（NuttX/FreeRTOS）运行时的 heap 内存占用，识别内存泄漏，定位高内存消耗模块，并提供优化建议。

## 适用场景
- 分析系统启动后各功能模块的内存占用情况
- 检测特定功能（播放/录音/通话）前后的内存变化
- 定位和验证内存泄漏问题
- 对比不同版本的内存使用差异
- 评估内存优化效果

---

## 快速开始 ⚡

### 推荐工具：memdump_quick.py

**适用场景**：快速评估、热点分析、无需ELF文件的基础统计

**核心优势**：
- ✅ 自动检测日志格式（支持多种格式）
- ✅ 零配置，完全独立运行
- ✅ 提供诊断模式，显示解析成功率
- ✅ 支持JSON导出
- ✅ <1秒完成分析

**基础用法**：
```bash
# 1. 快速诊断（检查日志格式和质量）
python3 scripts/memdump_quick.py log.txt --diagnose

# 2. 分析所有进程
python3 scripts/memdump_quick.py log.txt

# 3. 分析特定PID
python3 scripts/memdump_quick.py log.txt 12

# 4. 导出JSON格式
python3 scripts/memdump_quick.py log.txt 12 --json report.json
```

**输出示例**：
```
PID 12 内存分析
分配次数: 1,827
有效数据: 220.92 KB
管理开销: 42.42 KB
总内存:   263.34 KB
平均分配: 123.8 bytes

Top 10 分配热点 (按大小)
调用地址        次数    总大小      平均      占比
0x10137438      24     70.4 KB    3001.7 B  31.8%
0x10137828      26     44.4 KB    1746.8 B  20.1%
0x10137d96     646     37.0 KB      58.7 B  16.8%
```

---

## 工具选择指南

| 场景 | 推荐工具 | 特点 | ELF要求 |
|------|---------|------|---------|
| 初步评估 | `memdump_quick.py` | 快速、无依赖、诊断模式 | ❌ 不需要 |
| 热点分析 | `memdump_quick.py` | 地址级热点、大小分布 | ❌ 不需要 |
| 源码定位 | `memdump_parser.py` | 解析为函数名和行号 | ✅ 必需 |
| 泄漏检测 | `memdump_leak_detect.py` | 多时间点趋势分析 | ✅ 推荐 |
| 版本对比 | `memdump_diff.py` | 差异分析 | ✅ 推荐 |

### 分阶段分析流程 🎯

**阶段1: 快速评估（1分钟）**
```bash
python3 scripts/memdump_quick.py log.txt --diagnose
```
**输出**: 格式识别、解析成功率、进程数量

**阶段2: 统计分析（2分钟）**
```bash
python3 scripts/memdump_quick.py log.txt 12
```
**输出**: 内存统计、热点Top 10、大小分布

**阶段3: 深度溯源（需要ELF）**
```bash
python3 scripts/memdump_parser.py log.txt 12 config.json
```
**输出**: 源码级定位（函数名、文件:行号）

---

## 快速故障排查

### 问题1: 无法识别日志格式
```
❌ 错误: 无法识别日志格式
```

**解决方案**:
1. 确认日志包含memdump输出（搜索PID、Size、Address等关键字）
2. 使用 `--diagnose` 查看详细信息
3. 检查日志格式是否匹配支持的格式（见[DATA_COLLECTION.md](references/DATA_COLLECTION.md)）

### 问题2: 解析失败率高（>10%）
```
⚠️ 警告: 失败率偏高，可能存在格式问题
```

**解决方法**：通常不影响分析，只要成功率>90%即可。如果成功率<50%，手动检查日志格式。

### 问题3: 找不到目标PID
```
❌ 未找到PID 12的数据
```

**解决方案**：先不指定PID，查看所有进程列表，确认PID是否正确。

---

## 前置条件

### 必备文件
- **设备日志文件**（如 `tmp.log`, `startup.log`）
  - 包含 `memdump` 命令的输出
  - 可包含多个时间点的采样数据
  - 多核系统需包含核心标识信息

- **ELF 调试文件**（仅深度分析需要）
  - 单核: `vela_audio.elf`
  - 多核: 每个核心对应的 ELF 文件（如 `cpu0_vela.elf`, `cpu1_vela.elf`）
  - 必须包含调试符号（未 stripped）

- **addr2line 工具**（仅深度分析需要）
  - 标准 binutils: `addr2line`
  - 或特定架构: `xtensa-esp32-elf-addr2line`, `arm-none-eabi-addr2line`

### 支持的日志格式

工具自动检测以下格式：

**格式1: Vela标准格式（带Sequence）**
```plaintext
PID    Size Overhead Sequence    Address  Backtrace
 12     424       20   781103  0x255b6a0  0x115d1136 0x115b62ba ...
```

**格式2: 带时间戳和核心标识**
```plaintext
[01/30 02:18:32.751630] [ 7] [ap]     12   56   24 0x202d27a8 0x101382d6
```

**格式3: NuttX简化格式**
```plaintext
 12     424       20  0x255b6a0  0x115d1136 0x115b62ba
```

---

## 详细文档目录

本skill提供以下详细参考文档：

### [DATA_COLLECTION.md](references/DATA_COLLECTION.md)
- 使用minicom/picocom采集日志
- memdump命令详细使用方法
- 多核系统日志采集策略
- 日志格式详细说明
- 多核日志识别方法

### [ANALYSIS_GUIDE.md](references/ANALYSIS_GUIDE.md)
- 场景一：基础Heap分析
- 场景二：事件前后差异对比
- 场景三：内存泄漏调试完整流程
- 泄漏模式识别（事件处理、错误路径、循环引用、缓存增长）
- 泄漏定位和验证方法

### [PROMPTS.md](references/PROMPTS.md)
- 基础分析Prompt模板
- 差异对比Prompt模板
- 泄漏检测Prompt模板（初步/深度/多时间点）
- Backtrace批量解析模板
- 单核/多核场景示例

### [SCRIPTS.md](references/SCRIPTS.md)
- memdump_parser.py详细说明
- memdump_diff.py使用方法
- memdump_leak_detect.py泄漏检测
- memdump_quick.py快速分析
- 配置文件编写指南

### [BEST_PRACTICES.md](references/BEST_PRACTICES.md)
- 数据采集最佳实践
- 分析策略建议
- 工具选择策略
- 与AI协作技巧
- 泄漏验证checklist
- 优化建议
- 常见问题完整解决方案

---

## 典型使用示例

### 示例1: 快速评估启动后内存
```bash
# Step 1: 诊断日志
python3 scripts/memdump_quick.py startup.log --diagnose

# Step 2: 分析目标PID
python3 scripts/memdump_quick.py startup.log 12
```

### 示例2: 分析事件前后差异
```bash
# 使用memdump_diff.py对比
python3 scripts/memdump_diff.py before_wakeup.log after_wakeup.log

# 或直接提示AI
# "请对比before_wakeup.log和after_wakeup.log，分析L1 wakeup事件对mediad线程的内存影响"
```

### 示例3: 检测内存泄漏
```bash
# 采集三个时间点
nsh> memdump 12 > baseline.log    # 稳定后
# [执行操作10次]
nsh> memdump 12 > mid.log
# [再执行10次]
nsh> memdump 12 > final.log

# 使用工具检测
python3 scripts/memdump_leak_detect.py baseline.log mid.log final.log

# 或提示AI
# "请分析这三个时间点，检测播放操作是否存在内存泄漏"
```

---

## 工具性能对比

| 工具 | 分析速度 | 依赖要求 | 输出格式 | 适用场景 |
|------|---------|---------|---------|---------|
| memdump_quick.py | <1秒 | 无 | 文本+JSON | 快速评估、热点分析 |
| memdump_parser.py | 数十秒 | ELF+addr2line | 文本 | 源码定位 |
| memdump_leak_detect.py | 数十秒 | ELF+addr2line | 文本 | 泄漏检测 |
| memdump_diff.py | 数十秒 | ELF+addr2line | 文本 | 版本对比 |

**推荐工作流**：
1. 先用 memdump_quick.py 快速评估（1分钟内）
2. 发现问题后用 memdump_parser.py 深度分析（需ELF）
3. 如需追踪趋势用 memdump_leak_detect.py

---

## 脚本位置

所有分析脚本位于 `scripts/` 目录下：
- `scripts/memdump_quick.py` - 快速分析（无依赖）
- `scripts/memdump_parser.py` - 解析器（需ELF）
- `scripts/memdump_diff.py` - 差异对比
- `scripts/memdump_leak_detect.py` - 泄漏检测
- `scripts/analyze.sh` - 快速启动脚本
- `scripts/multi_core_config.json` - 多核配置示例

---

## 进阶主题

### 多核系统分析
多核系统（如ARM+Xtensa双核）需要特殊处理，包括：
- 核心识别策略（日志前缀、地址范围、任务名称）
- 配置文件编写
- ELF文件和工具链映射

详见 [DATA_COLLECTION.md - 多核日志识别](references/DATA_COLLECTION.md#多核日志识别)

### 内存泄漏调试
完整的泄漏调试流程包括：
- 采样策略设计
- 多时间点对比
- Backtrace分组检测（关键方法）
- 泄漏模式识别
- 源码定位和修复
- 验证修复效果

详见 [ANALYSIS_GUIDE.md - 场景三：内存泄漏调试](references/ANALYSIS_GUIDE.md#场景三内存泄漏调试)

---

## 获取帮助

- 工具使用问题：查看 [SCRIPTS.md](references/SCRIPTS.md)
- 分析方法问题：查看 [ANALYSIS_GUIDE.md](references/ANALYSIS_GUIDE.md)
- 常见错误：查看 [BEST_PRACTICES.md - 常见问题处理](references/BEST_PRACTICES.md#常见问题处理)
- Prompt模板：查看 [PROMPTS.md](references/PROMPTS.md)
