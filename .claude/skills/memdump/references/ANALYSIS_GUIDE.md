# 分析流程详细指南

本文档提供完整的memdump分析流程，包括基础分析、差异对比、内存泄漏调试等场景。

完整内容请参考旧版SKILL.md（SKILL.md.bak）的第352-773行，包含：

## 主要章节

### 场景一：基础Heap分析
- 单核系统分析流程
- 多核系统分析流程  
- 统计汇总方法
- Backtrace解析技巧

### 场景二：事件前后差异对比
- 基于序列号的精确对比
- 识别被释放和新增的分配
- 差异报告解读

### 场景三：内存泄漏调试（重点）
- 采样策略设计
- 多时间点对比分析
- **Backtrace分组检测**（关键方法）
- 泄漏模式识别：
  - 事件处理泄漏
  - 错误路径泄漏
  - 循环引用泄漏
  - 缓存无限增长
- 源码定位和修复方法
- 验证修复效果

## 快速参考

### 泄漏检测Backtrace分组方法（核心）

```python
# 按前3层backtrace分组
def group_by_backtrace(allocations):
    groups = {}
    for alloc in allocations:
        bt_key = tuple(alloc['backtrace'][:3])
        if bt_key not in groups:
            groups[bt_key] = []
        groups[bt_key].append(alloc)
    return groups

# 找出增长的backtrace
baseline_groups = groupby_backtrace(baseline)
final_groups = group_by_backtrace(final)

for bt_key in final_groups:
    baseline_count = len(baseline_groups.get(bt_key, []))
    final_count = len(final_groups[bt_key])
    
    if final_count > baseline_count:
        growth = final_count - baseline_count
        print(f"⚠️ 泄漏嫌疑: {bt_key}")
        print(f"   增长: {growth}个分配")
```

这是检测泄漏的**最关键方法**，通过调用栈分组能快速定位持续增长的分配点。

## 使用建议

对于复杂的分析场景，建议：
1. 参考[PROMPTS.md](PROMPTS.md)获取合适的提示词模板
2. 使用[SCRIPTS.md](SCRIPTS.md)中的工具自动化分析
3. 遵循[BEST_PRACTICES.md](BEST_PRACTICES.md)的最佳实践

---

**注**: 完整的分析流程、代码示例和案例研究保存在SKILL.md.bak中，可根据需要查阅。
