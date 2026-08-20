# 脚本工具详细说明

本文档介绍memdump分析相关的Python脚本工具。

## 工具概览

| 工具 | 功能 | 依赖 | 速度 |
|------|------|------|------|
| memdump_quick.py | 快速分析、热点检测 | 无 | <1秒 |
| memdump_parser.py | 源码级Backtrace解析 | ELF+addr2line | 数十秒 |
| memdump_diff.py | 两时间点差异对比 | ELF+addr2line | 数十秒 |
| memdump_leak_detect.py | 多时间点泄漏检测 | ELF+addr2line | 数十秒 |

---

## memdump_quick.py

### 功能特性
- ✅ 零依赖，纯Python实现
- ✅ 自动检测日志格式
- ✅ 诊断模式显示解析质量
- ✅ 支持JSON导出
- ✅ 按大小/次数排序热点

### 使用方法

```bash
# 诊断模式（推荐首次使用）
python3 scripts/memdump_quick.py log.txt --diagnose

# 分析所有PID
python3 scripts/memdump_quick.py log.txt

# 分析特定PID
python3 scripts/memdump_quick.py log.txt 12

# 导出JSON
python3 scripts/memdump_quick.py log.txt 12 --json output.json
```

### 输出格式

**文本格式：**
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
```

**JSON格式：**
```json
{
  "pid": 12,
  "total_allocations": 1827,
  "total_size": 226241,
  "total_overhead": 43428,
  "hotspots": [
    {
      "address": "0x10137438",
      "count": 24,
      "total_size": 72042,
      "avg_size": 3001.7
    }
  ]
}
```

---

## memdump_parser.py

### 功能特性
- 解析Backtrace到源码位置（函数名、文件:行号）
- 支持单核和多核系统
- 自动检测核心ID
- 按任务统计内存占用

### 配置文件

**单核系统：**
```json
{
  "cores": {
    "default": {
      "elf_file": "./vela_audio.elf",
      "addr2line": "xtensa-esp32-elf-addr2line",
      "architecture": "xtensa"
    }
  }
}
```

**多核系统：**
```json
{
  "cores": {
    "cpu0": {
      "elf_file": "./cpu0_vela.elf",
      "addr2line": "arm-none-eabi-addr2line",
      "architecture": "arm",
      "tasks": ["mediad", "media_trigger"]
    },
    "cpu1": {
      "elf_file": "./cpu1_vela.elf",
      "addr2line": "xtensa-esp32-elf-addr2line",
      "architecture": "xtensa",
      "address_prefix": "0x2",
      "tasks": ["audio_dsp"]
    }
  }
}
```

### 使用方法

```bash
# 单核系统
python3 scripts/memdump_parser.py log.txt

# 多核系统（需配置文件）
python3 scripts/memdump_parser.py log.txt multi_core_config.json

# 分析特定PID
python3 scripts/memdump_parser.py log.txt 12 config.json
```

---

## memdump_diff.py

### 功能特性
- 基于序列号(Sequence)精确追踪分配变化
- 识别被释放、新增、保留的分配
- 解析差异部分的Backtrace

### 使用方法

```bash
# 对比两个时间点
python3 scripts/memdump_diff.py before.log after.log

# 多核系统
python3 scripts/memdump_diff.py before.log after.log multi_core_config.json
```

### 输出示例

```
线程: mediad (PID 12)
  变化前: 1536 个分配, 179,096 bytes
  变化后: 1579 个分配, 208,400 bytes
  
  保留: 1535 个分配
  释放: 1 个分配 (424 bytes)
  新增: 44 个分配 (29,728 bytes)
  
新增分配 Top 10:
  1,312 bytes ×21  posix_memalign → av_buffer_alloc
    80 bytes ×18  posix_memalign → av_mallocz
```

---

## memdump_leak_detect.py

### 功能特性
- 多时间点（≥3个）趋势分析
- 按前3层Backtrace分组
- 识别持续增长的调用路径
- 计算增长速率

### 使用方法

```bash
# 三个时间点
python3 scripts/memdump_leak_detect.py baseline.log mid.log final.log

# 带配置文件（多核）
python3 scripts/memdump_leak_detect.py t1.log t2.log t3.log config.json
```

### 输出示例

```
时间点分析:
  T1 (baseline.log): 1536 个分配
  T2 (mid.log):      1680 个分配 (+144)
  T3 (final.log):    1824 个分配 (+288)
  
平均增长率: 每次操作 +14.4 个分配

持续增长的Backtrace:
  ⚠️ 0x115d114a → 0x115b62ba → 0x115fd67d
     T1: 100个 → T2: 121个 → T3: 142个
     每次操作: +2.1个分配
     
     解析:
     posix_memalign → av_malloc → av_frame_alloc
     [frame.c:152]
```

---

## analyze.sh - 快速启动脚本

### 功能
统一入口，简化工具调用

### 使用方法

```bash
# 安装（添加执行权限）
chmod +x scripts/analyze.sh

# 统计分析
scripts/analyze.sh stat log.txt

# 差异对比
scripts/analyze.sh diff before.log after.log

# 泄漏检测
scripts/analyze.sh leak t1.log t2.log t3.log

# 带配置文件
scripts/analyze.sh stat log.txt config.json
```

---

## 脚本实现位置

所有脚本位于 `scripts/` 目录下：
- `scripts/memdump_quick.py`
- `scripts/memdump_parser.py`
- `scripts/memdump_diff.py`
- `scripts/memdump_leak_detect.py`
- `scripts/analyze.sh`
- `scripts/multi_core_config.json` (配置示例)

## 自定义和扩展

脚本设计为易于扩展，可以：
1. 修改Backtrace分组深度（默认前3层）
2. 调整热点数量（默认Top 10）
3. 添加自定义过滤器
4. 修改输出格式

具体实现请查看各脚本的源码，代码注释详细。

---

## 常见用法组合

### 场景1: 初次分析
```bash
# Step 1: 诊断
scripts/analyze.sh stat log.txt --diagnose

# Step 2: 快速分析
scripts/analyze.sh stat log.txt 12

# Step 3: 如需源码定位
scripts/analyze.sh stat log.txt 12 config.json
```

### 场景2: 泄漏检测完整流程
```bash
# 采集数据（在设备上）
nsh> memdump 12 > /tmp/t1.log
# ... 执行操作10次 ...
nsh> memdump 12 > /tmp/t2.log
# ... 再执行10次 ...
nsh> memdump 12 > /tmp/t3.log

# 分析（在开发机上）
scripts/analyze.sh leak t1.log t2.log t3.log config.json
```

### 场景3: 版本对比
```bash
# 对比新旧版本
scripts/analyze.sh diff old_version.log new_version.log config.json
```
