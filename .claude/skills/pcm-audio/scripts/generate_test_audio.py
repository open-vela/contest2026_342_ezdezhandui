#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Generate test PCM audio files with various audio quality issues
for verifying audio analysis tool detection capabilities
"""

import numpy as np
import os

SAMPLE_RATE = 16000
DURATION = 5  # seconds

def generate_sine_wave(freq=440, duration=DURATION, amplitude=0.5):
    """Generate a sine wave"""
    t = np.linspace(0, duration, int(SAMPLE_RATE * duration), False)
    wave = amplitude * np.sin(2 * np.pi * freq * t)
    return (wave * 32767).astype(np.int16)

def save_pcm(samples, filename):
    """Save as PCM file"""
    samples.tofile(filename)
    print(f"  Generated: {filename} ({len(samples)} samples, {len(samples)/SAMPLE_RATE:.2f}s)")

def generate_test_files(output_dir):
    """Generate all test files"""
    os.makedirs(output_dir, exist_ok=True)
    print(f"\nOutput directory: {output_dir}\n")
    
    # 1. Normal audio (baseline)
    print("[1] Generating normal audio (baseline)")
    normal = generate_sine_wave(440, DURATION, 0.5)
    save_pcm(normal, f"{output_dir}/01_normal.pcm")
    
    # 2. Clipping distortion - excessive gain causing saturation
    print("\n[2] Generating clipping distortion audio")
    clipped = generate_sine_wave(440, DURATION, 1.5)  # Amplitude too high
    clipped = np.clip(clipped, -32767, 32767).astype(np.int16)
    save_pcm(clipped, f"{output_dir}/02_clipping.pcm")
    
    # 3. Silence insertion - zero-value segments in the middle
    print("\n[3] Generating silence insertion audio")
    silence_insert = generate_sine_wave(440, DURATION, 0.5).copy()
    # Insert silence at 1.5s-2.5s
    start_idx = int(1.5 * SAMPLE_RATE)
    end_idx = int(2.5 * SAMPLE_RATE)
    silence_insert[start_idx:end_idx] = 0
    save_pcm(silence_insert, f"{output_dir}/03_silence_insert.pcm")
    
    # 4. Pops/clicks - sudden level jumps
    print("\n[4] Generating pops/clicks audio")
    pops = generate_sine_wave(440, DURATION, 0.3).copy()
    pop_positions = [0.5, 1.2, 2.0, 2.8, 3.5, 4.2]  # Pop positions (seconds)
    for pos in pop_positions:
        idx = int(pos * SAMPLE_RATE)
        pops[idx:idx+5] = np.array([20000, -25000, 30000, -20000, 15000], dtype=np.int16)
    save_pcm(pops, f"{output_dir}/04_pops_clicks.pcm")
    
    # 5. High noise floor - overlaid noise
    print("\n[5] Generating high noise floor audio")
    noisy = generate_sine_wave(440, DURATION, 0.3)
    noise = (np.random.randn(len(noisy)) * 3000).astype(np.int16)  # Strong noise
    noisy = np.clip(noisy.astype(np.int32) + noise.astype(np.int32), -32767, 32767).astype(np.int16)
    save_pcm(noisy, f"{output_dir}/05_high_noise.pcm")
    
    # 6. Periodic distortion - timer interrupt simulation
    print("\n[6] Generating periodic distortion audio (timer interrupt simulation)")
    periodic = generate_sine_wave(440, DURATION, 0.5).copy()
    period_samples = 180  # 11.25ms period @16kHz
    for i in range(0, len(periodic), period_samples):
        if i < len(periodic):
            periodic[i] = np.clip(periodic[i] + 5000, -32767, 32767)
    save_pcm(periodic, f"{output_dir}/06_periodic_distortion.pcm")
    
    # 7. Multiple silence gaps - buffer underrun simulation
    print("\n[7] Generating buffer underrun audio (multiple silence gaps)")
    underrun = generate_sine_wave(440, DURATION, 0.5).copy()
    # 0.1s silence every 0.5s
    for start in np.arange(0.5, DURATION, 0.5):
        s_idx = int(start * SAMPLE_RATE)
        e_idx = int((start + 0.1) * SAMPLE_RATE)
        if e_idx < len(underrun):
            underrun[s_idx:e_idx] = 0
    save_pcm(underrun, f"{output_dir}/07_buffer_underrun.pcm")
    
    # 8. Combined issues - multiple problems overlaid
    print("\n[8] Generating combined issues audio")
    combined = generate_sine_wave(440, DURATION, 0.8).copy()
    # Add clipping
    combined = np.clip(combined, -25000, 25000).astype(np.int16)
    # Add silence segment
    combined[int(2*SAMPLE_RATE):int(2.3*SAMPLE_RATE)] = 0
    # Add pop
    combined[int(1*SAMPLE_RATE):int(1*SAMPLE_RATE)+3] = np.array([30000, -30000, 25000], dtype=np.int16)
    # Add noise
    noise = (np.random.randn(len(combined)) * 500).astype(np.int16)
    combined = np.clip(combined.astype(np.int32) + noise.astype(np.int32), -32767, 32767).astype(np.int16)
    save_pcm(combined, f"{output_dir}/08_combined_issues.pcm")
    
    # 9. DC offset
    print("\n[9] Generating DC offset audio")
    dc_offset = generate_sine_wave(440, DURATION, 0.4).copy()
    dc_offset = np.clip(dc_offset.astype(np.int32) + 8000, -32767, 32767).astype(np.int16)
    save_pcm(dc_offset, f"{output_dir}/09_dc_offset.pcm")
    
    # 10. Harmonic distortion - square wave
    print("\n[10] Generating harmonic distortion audio (square wave)")
    t = np.linspace(0, DURATION, int(SAMPLE_RATE * DURATION), False)
    square = np.sign(np.sin(2 * np.pi * 440 * t)) * 0.5
    square = (square * 32767).astype(np.int16)
    save_pcm(square, f"{output_dir}/10_harmonic_distortion.pcm")
    
    print(f"\nGenerated 10 test audio files")
    print(f"Location: {output_dir}/")
    return output_dir

if __name__ == "__main__":
    output_dir = "/home/mi/.copilot/skills/pcm_audio/test_samples"
    generate_test_files(output_dir)
