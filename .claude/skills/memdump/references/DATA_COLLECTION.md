# 数据采集详细指南

本文档介绍如何采集memdump日志数据，包括串口工具使用、memdump命令、多核系统处理等。

## 使用串口工具采集日志

### minicom
```bash
# 连接设备并保存日志到文件
minicom -D /dev/ttyUSB0 -b 921600 -C tmp.log

# 常用参数
# -D: 串口设备路径
# -b: 波特率
# -C: 日志文件路径
```

### picocom
```bash
# 连接设备并记录日志
picocom -b 921600 /dev/ttyUSB0 --logfile tmp.log

# 退出: Ctrl+A Ctrl+X
```

### 其他工具
- **screen**: `screen /dev/ttyUSB0 921600 -L -Logfile tmp.log`
- **cu**: `cu -l /dev/ttyUSB0 -s 921600 | tee tmp.log`

---

## memdump 命令使用

### 基本用法

```bash
# 显示所有线程的内存分配
nsh> memdump

# 只显示指定PID的分配
nsh> memdump 12

# 某些系统可能需要指定输出位置
nsh> memdump > /tmp/memdump.log
```

### 多核系统注意事项

多核系统需要在每个核上分别执行memdump：

#### 方法1: 核心切换命令
```bash
nsh> cpu 0  # 切换到 CPU0
nsh> memdump
nsh> cpu 1  # 切换到 CPU1
nsh> memdump
```

#### 方法2: 自动核心标识
```bash
# 如果日志自动包含核心标识
nsh> memdump  # 输出会自动标记 [CPU0] 或 [CPU1]
```

#### 方法3: 核心特定命令
```bash
# 使用核心特定的命令前缀
nsh> memdump_cpu0
nsh> memdump_cpu1
```

---

## 常见采集场景

### 场景1: 评估启动后内存占用
```bash
# 系统启动完成，等待稳定（如30秒）
nsh> memdump     # 全部线程
nsh> memdump 12  # 指定 PID
```

### 场景2: 事件前后对比
```bash
# 事件前采样
nsh> memdump 12 > /tmp/before.log

# 执行事件（如播放音频、接听电话）
nsh> play test.wav

# 事件后采样
nsh> memdump 12 > /tmp/after.log
```

### 场景3: 泄漏检测（多时间点）
```bash
# T1: 基准采样
nsh> memdump 12 > /tmp/baseline.log

# 执行操作10次
# ... 重复操作 ...

# T2: 中间采样
nsh> memdump 12 > /tmp/mid.log

# 再执行10次
# ... 重复操作 ...

# T3: 最终采样
nsh> memdump 12 > /tmp/final.log
```

---

## memdump 输出格式说明

工具支持多种日志格式，会自动检测并适配。

### 格式1: Vela标准格式（带Sequence）
```plaintext
PID    Size Overhead Sequence    Address  Backtrace
 12     424       20   781103  0x255b6a0  0x115d1136 0x115b62ba 0x115b6441 ...
 │       │        │       │         │           └── 调用栈地址（最多8层）
 │       │        │       │         └── 内存块地址
 │       │        │       └── 分配序列号（全局递增，唯一标识）
 │       │        └── 管理开销（通常 16-32 字节）
 │       └── 有效数据大小（用户请求的大小）
 └── 进程/线程 ID
```

### 格式2: 带时间戳和核心标识
```plaintext
[01/30 02:18:32.751630] [ 7] [ap]     12          56       24 0x202d27a8 0x101382d6
 └── 时间戳              └── 核心ID  └── PID  Size  Overhead  Address   Backtrace
```

### 格式3: NuttX简化格式
```plaintext
 12     424       20  0x255b6a0  0x115d1136 0x115b62ba
 └── PID  Size  Overhead Address  Backtrace（无Sequence）
```

### 字段说明

| 字段 | 说明 | 示例 |
|------|------|------|
| PID | 进程/线程 ID | 12 |
| Size | 有效数据大小（用户请求的字节数） | 424 |
| Overhead | 内存管理器开销（元数据、对齐） | 20 |
| Sequence | 分配序列号（全局递增，唯一标识） | 781103 |
| Address | 内存块地址 | 0x255b6a0 |
| Backtrace | 调用栈地址（最多8层） | 0x115d1136 0x115b62ba ... |

**重要**：Sequence字段用于追踪同一分配在不同时间点的状态，是检测泄漏的关键。

---

## 多核日志识别

### 常见核心标识方式

#### 方式1: 日志前缀标记
```plaintext
[CPU0][01/02 12:22:36] Memdump task mediad
[CPU0] 12     424       20   781103  0x255b6a0  0x115d1136...
[CPU1][01/02 12:22:40] Memdump task audio_sink
[CPU1] 15     1024      24   781200  0x355a8c0  0x215c2246...
```

**识别策略**：在每行日志中查找 `[CPU0]` 或 `[CPU1]` 前缀

#### 方式2: 独立日志文件
```bash
# 每个核心单独输出到不同文件
tmp_cpu0.log  # CPU0 的日志
tmp_cpu1.log  # CPU1 的日志
```

**识别策略**：通过文件名或目录结构区分

#### 方式3: 地址范围识别
```plaintext
# 不同核心的地址范围不同
CPU0: 0x1xxxxxxx (地址以 0x1 开头)
CPU1: 0x2xxxxxxx (地址以 0x2 开头)

# 通过 backtrace 地址判断核心
12  424  20  781103  0x155b6a0  0x115d1136...  ← CPU0
15  1024 24  781200  0x255a8c0  0x215c2246...  ← CPU1
```

**识别策略**：分析Address或Backtrace地址的高位，判断所属核心

#### 方式4: 任务名称约定
```plaintext
# 某些任务只在特定核心运行
Memdump task mediad           ← 已知运行在 CPU0
Memdump task audio_dsp        ← 已知运行在 CPU1
```

**识别策略**：维护任务到核心的映射表

### AI自动识别策略

```python
def detect_core_from_log(log_lines):
    """
    从日志上下文识别当前 memdump 属于哪个核心
    """
    # 策略1: 查找日志前缀
    for line in log_lines:
        if match := re.search(r'\[(CPU\d+)\]', line):
            return match.group(1).lower()  # 返回 'cpu0' 或 'cpu1'
    
    # 策略2: 分析地址范围
    addresses = extract_addresses(log_lines)
    if addresses:
        first_addr = int(addresses[0], 16)
        if first_addr & 0x10000000:
            return 'cpu0'
        elif first_addr & 0x20000000:
            return 'cpu1'
    
    # 策略3: 任务名称匹配
    task_name = extract_task_name(log_lines)
    if task_name in CPU0_TASKS:
        return 'cpu0'
    elif task_name in CPU1_TASKS:
        return 'cpu1'
    
    # 策略4: 询问用户
    return 'unknown'

def select_elf_file(core_id, elf_mapping):
    """
    根据核心ID选择对应的ELF文件
    """
    return elf_mapping.get(core_id, elf_mapping.get('default'))
```

---

## 配置文件示例

### 单核系统配置
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

### 多核系统配置
```json
{
  "cores": {
    "cpu0": {
      "elf_file": "./cpu0_vela.elf",
      "addr2line": "arm-none-eabi-addr2line",
      "architecture": "arm",
      "address_prefix": "0x1",
      "tasks": ["mediad", "media_trigger"]
    },
    "cpu1": {
      "elf_file": "./cpu1_vela.elf",
      "addr2line": "xtensa-esp32-elf-addr2line",
      "architecture": "xtensa",
      "address_prefix": "0x2",
      "tasks": ["audio_dsp", "codec_task"]
    }
  }
}
```

### 配置字段说明

| 字段 | 说明 | 示例 |
|------|------|------|
| elf_file | ELF调试文件路径 | "./cpu0_vela.elf" |
| addr2line | addr2line工具路径 | "arm-none-eabi-addr2line" |
| architecture | 架构类型 | "arm", "xtensa", "riscv" |
| address_prefix | 地址前缀（用于自动识别） | "0x1", "0x2" |
| tasks | 运行在该核心的任务列表 | ["mediad", "media_trigger"] |

---

## 数据采集最佳实践

### 1. 采样时机
- ✅ 系统稳定后采集（启动完成、空闲30秒）
- ✅ 事件执行前后立即采集
- ✅ 多次采样确保数据可靠性
- ❌ 避免在系统启动中采集（分配未完成）

### 2. 多时间点采样
- 至少采集3个时间点（基准、中间、最终）
- 明确标记采样时间和操作步骤
- 保持操作一致性（相同的触发条件）

### 3. 日志质量
- 保存完整日志（不要截断）
- 包含上下文信息（任务名、时间戳）
- 单独保存每次采样（不要覆盖）

### 4. 多核采集
- 确认每个核心都执行了memdump
- 验证核心标识清晰可见
- 记录核心间的任务分配关系

### 5. 文档记录
- 记录采集时间和操作步骤
- 记录系统状态和关键参数
- 记录异常现象和观察
