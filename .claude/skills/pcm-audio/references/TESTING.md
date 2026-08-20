# PCM Audio Testing Guide

本文档提供详细的测试方法和测试用例，供 AI 在需要进行深度测试时参考。

## 测试文件生成

使用 [scripts/generate_test_audio.py](../scripts/generate_test_audio.py) 脚本生成包含各种音频问题的测试文件：

```bash
python scripts/generate_test_audio.py
```

该脚本会在 `test_samples/` 目录下生成10个测试PCM文件，每个文件模拟特定的音频质量问题。

---

## 测试文件清单

| 文件名 | 问题类型 | 说明 | 预期检测结果 |
|--------|---------|------|-------------|
| `01_normal.pcm` | 无问题 | 正常440Hz正弦波，作为基准对照 | ✅ 全部通过 |
| `02_clipping.pcm` | 削波失真 | 振幅1.5x导致约2%削波 | ❌ 削波 ~2% |
| `03_silence_insert.pcm` | 静音插入 | 1.5s-2.5s区间插入1秒静音 | ❌ 静音段 1000ms |
| `04_pops_clicks.pcm` | 爆破音 | 6个位置有突变爆破音 | ❌ 36个爆破音 |
| `05_high_noise.pcm` | 高底噪 | 叠加强噪声(RMS 3000) | ⚠️ 高密度异常 |
| `06_periodic_distortion.pcm` | 周期性失真 | 每180样本(11.25ms)有脉冲 | ⚠️ 周期性失真 + 爆破音 |
| `07_buffer_underrun.pcm` | Buffer欠载 | 每0.5秒有0.1秒静音段 | ❌ 9个静音段, 共900ms |
| `08_combined_issues.pcm` | 综合问题 | 削波+静音+爆破音+噪声 | ❌ 多种问题叠加 |
| `09_dc_offset.pcm` | DC偏移 | +8000 DC分量 | ℹ️ DC偏移 |
| `10_harmonic_distortion.pcm` | 谐波失真 | 方波信号(高THD) | ⚠️ 高谐波失真 |

---

## 基础测试命令

### 单个文件测试

```bash
cd test_samples

# 测试正常音频（应无问题）
python ../scripts/audio_analyzer.py 01_normal.pcm --sample-rate 16000 --channels 1

# 测试削波检测
python ../scripts/audio_analyzer.py 02_clipping.pcm --sample-rate 16000 --channels 1
# 预期：削波率 ~2%

# 测试静音检测
python ../scripts/audio_analyzer.py 03_silence_insert.pcm --sample-rate 16000 --channels 1
# 预期：检测到1.5s-2.5s静音段

# 测试爆破音检测
python ../scripts/audio_analyzer.py 04_pops_clicks.pcm --sample-rate 16000 --channels 1
# 预期：检测到6个爆破音位置

# 测试Buffer欠载
python ../scripts/audio_analyzer.py 07_buffer_underrun.pcm --sample-rate 16000 --channels 1
# 预期：检测到9个静音段
```

### 批量测试

批量分析所有测试文件：

```bash
for f in test_samples/*.pcm; do
    echo "========== $(basename $f) =========="
    python scripts/audio_analyzer.py "$f" --sample-rate 16000 --channels 1 --no-visualize 2>/dev/null | grep -E "(✅|❌|⚠️)"
done
```

---

## 详细测试用例

### 测试1: 正常音频基准

**文件**: `01_normal.pcm`

**特征**:
- 440Hz 正弦波
- 幅度 0.8 (未饱和)
- 无噪声、无失真

**预期结果**:
```
✅ 削波样本: 0.0% (正常)
✅ 未检测到静音段
✅ 爆破音数量: 0 (优秀)
✅ 信噪比: > 80 dB (优秀)
✅ 未检测到周期性失真
```

---

### 测试2: 削波失真

**文件**: `02_clipping.pcm`

**特征**:
- 440Hz 正弦波 × 1.5 倍幅度
- 峰值超过 ±32767

**预期结果**:
```
❌ 削波样本: ~2.0% (严重)
   最大连续削波: > 50 样本
```

**调试提示**:
- 检查增益计算
- 验证混音算法
- 确认格式转换是否有饱和保护

---

### 测试3: 静音插入

**文件**: `03_silence_insert.pcm`

**特征**:
- 正常音频 + 1秒零值静音 (1.5s - 2.5s)

**预期结果**:
```
❌ 静音段1: 1.5s - 2.5s, 时长1000.0ms, 零值100%
   💡 检查buffer大小计算、内存初始化
```

**调试提示**:
- 检查 buffer 分配大小
- 验证采样率转换计算
- 确认内存是否正确初始化

---

### 测试4: 爆破音/咔哒声

**文件**: `04_pops_clicks.pcm`

**特征**:
- 6个位置插入 ±20000 突变

**预期结果**:
```
❌ 检测到 6-36 个爆破音
   位置: 0.5s, 1.0s, 1.5s, 2.0s, 2.5s, 3.0s
   幅度跳变: > 15000
```

**调试提示**:
- 检查 buffer 切换是否平滑
- 验证多线程并发访问
- 确认 DMA 配置

---

### 测试5: 高底噪

**文件**: `05_high_noise.pcm`

**特征**:
- 叠加 RMS=3000 的高强度噪声

**预期结果**:
```
⚠️ 信噪比: < 30 dB (差)
   底噪RMS: > 2500
```

**调试提示**:
- 检查硬件增益设置
- 验证 ADC 配置
- 确认未初始化变量

---

### 测试6: 周期性失真

**文件**: `06_periodic_distortion.pcm`

**特征**:
- 每 180 样本 (11.25ms @ 16kHz) 插入脉冲

**预期结果**:
```
⚠️ 检测到周期性失真
   周期: ~11.25ms
   频率: ~88.9Hz
   可能来源: 定时器冲突
```

**调试提示**:
- 检查定时器配置
- 验证 DMA 刷新周期
- 确认中断优先级

---

### 测试7: Buffer 欠载

**文件**: `07_buffer_underrun.pcm`

**特征**:
- 每 0.5 秒有 0.1 秒静音段
- 模拟 buffer underrun 场景

**预期结果**:
```
❌ 检测到 9 个静音段
   总时长: ~900ms
   分布: 规律间隔
```

**调试提示**:
- 检查 buffer 大小配置
- 验证数据生产/消费速率
- 确认线程调度

---

### 测试8: 综合问题

**文件**: `08_combined_issues.pcm`

**特征**:
- 削波 + 静音 + 爆破音 + 噪声

**预期结果**:
```
❌ 削波样本: > 1%
❌ 静音段: 1+ 个
❌ 爆破音: 多个
⚠️ 信噪比: < 40 dB
```

**用途**: 压力测试，验证工具能同时检测多种问题

---

### 测试9: DC偏移

**文件**: `09_dc_offset.pcm`

**特征**:
- 信号整体偏移 +8000

**预期结果**:
```
ℹ️ 检测到 DC 偏移
   偏移量: ~8000
```

**调试提示**:
- 检查 ADC 偏移校准
- 验证信号处理链路

---

### 测试10: 谐波失真

**文件**: `10_harmonic_distortion.pcm`

**特征**:
- 方波信号（含丰富谐波）

**预期结果**:
```
⚠️ THD (总谐波失真) > 10%
```

**调试提示**:
- 检查非线性处理
- 验证饱和逻辑

---

## 自定义测试用例

### 添加新测试文件

编辑 [scripts/generate_test_audio.py](../scripts/generate_test_audio.py)：

```python
import numpy as np

def generate_sine_wave(freq=440, duration=5, sample_rate=16000, amplitude=0.8):
    """生成正弦波"""
    t = np.linspace(0, duration, int(sample_rate * duration))
    samples = amplitude * 32767 * np.sin(2 * np.pi * freq * t)
    return samples.astype(np.int16)

# 生成基础正弦波
samples = generate_sine_wave(freq=440, duration=5, amplitude=0.5)

# 添加问题特征
# ... 修改 samples ...

# 保存文件
def save_pcm(samples, filename):
    with open(filename, 'wb') as f:
        f.write(samples.tobytes())

save_pcm(samples, "test_samples/11_custom_test.pcm")
```

### 常见问题模拟

**模拟削波**:
```python
samples = samples * 1.5  # 增大幅度导致饱和
samples = np.clip(samples, -32768, 32767)
```

**模拟静音**:
```python
start_idx = int(1.5 * sample_rate)
end_idx = int(2.5 * sample_rate)
samples[start_idx:end_idx] = 0
```

**模拟爆破音**:
```python
click_positions = [8000, 16000, 24000]
for pos in click_positions:
    samples[pos] = 20000  # 突然跳变
```

**模拟噪声**:
```python
noise = np.random.normal(0, 3000, len(samples))
samples = samples + noise.astype(np.int16)
```

---

## 验证测试结果

### 人工验证

对于关键测试，建议人工验证：

1. 播放音频文件（需转换为 WAV）
2. 使用音频编辑软件查看波形
3. 对比分析工具输出与实际听感

### 转换为 WAV 格式

```bash
# 使用 ffmpeg 转换（用于人工听验证）
ffmpeg -f s16le -ar 16000 -ac 1 -i test.pcm test.wav
```

### 自动化验证

编写验证脚本：

```python
import subprocess
import json

def run_test(pcm_file, expected_issues):
    """运行测试并验证结果"""
    result = subprocess.run(
        ['python', 'scripts/audio_analyzer.py', pcm_file, 
         '--sample-rate', '16000', '--channels', '1'],
        capture_output=True, text=True
    )
    
    # 解析输出
    output = result.stdout
    
    # 验证预期问题
    for issue in expected_issues:
        assert issue in output, f"未检测到预期问题: {issue}"
    
    print(f"✅ {pcm_file} 测试通过")

# 运行测试
run_test('test_samples/02_clipping.pcm', ['削波', '❌'])
run_test('test_samples/03_silence_insert.pcm', ['静音段', '1000'])
```

---

## 故障排除

### 问题: 测试文件生成失败

**原因**: 缺少依赖库

**解决**:
```bash
pip install numpy scipy
```

### 问题: 检测结果与预期不符

**原因**: 采样率或声道数不匹配

**解决**: 确认测试命令中的参数：
```bash
python scripts/audio_analyzer.py test.pcm --sample-rate 16000 --channels 1
```

### 问题: 批量测试脚本无输出

**原因**: `grep` 过滤掉了所有输出

**解决**: 移除 `grep` 或调整过滤条件：
```bash
python scripts/audio_analyzer.py "$f" --sample-rate 16000 --channels 1 --no-visualize
```

---

## 性能测试

### 测试大文件处理

生成大文件（60秒）:
```python
samples = generate_sine_wave(duration=60)  # 60秒
save_pcm(samples, "test_samples/large_60s.pcm")
```

测试分析时间:
```bash
time python scripts/audio_analyzer.py test_samples/large_60s.pcm --sample-rate 16000 --channels 1
```

### 预期性能

| 文件时长 | 分析时间 | 内存占用 |
|---------|---------|---------|
| 5秒 | < 1秒 | < 50MB |
| 30秒 | < 3秒 | < 100MB |
| 60秒 | < 5秒 | < 200MB |

---

## 持续集成 (CI)

### GitHub Actions 示例

```yaml
name: PCM Audio Tests

on: [push, pull_request]

jobs:
  test:
    runs-on: ubuntu-latest
    
    steps:
    - uses: actions/checkout@v2
    
    - name: Setup Python
      uses: actions/setup-python@v2
      with:
        python-version: '3.9'
    
    - name: Install dependencies
      run: pip install numpy scipy matplotlib
    
    - name: Generate test files
      run: python scripts/generate_test_audio.py
    
    - name: Run tests
      run: |
        for f in test_samples/*.pcm; do
          python scripts/audio_analyzer.py "$f" --sample-rate 16000 --channels 1 --no-visualize
        done
```

---

## 总结

本测试指南涵盖了：
- ✅ 10种预定义测试用例
- ✅ 批量测试方法
- ✅ 自定义测试用例生成
- ✅ 结果验证方法
- ✅ 故障排除指南

更多信息请参考 [SKILL.md](../SKILL.md)。
