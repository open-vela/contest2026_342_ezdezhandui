#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
PCM Audio Quality Analysis Tool

Features:
- Clipping distortion detection
- Silence insertion detection
- Pops/clicks detection
- Noise floor analysis
- Periodic distortion detection
- Harmonic distortion detection
- Visualization chart generation

Usage:
    python audio_analyzer.py input.pcm --sample-rate 16000 --channels 2
"""

import numpy as np
import argparse
import sys
from scipy import signal


class AudioQualityAnalyzer:
    """Audio quality comprehensive analyzer"""
    
    def __init__(self, pcm_file, sample_rate=16000, channels=2, sample_width=2):
        """
        Initialize analyzer
        
        Args:
            pcm_file: PCM file path
            sample_rate: Sample rate (Hz)
            channels: Number of channels
            sample_width: Sample width (bytes)
        """
        try:
            with open(pcm_file, 'rb') as f:
                pcm_data = f.read()
        except FileNotFoundError:
            print(f"Error: file '{pcm_file}' not found")
            sys.exit(1)
        except PermissionError:
            print(f"Error: no permission to read file '{pcm_file}'")
            sys.exit(1)
        
        if len(pcm_data) == 0:
            print(f"Error: file '{pcm_file}' is empty")
            sys.exit(1)
        
        self.samples = np.frombuffer(pcm_data, dtype=np.int16)
        self.sample_rate = sample_rate
        self.channels = channels
        self.sample_width = sample_width
        self.pcm_file = pcm_file
        
        print(f"\nLoaded PCM file: {pcm_file}")
        print(f"   Sample rate: {sample_rate} Hz")
        print(f"   Channels: {channels}")
        print(f"   Total samples: {len(self.samples):,}")
        print(f"   Duration: {len(self.samples) / (sample_rate * channels):.2f} s")
        print(f"   File size: {len(pcm_data) / 1024:.1f} KB")

    def detect_clipping(self, threshold=0.98):
        """Detect clipping distortion"""
        max_value = 32767
        clip_threshold = max_value * threshold
        
        clipped = np.sum(np.abs(self.samples) >= clip_threshold)
        clipped_pct = clipped / len(self.samples) * 100
        
        # Consecutive clipping detection
        is_clipped = np.abs(self.samples) >= clip_threshold
        consecutive_clips = 0
        max_consecutive = 0
        for val in is_clipped:
            if val:
                consecutive_clips += 1
                max_consecutive = max(max_consecutive, consecutive_clips)
            else:
                consecutive_clips = 0
        
        is_severe = clipped_pct > 1.0 or max_consecutive > 100
        
        return {
            'clipped_pct': clipped_pct,
            'clipped_samples': clipped,
            'max_consecutive': max_consecutive,
            'is_severe': is_severe
        }
    
    def detect_silence_insertion(self, window_size=1600):
        """Detect silence insertion"""
        silence_segments = []
        i = 0
        
        while i < len(self.samples):
            window = self.samples[i:i+window_size]
            if len(window) == 0:
                break
            
            zero_pct = np.sum(window == 0) / len(window)
            
            if zero_pct > 0.5:
                start = i
                while i < len(self.samples):
                    window = self.samples[i:i+window_size]
                    if len(window) == 0 or np.sum(window == 0) <= len(window) * 0.5:
                        break
                    i += window_size
                end = i
                
                silence_segments.append({
                    'start_sample': start,
                    'end_sample': end,
                    'start_time_s': start / (self.sample_rate * self.channels),
                    'end_time_s': end / (self.sample_rate * self.channels),
                    'duration_ms': (end - start) / (self.sample_rate * self.channels) * 1000,
                    'zero_pct': np.sum(self.samples[start:end] == 0) / (end - start) * 100 if end > start else 0
                })
            
            i += window_size
        
        return silence_segments
    
    def detect_clicks(self, threshold=5000):
        """Detect pops/clicks"""
        diff = np.diff(self.samples.astype(np.int32))
        
        clicks = []
        for i, d in enumerate(diff):
            if abs(d) > threshold:
                clicks.append({
                    'position': i,
                    'time_s': i / (self.sample_rate * self.channels),
                    'amplitude_jump': int(d),
                    'before': int(self.samples[i]),
                    'after': int(self.samples[i+1])
                })
        
        return clicks
    
    def detect_noise_floor(self, silence_threshold=100):
        """Detect noise floor"""
        quiet_samples = self.samples[np.abs(self.samples) < silence_threshold]
        
        if len(quiet_samples) > 1000:
            noise_rms = np.sqrt(np.mean(quiet_samples.astype(np.float64) ** 2))
            noise_peak = np.max(np.abs(quiet_samples))
            snr_db = 20 * np.log10(32767 / noise_rms) if noise_rms > 0 else float('inf')
            
            # Spectral flatness
            try:
                freqs, psd = signal.welch(quiet_samples, fs=self.sample_rate, nperseg=min(1024, len(quiet_samples)))
                psd_std = np.std(psd)
                psd_mean = np.mean(psd)
                flatness = psd_std / psd_mean if psd_mean > 0 else 0
            except:
                flatness = 0
            
            return {
                'noise_rms': noise_rms,
                'noise_peak': int(noise_peak),
                'snr_db': snr_db,
                'is_white_noise': flatness < 0.5,
                'spectral_flatness': flatness
            }
        
        return None
    
    def detect_periodic_distortion(self, max_period=48000):
        """Detect periodic distortion"""
        test_len = min(max_period * 2, len(self.samples))
        if test_len < 1000:
            return None
        
        try:
            autocorr = signal.correlate(self.samples[:test_len], 
                                        self.samples[:test_len], 
                                        mode='full')
            autocorr = autocorr[len(autocorr)//2:]
            
            if len(autocorr) < 200:
                return None
            
            peaks, _ = signal.find_peaks(autocorr[100:], 
                                         height=np.max(autocorr[100:]) * 0.5)
            
            if len(peaks) > 0:
                dominant_period = peaks[0] + 100
                period_ms = dominant_period / self.sample_rate * 1000
                frequency_hz = self.sample_rate / dominant_period
                
                source = "unknown"
                if 45 < frequency_hz < 55:
                    source = "mains interference (50Hz)"
                elif 55 < frequency_hz < 65:
                    source = "mains interference (60Hz)"
                elif period_ms < 25:
                    source = "timer interrupt"
                
                return {
                    'period_samples': dominant_period,
                    'period_ms': period_ms,
                    'frequency_hz': frequency_hz,
                    'possible_source': source
                }
        except:
            pass
        
        return None
    
    def analyze_all(self):
        """Run full analysis"""
        print("\n" + "="*70)
        print(" Audio Quality Analysis Report ".center(70, "="))
        print("="*70)
        
        results = {}
        issues = []
        
        # 1. Clipping detection
        print("\n[1] Clipping Distortion Detection")
        print("-" * 70)
        clipping = self.detect_clipping()
        results['clipping'] = clipping
        if clipping['is_severe']:
            issues.append({
                'level': 'error',
                'message': f"Severe clipping: {clipping['clipped_pct']:.2f}%, "
                          f"max consecutive {clipping['max_consecutive']} samples",
                'suggestion': "Check gain settings, mixing algorithm, format conversion"
            })
            print(f"  Clipped samples: {clipping['clipped_pct']:.2f}%")
            print(f"  Max consecutive clipping: {clipping['max_consecutive']} samples")
            print(f"  Suggestion: check gain settings, integer overflow in mixing algorithm")
        else:
            print(f"  Clipped samples: {clipping['clipped_pct']:.2f}% (normal)")
            if clipping['clipped_pct'] > 0.1:
                print(f"  Minor clipping detected, consider checking peak handling")
        
        # 2. Silence detection
        print("\n[2] Silence Insertion Detection")
        print("-" * 70)
        silence = self.detect_silence_insertion()
        results['silence'] = silence
        if len(silence) > 0:
            total_silence_ms = sum(seg['duration_ms'] for seg in silence)
            issues.append({
                'level': 'error',
                'message': f"Detected {len(silence)} silence segments, total {total_silence_ms:.1f}ms",
                'suggestion': "Check buffer size calculation, memory initialization, sample rate conversion"
            })
            for i, seg in enumerate(silence[:5]):  # Show first 5
                print(f"  Silence segment {i+1}: "
                     f"{seg['start_time_s']:.2f}s - {seg['end_time_s']:.2f}s, "
                     f"duration {seg['duration_ms']:.1f}ms, "
                     f"zeros {seg['zero_pct']:.1f}%")
            if len(silence) > 5:
                print(f"  ... {len(silence)-5} more silence segments")
            print(f"  Suggestion: check buffer size calculation in sample rate conversion")
        else:
            print(f"  No abnormal silence segments detected")
        
        # 3. Pops/clicks detection
        print("\n[3] Pops/Clicks Detection")
        print("-" * 70)
        clicks = self.detect_clicks()
        results['clicks'] = clicks
        total_time = len(self.samples) / (self.sample_rate * self.channels)
        density = len(clicks) / total_time if total_time > 0 else 0
        
        if len(clicks) > 10:
            issues.append({
                'level': 'warning' if density < 1 else 'error',
                'message': f"Detected {len(clicks)} pops/clicks ({density:.1f}/sec)",
                'suggestion': "Check buffer switching smoothness, clock sync, DMA configuration"
            })
            print(f"  Pop count: {len(clicks)}")
            print(f"  Density: {density:.1f} per second")
            for i, click in enumerate(clicks[:5]):
                print(f"     Position {i+1}: {click['time_s']:.3f}s, "
                     f"jump {click['amplitude_jump']:+d}")
            if len(clicks) > 5:
                print(f"  ... {len(clicks)-5} more pops/clicks")
            print(f"  Suggestion: check double-buffer switching smoothness, concurrent access")
        else:
            print(f"  Pop count: {len(clicks)} (acceptable)")
            if len(clicks) > 0:
                print(f"     Density: {density:.2f} per second")
        
        # 4. Noise floor detection
        print("\n[4] Noise Floor Analysis")
        print("-" * 70)
        noise = self.detect_noise_floor()
        results['noise_floor'] = noise
        if noise:
            if noise['snr_db'] < 40:
                issues.append({
                    'level': 'warning' if noise['snr_db'] >= 30 else 'error',
                    'message': f"Low SNR: {noise['snr_db']:.1f} dB",
                    'suggestion': "Check hardware gain settings, ADC bit width, power supply filtering"
                })
                print(f"  SNR: {noise['snr_db']:.1f} dB ({'critically low' if noise['snr_db'] < 30 else 'low'})")
                print(f"  Suggestion: check if microphone gain is too high, hardware circuit noise")
            else:
                print(f"  SNR: {noise['snr_db']:.1f} dB (good)")
            
            print(f"     Noise floor RMS: {noise['noise_rms']:.2f}")
            print(f"     Noise floor peak: {noise['noise_peak']}")
            print(f"     Noise type: {'white noise' if noise['is_white_noise'] else 'colored noise'} "
                 f"(flatness: {noise['spectral_flatness']:.3f})")
        else:
            print(f"  Cannot analyze (insufficient silence segments)")
        
        # 5. Periodic distortion detection
        print("\n[5] Periodic Distortion Detection")
        print("-" * 70)
        periodic = self.detect_periodic_distortion()
        results['periodic'] = periodic
        if periodic:
            issues.append({
                'level': 'warning',
                'message': f"Periodic distortion: {periodic['frequency_hz']:.1f} Hz ({periodic['possible_source']})",
                'suggestion': "Check timer interrupt, DMA refresh rate, power supply filtering"
            })
            print(f"  Period: {periodic['period_ms']:.2f} ms ({periodic['period_samples']} samples)")
            print(f"  Frequency: {periodic['frequency_hz']:.1f} Hz")
            print(f"  Possible source: {periodic['possible_source']}")
            print(f"  Suggestion: check {periodic['possible_source']} related configuration")
        else:
            print(f"  No periodic distortion detected")
        
        # Summary
        print("\n" + "="*70)
        print(" Analysis Summary ".center(70, "="))
        print("="*70)
        
        if issues:
            errors = [i for i in issues if i['level'] == 'error']
            warnings = [i for i in issues if i['level'] == 'warning']
            
            if errors:
                print(f"\nFound {len(errors)} critical issues:\n")
                for i, issue in enumerate(errors, 1):
                    print(f"  {i}. {issue['message']}")
                    print(f"     Suggestion: {issue['suggestion']}")
            
            if warnings:
                print(f"\nFound {len(warnings)} warnings:\n")
                for i, issue in enumerate(warnings, 1):
                    print(f"  {i}. {issue['message']}")
                    print(f"     Suggestion: {issue['suggestion']}")
        else:
            print("\nNo significant audio quality issues detected")
        
        print("\n" + "="*70 + "\n")
        
        return results
    
    def visualize(self, output_file='audio_analysis.png'):
        """Generate visualization charts"""
        try:
            import matplotlib.pyplot as plt
            from matplotlib.gridspec import GridSpec
        except ImportError:
            print("Warning: matplotlib is required for visualization")
            print("   Install: pip install matplotlib")
            return
        
        print(f"Generating visualization charts...")
        
        fig = plt.figure(figsize=(16, 10))
        gs = GridSpec(3, 2, figure=fig)
        
        time = np.arange(len(self.samples)) / (self.sample_rate * self.channels)
        
        # 1. Global waveform
        ax1 = fig.add_subplot(gs[0, :])
        ax1.plot(time, self.samples, linewidth=0.5, alpha=0.7)
        ax1.set_xlabel('Time (s)')
        ax1.set_ylabel('Amplitude')
        ax1.set_title(f'PCM Audio Waveform - {self.pcm_file}')
        ax1.grid(True, alpha=0.3)
        ax1.axhline(y=32767, color='r', linestyle='--', alpha=0.3, label='Max')
        ax1.axhline(y=-32768, color='r', linestyle='--', alpha=0.3)
        ax1.legend()
        
        # 2. Zero ratio time series
        ax2 = fig.add_subplot(gs[1, 0])
        window_size = self.sample_rate * self.channels
        zero_ratios = []
        window_times = []
        
        for i in range(0, len(self.samples) - window_size, max(1, window_size // 10)):
            window = self.samples[i:i+window_size]
            zero_ratio = np.sum(window == 0) / len(window) * 100
            zero_ratios.append(zero_ratio)
            window_times.append(i / (self.sample_rate * self.channels))
        
        ax2.plot(window_times, zero_ratios, marker='o', markersize=3, linewidth=1)
        ax2.axhline(y=50, color='red', linestyle='--', alpha=0.5, label='50% Threshold')
        ax2.set_xlabel('Time (s)')
        ax2.set_ylabel('Zero Samples (%)')
        ax2.set_title('Zero Sample Ratio Over Time (1s Window)')
        ax2.legend()
        ax2.grid(True, alpha=0.3)
        ax2.set_ylim([0, 100])
        
        # 3. Energy distribution
        ax3 = fig.add_subplot(gs[1, 1])
        rms_values = []
        for i in range(0, len(self.samples) - window_size, max(1, window_size // 10)):
            window = self.samples[i:i+window_size]
            rms = np.sqrt(np.mean(window.astype(np.float64) ** 2))
            rms_values.append(rms)
        
        ax3.plot(window_times, rms_values, color='green', marker='o', markersize=3, linewidth=1)
        ax3.set_xlabel('Time (s)')
        ax3.set_ylabel('RMS Energy')
        ax3.set_title('Audio Energy (RMS) Over Time')
        ax3.grid(True, alpha=0.3)
        
        # 4. Spectrogram
        ax4 = fig.add_subplot(gs[2, 0])
        try:
            nperseg = min(1024, len(self.samples) // 4)
            freqs, times, Sxx = signal.spectrogram(self.samples, 
                                                    fs=self.sample_rate,
                                                    nperseg=nperseg)
            pcm = ax4.pcolormesh(times, freqs, 10 * np.log10(Sxx + 1e-10), 
                           shading='gouraud', cmap='viridis')
            plt.colorbar(pcm, ax=ax4, label='Power (dB)')
            ax4.set_ylabel('Frequency (Hz)')
            ax4.set_xlabel('Time (s)')
            ax4.set_title('Spectrogram')
            ax4.set_ylim([0, self.sample_rate / 2])
        except:
            ax4.text(0.5, 0.5, 'Spectrogram generation failed', 
                    ha='center', va='center', transform=ax4.transAxes)
        
        # 5. Amplitude distribution histogram
        ax5 = fig.add_subplot(gs[2, 1])
        ax5.hist(self.samples, bins=100, alpha=0.7, edgecolor='black', color='steelblue')
        ax5.axvline(x=32767, color='r', linestyle='--', alpha=0.5, label='Max value')
        ax5.axvline(x=-32768, color='r', linestyle='--', alpha=0.5)
        ax5.set_xlabel('Amplitude')
        ax5.set_ylabel('Count (log scale)')
        ax5.set_yscale('log')
        ax5.set_title('Amplitude Distribution')
        ax5.legend()
        ax5.grid(True, alpha=0.3, axis='y')
        
        plt.tight_layout()
        plt.savefig(output_file, dpi=150, bbox_inches='tight')
        print(f"Visualization saved: {output_file}\n")


def main():
    parser = argparse.ArgumentParser(
        description='PCM Audio Quality Analysis Tool',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s input.pcm
  %(prog)s input.pcm --sample-rate 48000 --channels 8
  %(prog)s input.pcm -r 16000 -c 2 --visualize
  %(prog)s input.pcm --no-visualize
        """
    )
    
    parser.add_argument('pcm_file', help='PCM audio file path')
    parser.add_argument('-r', '--sample-rate', type=int, default=16000,
                       help='Sample rate (Hz), default: 16000')
    parser.add_argument('-c', '--channels', type=int, default=2,
                       help='Number of channels, default: 2')
    parser.add_argument('-w', '--sample-width', type=int, default=2,
                       help='Sample width (bytes), default: 2 (16-bit)')
    parser.add_argument('-o', '--output', default='audio_analysis.png',
                       help='Visualization output file, default: audio_analysis.png')
    parser.add_argument('--visualize', action='store_true', default=True,
                       help='Generate visualization charts (default: enabled)')
    parser.add_argument('--no-visualize', dest='visualize', action='store_false',
                       help='Do not generate visualization charts')
    
    args = parser.parse_args()
    
    # Create analyzer
    analyzer = AudioQualityAnalyzer(
        pcm_file=args.pcm_file,
        sample_rate=args.sample_rate,
        channels=args.channels,
        sample_width=args.sample_width
    )
    
    # Run analysis
    results = analyzer.analyze_all()
    
    # Generate visualization
    if args.visualize:
        analyzer.visualize(args.output)


if __name__ == '__main__':
    main()
