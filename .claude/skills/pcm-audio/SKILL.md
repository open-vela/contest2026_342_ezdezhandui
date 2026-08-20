---
name: pcm-audio
description: Analyzes PCM audio file quality issues including clipping, silence insertion, clicks, noise floor, and periodic distortion. Use when debugging audio capture/playback problems, investigating audio glitches, or identifying audio processing bugs in embedded systems.
metadata:
  version: "1.0"
  audio-formats: "PCM (8-48kHz, 16-bit)"
---

# PCM Audio Quality Analysis Skill

## 概述
分析PCM音频文件的质量问题，包括削波失真、静音插入、爆破音、底噪、周期性失真等，并将问题特征与可能的代码错误对应起来。

## 适用场景
- 音频采集/播放出现杂音、噪音、爆音
- 音频流中出现静音段、断断续续
- 音频质量下降、失真
- 需要定位音频处理代码的bug

## 支持的音频格式
- **PCM格式**: RAW PCM、.pcm文件
- **采样率**: 8kHz - 48kHz
- **位宽**: 16-bit (int16)
- **声道**: 单声道/立体声/多声道

---

## 🚀 使用方法

### 命令行调用

\`\`\`bash
# 分析单声道16kHz PCM文件
python scripts/audio_analyzer.py input.pcm --sample-rate 16000 --channels 1

# 分析立体声48kHz文件
python scripts/audio_analyzer.py stereo.pcm --sample-rate 48000 --channels 2

# 不生成图表（加快速度）
python scripts/audio_analyzer.py input.pcm --no-visualize
\`\`\`

### Python API调用

\`\`\`python
import sys
sys.path.append('scripts')
from audio_analyzer import AudioQualityAnalyzer

# 创建分析器（注意：根据实际文件调整参数）
analyzer = AudioQualityAnalyzer(
    pcm_file='input.pcm',
    sample_rate=16000,
    channels=1  # 1=单声道, 2=立体声
)

# 完整分析
results = analyzer.analyze_all()

# 单项检测
clipping = analyzer.detect_clipping()
silence = analyzer.detect_silence_insertion()
clicks = analyzer.detect_clicks()

# 自定义阈值
analyzer.detect_clipping(threshold=0.95)  # 更严格
analyzer.detect_clicks(threshold=3000)  # 更敏感

# 生成可视化
analyzer.visualize('output.png')
\`\`\`

### 参数说明

| 参数 | 说明 | 默认值 | 示例 |
|-----|------|--------|------|
| `pcm_file` | PCM文件路径 | 必填 | `input.pcm` |
| `--sample-rate` | 采样率(Hz) | 16000 | `48000` |
| `--channels` | 声道数 | 2 | `1`, `8` |
| `--sample-width` | 样本宽度(字节) | 2 | `2` |
| `--output` | 输出图表文件 | `audio_analysis.png` | `result.png` |
| `--no-visualize` | 不生成图表 | False | - |

---

## 🔍 核心检测能力

### 1. 削波失真检测

**调用**: \`analyzer.detect_clipping(threshold=0.98)\`

**返回值**: \`{'clipped_pct': float, 'clipped_samples': int, 'max_consecutive': int, 'is_severe': bool}\`

**问题映射**:
- 削波率 > 1% → 音量增益过大、整数溢出、格式转换错误

### 2. 静音插入检测

**调用**: \`analyzer.detect_silence_insertion(window_size=1600)\`

**返回值**: \`[{'start_time_s': float, 'end_time_s': float, 'duration_ms': float, 'zero_pct': float}]\`

**问题映射**:
- 零值 > 50% → Buffer大小错误、未初始化、采样率转换错误

### 3. 爆破音检测

**调用**: \`analyzer.detect_clicks(threshold=5000)\`

**返回值**: \`[{'position': int, 'time_s': float, 'amplitude_jump': int}]\`

**问题映射**:
- 跳变 > 10000 → Buffer切换不平滑、时钟不同步、DMA错误

### 4. 底噪分析

**调用**: \`analyzer.detect_noise_floor(silence_threshold=100)\`

**返回值**: \`{'noise_rms': float, 'snr_db': float, 'is_white_noise': bool}\`

**问题映射**:
- SNR < 40dB → 硬件增益过高、ADC位宽不足、未初始化变量

### 5. 周期性失真检测

**调用**: \`analyzer.detect_periodic_distortion(max_period=48000)\`

**返回值**: \`{'period_ms': float, 'frequency_hz': float, 'possible_source': str}\`

**问题映射**:
- 50/60Hz → 市电干扰
- <25ms → 定时器中断冲突

---

## � 输出示例

工具会生成结构化的分析报告：

```
📁 加载PCM文件: test.pcm
   采样率: 16000 Hz
   声道数: 2
   总样本: 256,000
   时长: 8.00 秒

======================================================================
                         音频质量综合分析报告
======================================================================

【1】削波失真检测
──────────────────────────────────────────────────────────────────────
  ✅ 削波样本: 0.12% (正常)

【2】静音插入检测
──────────────────────────────────────────────────────────────────────
  ❌ 静音段1: 4.02s - 5.15s, 时长1130.0ms, 零值67.5%
  💡 建议: 检查采样率转换时buffer大小计算是否正确

【3】爆破音/咔哒声检测
──────────────────────────────────────────────────────────────────────
  ✅ 爆破音数量: 3 (可接受)

【4】底噪分析
──────────────────────────────────────────────────────────────────────
  ✅ 信噪比: 45.2 dB (良好)
     底噪RMS: 18.52
     噪声类型: 白噪声

【5】周期性失真检测
──────────────────────────────────────────────────────────────────────
  ✅ 未检测到周期性失真

======================================================================
                             分析总结
======================================================================

❌ 发现 1 个严重问题:

  1. 检测到 1 个静音段，总时长 1130.0ms
     💡 检查buffer大小计算、内存初始化、采样率转换

======================================================================
```

## 🎨 可视化输出

分析会自动生成包含以下图表的 PNG 文件：

1. **全局波形图** - 显示完整音频波形
2. **零值比例时间序列** - 显示静音问题的时间分布
3. **RMS能量时间序列** - 显示能量变化
4. **频谱图** - 显示频率分布
5. **幅度分布直方图** - 显示削波情况

---

## �📋 代码错误快速映射表

| 问题类型 | 典型特征 | 立即检查 |
|---------|---------|---------|
| **削波失真** | 样本=±32767, >1% | 增益计算、混音算法 |
| **静音插入** | 零值>50%, 集中时段 | buffer大小、采样率转换 |
| **爆破音** | 突变>10000, 分散 | buffer切换、并发访问 |
| **底噪高** | SNR<40dB, 持续 | 硬件增益、ADC配置 |
| **周期失真** | 固定周期, 规律 | 定时器、DMA刷新 |

---

## 💡 典型调试案例

### 案例1: 采样率转换静音问题

**问题现象**:
```
静音段: 4.02s - 5.15s, 时长1130.0ms, 零值67.5%
能量下降: 83.4%
```

**AI分析流程**:
1. 离线PCM分析 → 发现67.5%零值
2. 数学验证 → 640/960 = 66.67% ≈ 67.5%
3. 代码检查 → buffer分配用了输入样本数
4. 根因定位 → 采样率转换计算错误（48kHz → 16kHz）

**修复方案**:
\`\`\`c
// ❌ 错误：用输入样本数分配输出buffer
out = ff_get_audio_buffer(outlink, in->nb_samples);  // 960样本

// ✅ 正确：根据采样率比例计算
int out_samples = in->nb_samples * out_rate / in_rate;  // 320样本
out = ff_get_audio_buffer(outlink, out_samples);
\`\`\`

### 案例2: 削波失真问题

**问题现象**:
```
削波率: 5.2%
最大连续削波: 237样本
峰值样本: ±32767 (饱和)
```

**可能原因**:
- 音量增益设置过高
- 混音算法整数溢出
- 格式转换未做饱和保护

**修复方案**:
\`\`\`c
// ❌ 错误：直接相加可能溢出
int16_t mixed = sample_a + sample_b;

// ✅ 正确：使用饱和运算
int32_t result = (int32_t)sample_a + (int32_t)sample_b;
if (result > 32767) result = 32767;
if (result < -32768) result = -32768;
int16_t mixed = (int16_t)result;
\`\`\`

---

## ⚠️ 注意事项

1. **参数准确**: channels必须与实际匹配（1=单声道, 2=立体声）
2. **文件格式**: 只支持RAW PCM
3. **依赖库**: \`pip install numpy scipy matplotlib\`

---

## 📚 相关文档

- **实现代码**: [scripts/audio_analyzer.py](scripts/audio_analyzer.py)
- **测试方法**: [references/TESTING.md](references/TESTING.md) - 详细的测试文档和用例
- **测试工具**: [scripts/generate_test_audio.py](scripts/generate_test_audio.py)
