# 最佳实践与常见问题

本文档提供memdump分析的最佳实践、工具使用技巧和常见问题解决方案。

---

## 最佳实践

### 1. 数据采集
- **多时间点采样**：至少采集 3 个时间点才能判断趋势
- **包含上下文**：记录操作步骤、时间戳、系统状态
- **保存原始日志**：完整保存 minicom 输出，便于后续重新分析
- **标记关键事件**：在日志中手动插入标记（如发送特定命令）

### 2. 分析策略
- **分阶段分析**：快速评估(memdump_quick.py) → 统计分析 → 深度溯源(memdump_parser.py)- **自顶向下**：先看整体统计，再深入异常点
- **序列号追踪**：利用 sequence 号追踪分配的生命周期
- **Backtrace 分组**：按调用栈分组是发现泄漏的关键
- **关注增长率**：重点分析增长率而非绝对值

### 3. 工具选择策略
```
快速评估场景:
  ├─ 无ELF文件 → 使用 memdump_quick.py --diagnose
  ├─ 查看热点 → 使用 memdump_quick.py <log> <pid>
  └─ 需要导出 → 使用 memdump_quick.py --json

深度分析场景:
  ├─ 源码定位 → 使用 memdump_parser.py（需ELF）
  ├─ 泄漏检测 → 使用 memdump_leak_detect.py
  └─ 版本对比 → 使用 memdump_diff.py
```

### 4. 与 AI 协作
- **明确分析目标**：指定线程、事件、时间段
- **提供足够上下文**：项目类型、RTOS、关键模块说明
- **迭代深入**：先统计概览，再逐步深入细节
- **保存分析脚本**：AI 生成的脚本可复用于新日志

### 5. 泄漏验证
- **可重复性测试**：确保泄漏现象可稳定复现
- **对照实验**：使用相同的测试流程验证修复
- **量化评估**：记录修复前后的具体数值（分配数、字节数）
- **长期监控**：将内存分析集成到 CI/CD 流程

### 6. 优化建议
- **优先级排序**：先解决泄漏，再优化占用
- **收益评估**：计算每个优化的预期内存节省
- **风险控制**：评估修改的影响范围和测试难度
- **文档记录**：记录优化措施和效果，形成知识库

---

## 工具使用技巧

### memdump_quick.py 技巧

#### 1. 使用诊断模式
```bash
# 首次分析必做：诊断日志格式
python3 scripts/memdump_quick.py log.txt --diagnose

# 输出：
# - 检测到的格式类型
# - 解析成功率
# - 发现的PID列表
```

#### 2. 导出JSON用于自动化
```bash
# 导出JSON格式，便于脚本处理
python3 scripts/memdump_quick.py log.txt 12 --json report.json

# JSON可用于：
# - CI/CD自动化检查
# - 趋势监控
# - 多版本对比
# - 报告生成
```

#### 3. 快速筛选目标PID
```bash
# 先不指定PID，查看所有进程
python3 scripts/memdump_quick.py log.txt

# 输出会显示所有PID及其统计，选择目标后：
python3 scripts/memdump_quick.py log.txt 12
```

### memdump_parser.py 技巧

#### 1. 配置文件复用
```bash
# 为项目创建配置文件template
cat > project_config.json <<EOF
{
  "cores": {
    "default": {
      "elf_file": "./out/vela/vela_audio.elf",
      "addr2line": "xtensa-esp32-elf-addr2line"
    }
  }
}
EOF

# 后续分析直接复用
python3 scripts/memdump_parser.py log1.txt config.json
python3 scripts/memdump_parser.py log2.txt config.json
```

#### 2. 批量解析多个日志
```bash
# 使用shell循环
for log in *.log; do
    echo "=== Analyzing $log ==="
    python3 scripts/memdump_parser.py "$log" config.json > "${log%.log}_analysis.txt"
done
```

### memdump_leak_detect.py 技巧

#### 1. 选择有代表性的时间点
```
❌ 错误：T1(0次) T2(1次) T3(2次)  # 样本太少
✅ 正确：T1(0次) T2(10次) T3(20次)  # 有足够差异

❌ 错误：T1 T2 T3 间隔5秒  # 系统未稳定
✅ 正确：T1 T2 T3 间隔5分钟  # 充分稳定
```

#### 2. 解读增长速率
```
场景: 播放音频测试
结果: 每次播放增加 3.5个分配，平均 1.2KB

判断:
- 如果是临时buffer，播放结束应该释放 → 可能泄漏
- 如果是缓存，合理的cache策略应该有上限 → 需要检查淘汰机制
- 如果是统计信息，可能是正常积累 → 评估长期影响
```

---

## 工具性能对比

| 工具 | 分析速度 | 依赖要 求 | 输出格式 | 适用场景 |
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

## 常见问题处理

### 问题0: 工具选择困惑
**现象**：不确定应该使用哪个工具

**解决方法**：
```bash
# 第一次分析 → 使用 memdump_quick.py
python3 scripts/memdump_quick.py log.txt --diagnose

# 发现异常需要源码定位 → 使用 memdump_parser.py
python3 scripts/memdump_parser.py log.txt 12 config.json

# 需要导出数据 → 使用 memdump_quick.py --json
python3 scripts/memdump_quick.py log.txt 12 --json report.json
```

### 问题1: addr2line 无法解析地址
**现象**：输出 `??:0` 或 `??:?`

**可能原因：**
- ELF 文件被 strip 掉调试符号
- 地址不在 ELF 的地址空间范围内
- 使用了错误的工具链

**解决方法：**
```bash
# 1. 检查是否有调试符号
readelf -S vela_audio.elf | grep debug

# 如果无输出，说明符号被strip，需要使用未strip的ELF

# 2. 检查地址是否在有效范围
readelf -S vela_audio.elf | grep -E "\.text|\.rodata"

# 对比地址范围，确认address在其中

# 3. 使用正确的工具链
xtensa-esp32-elf-addr2line -e vela_audio.elf -f -C 0x115d114a
```

### 问题2: memdump 日志格式不一致
**现象**：解析脚本报错或解析结果不准确

**可能原因：**
- 不同 NuttX/FreeRTOS 版本格式差异
- memdump 输出被其他日志干扰
- backtrace 层数不足 8 层

**解决方法：**
```bash
# 1. 使用 memdump_quick.py --diagnose 查看详情
python3 scripts/memdump_quick.py log.txt --diagnose

# 2. 提取纯净的memdump数据
grep -A 1000 "Memdump task" log.txt | grep "^[ ]*[0-9]" > clean.txt

# 3. 向AI提供日志样例，让其适配
# "这是我的日志格式:
# [时间戳] PID Size Overhead Address Backtrace
# 请帮我解析"
```

### 问题3: 统计结果与实际不符
**现象**：分析报告的总内存与预期不符

**可能原因：**
- 遗漏某些线程的分配
- overhead 计算方式不同
- 日志采集不完整

**解决方法：**
```bash
# 1. 检查日志完整性
grep "Memdump task" log.txt
# 确认所有目标线程都有memdump输出

# 2. 与 free 命令对比
nsh> free
# 对比总体内存使用情况

# 3. 重新采集完整日志
nsh> memdump > /tmp/complete.log
```

### 问题4: 无法定位泄漏源
**现象**：发现泄漏但 backtrace 太短

**可能原因：**
- backtrace 深度不够（只有 2-3 层）
- 调用来自函数指针或虚函数
- 优化导致内联

**解决方法：**
```bash
# 1. 配置更深的 backtrace（如果系统支持）
# 修改 NuttX 配置，增加 backtrace 层数

# 2. 关闭优化重新编译（调试版本）
# CFLAGS += -O0 -g

# 3. 结合源码分析
# 查看仅有的 backtrace 层，手动追踪调用链

# 4. 使用内存泄漏检测工具
# Valgrind（如果平台支持）
# AddressSanitizer（ASAN）
```

### 问题5: 多核日志混乱
**现象**：无法区分哪些分配来自哪个核心

**可能原因：**
- 日志没有核心标识
- 不同核心的日志混杂

**解决方法：**
```bash
# 方法1: 修改日志格式，添加核心前缀
# 在memdump命令前添加标记
nsh> echo "[CPU0] START"
nsh> memdump
nsh> echo "[CPU0] END"

# 方法2: 分别采集到不同文件
# CPU0: nsh> memdump > /tmp/cpu0.log
# CPU1: nsh> memdump > /tmp/cpu1.log

# 方法3: 使用地址范围识别
# 分析address字段，不同核心通常有不同地址范围
```

### 问题6: Python脚本执行失败
**现象**：ImportError, ModuleNotFoundError

**解决方法：**
```bash
# 检查Python版本（需要3.7+）
python3 --version

# 安装依赖（如果需要）
# memdump_quick.py 无依赖
# 其他脚本可能需要：
pip3 install --user regex

# 检查脚本权限
chmod +x memdump_*.py

# 使用绝对路径
python3 /path/to/memdump_quick.py log.txt
```

### 问题7: 分析速度慢
**现象**：memdump_parser.py 执行时间过长

**可能原因：**
- 日志文件很大（>10MB）
- backtrace 解析调用 addr2line 次数过多
- ELF 文件很大

**解决方法：**
```bash
# 1. 只分析目标PID
python3 scripts/memdump_parser.py log.txt 12 config.json

# 2. 使用 memdump_quick.py 先筛选
python3 scripts/memdump_quick.py log.txt 12

# 3. 提取目标部分日志
grep "^[ ]*12 " log.txt > pid12.log
python3 scripts/memdump_parser.py pid12.log 12 config.json
```

---

## 泄漏调试检查清单

使用以下checklist系统化地进行泄漏调试：

### 数据采集阶段
- [ ] 采集多个时间点的 memdump（至少 3 个）
- [ ] 确保每个时间点间有足够操作次数（如10次）
- [ ] 记录准确的操作步骤和时间
- [ ] 确认日志采集完整，无截断

### 初步分析阶段
- [ ] 使用 memdump_quick.py --diagnose 检查日志质量
- [ ] 确认分配总数趋势（稳定/增长/下降）
- [ ] 计算每次操作的平均增长量
- [ ] 识别增长是否线性相关

### 深度分析阶段
- [ ] 按 backtrace 分组统计增长情况
- [ ] 使用 ELF 解析可疑 backtrace 的完整调用栈
- [ ] 定位到具体源码文件和行号
- [ ] 识别泄漏模式（事件处理/错误路径/循环引用/缓存增长）

### 代码检查阶段
- [ ] 检查对应的释放函数是否被调用
- [ ] 检查所有返回路径的释放逻辑
- [ ] 检查错误处理路径的清理代码
- [ ] 检查异步场景的生命周期管理

### 修复验证阶段
- [ ] 部署修复后的固件
- [ ] 使用相同的测试流程重新采集数据
- [ ] 对比修复前后的数值
- [ ] 长期运行测试（如100次、1000次操作）
- [ ] 记录修复效果和经验

---

## 性能优化建议列表

当发现内存占用过高时，按优先级考虑以下优化：

### 优先级1：修复泄漏
- **收益**：根本性解决问题
- **风险**：低（如果修复正确）
- **���间**：中等

### 优先级2：减少大分配
- 分析 Top 10 大分配的必要性
- 评估是否可以降低 buffer 大小
- 考虑按需分配而非预分配
- **收益**：立竿见影
- **风险**：中（需要测试功能）

### 优先级3：优化数据结构
- 使用更紧凑的数据类型
- 减少对齐浪费
- 共享常量数据
- **收益**：累积效果明显
- **风险**：低

### 优先级4：实现对象池
- 频繁分配释放的对象使用对象池
- 减少分配次数
- **收益**：减少碎片和开销
- **风险**：中（增加复杂度）

### 优先级5：延迟加载
- 非关键功能延迟初始化
- 按需创建而非启动创建
- **收益**：降低启动内存峰值
- **风险**：低

### 优先级6：资源复用
- 多个功能共享buffer
- 时间换空间策略
- **收益**：显著
- **风险**：高（需要careful设计）

---

## 文档和工具维护

### 保持配置文件更新
```bash
# 项目配置模板
mkdir -p .memdump
cp multi_core_config.json .memdump/project_config.json

# 版本控制
git add .memdump/
git commit -m "Add memdump analysis configuration"
```

### 分析脚本版本化
```bash
# 记录AI生成的分析脚本
mkdir -p scripts/memory_analysis
mv analyze_*.py scripts/memory_analysis/
git add scripts/memory_analysis/
```

### 建立知识库
```markdown
# 项目内存分析知识库

## 已知问题
1. mediad启动分配60KB线程栈 - 正常
2. audio_dsp缓存增长至200KB - 需要优化

## 优化历史
- 2024-01-15: 修复av_frame_alloc泄漏，节省45KB
- 2024-01-20: 优化buffer分配策略，节省30KB

## 分析经验
- mediad的大分配主要来自FFmpeg
- 使用memdump_quick.py可快速定位热点
```
