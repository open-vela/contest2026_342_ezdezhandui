#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Code Size Analysis Tool - Enhanced

Features:
  - Parse size command output files
  - Collect Flash and RAM usage statistics per module
  - Generate detailed analysis reports
  - Multiple output formats (Markdown/JSON/CSV)
  - Provide optimization suggestions

Usage:
  1. Export data using the size command:
     size -t path/to/*.a > size_output.txt
  
  2. Run this script:
     python3 analyze_size_output.py size_output.txt
  
  3. Optional arguments:
     --top N          Show Top N largest objects (default 15)
     --threshold KB   Only analyze modules above the specified threshold (default 0)
     --format         Output format: markdown, json, csv
     --output FILE    Output report to file
     --fail-on-threshold KB  Return non-zero exit code if threshold exceeded

Examples:
  python3 analyze_size_output.py /tmp/size.txt --top 20 --format json

Author: AI Generated
Version: 2.0
Date: 2026-01-29
"""

from __future__ import annotations

import sys
import json
import csv
import argparse
from collections import defaultdict
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple, Any
from pathlib import Path
from io import StringIO

# Try to import common module
try:
    from codesize_utils import format_size, format_percentage
except ImportError:
    def format_size(size_bytes: int) -> str:
        """Format size for display"""
        if abs(size_bytes) < 1024:
            return f"{size_bytes} B"
        elif abs(size_bytes) < 1024 * 1024:
            return f"{size_bytes / 1024:.2f} KB"
        else:
            return f"{size_bytes / (1024 * 1024):.2f} MB"


@dataclass
class ObjectInfo:
    """Object file information"""
    name: str
    library: str
    lib_path: str
    text: int
    data: int
    bss: int
    
    @property
    def total(self) -> int:
        return self.text + self.data + self.bss
    
    @property
    def flash(self) -> int:
        return self.text + self.data
    
    @property
    def ram(self) -> int:
        return self.data + self.bss


@dataclass
class LibraryStats:
    """Library file statistics"""
    name: str
    text: int = 0
    data: int = 0
    bss: int = 0
    objects: List[ObjectInfo] = field(default_factory=list)
    
    @property
    def total(self) -> int:
        return self.text + self.data + self.bss
    
    @property
    def flash(self) -> int:
        return self.text + self.data
    
    @property
    def ram(self) -> int:
        return self.data + self.bss


@dataclass
class AnalysisResult:
    """Analysis result"""
    objects: List[ObjectInfo] = field(default_factory=list)
    libraries: Dict[str, LibraryStats] = field(default_factory=dict)
    
    @property
    def total_text(self) -> int:
        return sum(obj.text for obj in self.objects)
    
    @property
    def total_data(self) -> int:
        return sum(obj.data for obj in self.objects)
    
    @property
    def total_bss(self) -> int:
        return sum(obj.bss for obj in self.objects)
    
    @property
    def total_flash(self) -> int:
        return self.total_text + self.total_data
    
    @property
    def total_ram(self) -> int:
        return self.total_data + self.total_bss
    
    @property
    def total_size(self) -> int:
        return self.total_text + self.total_data + self.total_bss


def parse_size_file(filename: str) -> AnalysisResult:
    """Parse size command output file
    
    Args:
        filename: Path to size output file
        
    Returns:
        AnalysisResult object
    """
    result = AnalysisResult()
    lib_stats: Dict[str, LibraryStats] = {}
    
    with open(filename, 'r', encoding='utf-8', errors='ignore') as f:
        for line in f:
            line = line.strip()
            
            # Skip header and totals line
            if not line or line.startswith('text') or '(TOTALS)' in line:
                continue
            
            parts = line.split()
            if len(parts) >= 6 and '(ex ' in line:
                try:
                    text = int(parts[0])
                    data = int(parts[1])
                    bss = int(parts[2])
                    
                    # Extract object file name and library name
                    obj_info = ' '.join(parts[5:])
                    obj_match = obj_info.split('(ex ')
                    if len(obj_match) == 2:
                        obj_name = obj_match[0].strip()
                        lib_path = obj_match[1].rstrip(')')
                        lib_parts = lib_path.split('/')
                        lib_name = lib_parts[-1] if lib_parts else lib_path
                        
                        # Create object info
                        obj = ObjectInfo(
                            name=obj_name,
                            library=lib_name,
                            lib_path=lib_path,
                            text=text,
                            data=data,
                            bss=bss
                        )
                        result.objects.append(obj)
                        
                        # Update library statistics
                        if lib_name not in lib_stats:
                            lib_stats[lib_name] = LibraryStats(name=lib_name)
                        
                        lib_stats[lib_name].text += text
                        lib_stats[lib_name].data += data
                        lib_stats[lib_name].bss += bss
                        lib_stats[lib_name].objects.append(obj)
                        
                except (ValueError, IndexError):
                    continue
    
    result.libraries = lib_stats
    return result


def generate_markdown_report(result: AnalysisResult, args: argparse.Namespace) -> str:
    """Generate Markdown format report
    
    Args:
        result: Analysis result
        args: Command line arguments
        
    Returns:
        Markdown format report string
    """
    lines: List[str] = []
    
    top_n = args.top
    module_name = args.module
    threshold = args.threshold * 1024
    
    lines.append("=" * 120)
    lines.append(" " * 40 + f"{module_name} Size Analysis Report")
    lines.append("=" * 120)
    lines.append("")
    
    # Overview
    lines.append("## Overview")
    lines.append("")
    lines.append("```")
    
    total_flash = result.total_flash
    total_ram = result.total_ram
    total_size = result.total_size
    
    text_pct = result.total_text / total_flash * 100 if total_flash > 0 else 0
    data_pct = result.total_data / total_flash * 100 if total_flash > 0 else 0
    
    lines.append("Flash Usage (code and data burned to Flash)")
    lines.append(f"  text (code):               {format_size(result.total_text):>12}  ({text_pct:.1f}%)")
    lines.append(f"  data (initialized data):   {format_size(result.total_data):>12}  ({data_pct:.1f}%)")
    lines.append(f"  Flash Total:               {format_size(total_flash):>12}")
    lines.append("")
    
    if total_ram > 0:
        data_ram_pct = result.total_data / total_ram * 100
        bss_pct = result.total_bss / total_ram * 100
    else:
        data_ram_pct = bss_pct = 0
    
    lines.append("RAM Usage (runtime memory)")
    lines.append(f"  data (initialized data):   {format_size(result.total_data):>12}  ({data_ram_pct:.1f}%)")
    lines.append(f"  bss (uninitialized data):  {format_size(result.total_bss):>12}  ({bss_pct:.1f}%)")
    lines.append(f"  RAM Total:                 {format_size(total_ram):>12}")
    lines.append("")
    lines.append(f"Grand Total:                 {format_size(total_size):>12}")
    lines.append("```")
    lines.append("")
    
    # By library
    lines.append("## By Library")
    lines.append("")
    lines.append(f"| Library | text (code) | data | bss | Flash Total | RAM Total |")
    lines.append("|---------|-------------|------|-----|-------------|-----------|")
    
    sorted_libs = sorted(result.libraries.values(), key=lambda x: x.total, reverse=True)
    for lib in sorted_libs:
        lines.append(f"| {lib.name} | {format_size(lib.text)} | {format_size(lib.data)} | "
                    f"{format_size(lib.bss)} | {format_size(lib.flash)} | {format_size(lib.ram)} |")
    
    lines.append("")
    
    # Library proportion analysis
    lines.append("### Flash Proportion by Library")
    lines.append("")
    lines.append("```")
    for lib in sorted_libs:
        pct = lib.flash / total_flash * 100 if total_flash > 0 else 0
        bar_len = int(pct / 2)
        bar = "█" * bar_len
        lines.append(f"{lib.name:<50} {bar} {pct:5.1f}% ({format_size(lib.flash)})")
    lines.append("```")
    lines.append("")
    
    # Top N object files
    lines.append(f"## Top {top_n} Largest Object Files")
    lines.append("")
    lines.append(f"| Rank | Object File | Library | Flash | RAM |")
    lines.append("|------|-------------|---------|-------|-----|")
    
    sorted_objects = sorted(result.objects, key=lambda x: x.total, reverse=True)
    for i, obj in enumerate(sorted_objects[:top_n], 1):
        lines.append(f"| {i} | {obj.name} | {obj.library} | "
                    f"{format_size(obj.flash)} | {format_size(obj.ram)} |")
    
    lines.append("")
    
    # Detailed module analysis
    lines.append("## Detailed Module Analysis")
    lines.append("")
    
    for lib in sorted_libs:
        flash_pct = lib.flash / total_flash * 100 if total_flash > 0 else 0
        ram_pct = lib.ram / total_ram * 100 if total_ram > 0 else 0
        
        lines.append(f"### {lib.name}")
        lines.append("")
        lines.append("```")
        lines.append(f"  text (code):               {format_size(lib.text):>12}")
        lines.append(f"  data (initialized data):   {format_size(lib.data):>12}")
        lines.append(f"  bss (uninitialized data):  {format_size(lib.bss):>12}")
        lines.append(f"  Flash Usage:               {format_size(lib.flash):>12}  ({flash_pct:.1f}% of total Flash)")
        lines.append(f"  RAM Usage:                 {format_size(lib.ram):>12}  ({ram_pct:.1f}% of total RAM)")
        lines.append(f"  Total:                     {format_size(lib.total):>12}")
        lines.append("```")
        
        # Show Top 10 object files for this library
        sorted_objs = sorted(lib.objects, key=lambda x: x.total, reverse=True)
        lines.append("")
        lines.append("**Top 10 Largest Objects:**")
        lines.append("")
        for i, obj in enumerate(sorted_objs[:10], 1):
            lines.append(f"{i:2d}. `{obj.name}` - Flash: {format_size(obj.flash)}, RAM: {format_size(obj.ram)}")
        lines.append("")
    
    # Optimization suggestions
    lines.append("=" * 120)
    lines.append("## Optimization Suggestions")
    lines.append("=" * 120)
    lines.append("")
    
    # Identify large objects
    large_objects = [obj for obj in sorted_objects if obj.flash > threshold]
    if large_objects:
        lines.append(f"### 1. Large Object File Optimization (Flash > {format_size(int(threshold))})")
        lines.append("")
        lines.append(f"Found {len(large_objects)} large object files, recommend reviewing:")
        lines.append("")
        for obj in large_objects[:8]:
            lines.append(f"- `{obj.name}` - Flash: {format_size(obj.flash)}")
            
            # General optimization suggestions
            obj_lower = obj.name.lower()
            if 'tool' in obj_lower or 'test' in obj_lower or 'debug' in obj_lower:
                lines.append(f"  - Check if debug/test code is included, consider conditional compilation")
            elif 'graph' in obj_lower or 'parser' in obj_lower:
                lines.append(f"  - Complex logic module, check if algorithms and data structures can be optimized")
            elif obj.flash > 20000:
                lines.append(f"  - Very large module, consider splitting into sub-modules for on-demand linking")
            else:
                lines.append(f"  - Check for unused functions, excessive inlining, or optimizable implementations")
        lines.append("")
    
    # RAM usage analysis
    if total_ram > 0:
        large_data = [obj for obj in sorted_objects if obj.data > 100]
        if large_data:
            lines.append(f"### 2. Initialized Data Optimization (data > 100B)")
            lines.append("")
            lines.append(f"Found {len(large_data)} objects with significant initialized data:")
            lines.append("")
            for obj in large_data[:5]:
                lines.append(f"- `{obj.name}` - data: {format_size(obj.data)}")
                lines.append(f"  - Suggestion: check if data can be made const (moved to rodata) or initialized at runtime")
            lines.append("")
    
    # Compilation optimization suggestions
    lines.append("### 3. Compilation Optimization Suggestions")
    lines.append("")
    lines.append("- [ ] Check if `-Os` (optimize for size) or `-Oz` is enabled")
    lines.append("- [ ] Ensure LTO (Link Time Optimization) is enabled")
    lines.append("- [ ] Check if `-ffunction-sections -fdata-sections` is enabled")
    lines.append("- [ ] Ensure `--gc-sections` is used at link time (remove unused code)")
    lines.append("- [ ] Consider using `-flto=thin` for faster LTO compilation")
    lines.append("")
    
    # Module-level suggestions
    lines.append("### 4. Module-Level Optimization Suggestions")
    lines.append("")
    top_modules = sorted_libs[:5]
    for lib in top_modules:
        if lib.flash > total_flash * 0.1:  # Over 10%
            lines.append(f"- **{lib.name}**")
            lines.append(f"  - Flash usage: {format_size(lib.flash)} ({lib.flash/total_flash*100:.1f}%)")
            lines.append(f"  - This module has a large proportion, consider splitting or optimizing")
    lines.append("")
    
    lines.append("=" * 120)
    
    return "\n".join(lines)


def generate_json_report(result: AnalysisResult) -> str:
    """Generate JSON format report"""
    report = {
        "summary": {
            "total_text": result.total_text,
            "total_data": result.total_data,
            "total_bss": result.total_bss,
            "total_flash": result.total_flash,
            "total_ram": result.total_ram,
            "total_size": result.total_size
        },
        "libraries": [
            {
                "name": lib.name,
                "text": lib.text,
                "data": lib.data,
                "bss": lib.bss,
                "flash": lib.flash,
                "ram": lib.ram,
                "total": lib.total,
                "object_count": len(lib.objects)
            }
            for lib in sorted(result.libraries.values(), key=lambda x: x.total, reverse=True)
        ],
        "objects": [
            {
                "name": obj.name,
                "library": obj.library,
                "text": obj.text,
                "data": obj.data,
                "bss": obj.bss,
                "flash": obj.flash,
                "ram": obj.ram,
                "total": obj.total
            }
            for obj in sorted(result.objects, key=lambda x: x.total, reverse=True)[:100]
        ]
    }
    return json.dumps(report, indent=2, ensure_ascii=False)


def generate_csv_report(result: AnalysisResult) -> str:
    """Generate CSV format report"""
    output = StringIO()
    writer = csv.writer(output)
    
    writer.writerow(["Object File", "Library", "text", "data", "bss", "flash", "ram", "total"])
    
    for obj in sorted(result.objects, key=lambda x: x.total, reverse=True):
        writer.writerow([
            obj.name,
            obj.library,
            obj.text,
            obj.data,
            obj.bss,
            obj.flash,
            obj.ram,
            obj.total
        ])
    
    return output.getvalue()


def main() -> int:
    """Main function
    
    Returns:
        Exit code: 0=success, 1=threshold exceeded, 2=error
    """
    parser = argparse.ArgumentParser(
        description='Analyze size command output and generate code size reports (enhanced)',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Basic usage
  %(prog)s size_output.txt
  
  # Show Top 20
  %(prog)s size_output.txt --top 20
  
  # Output in JSON format
  %(prog)s size_output.txt --format json
  
  # Output to file
  %(prog)s size_output.txt --output report.md
  
  # CI/CD integration: fail if Flash exceeds 512KB
  %(prog)s size_output.txt --fail-on-threshold 512
        """)
    
    parser.add_argument('input_file', 
                        help='Path to size command output file')
    parser.add_argument('--top', type=int, default=15,
                        help='Show Top N largest object files (default: 15)')
    parser.add_argument('--threshold', type=float, default=5.0,
                        help='Large object threshold in KB (default: 5.0)')
    parser.add_argument('--output', '-o',
                        help='Output report to specified file')
    parser.add_argument('--format', '-f', choices=['markdown', 'json', 'csv'], default='markdown',
                        help='Output format: markdown (default), json, csv')
    parser.add_argument('--module', default='Code',
                        help='Module name for report title (default: "Code")')
    parser.add_argument('--fail-on-threshold', type=float, default=None,
                        help='Return non-zero exit code if Flash exceeds this threshold (KB)')
    
    args = parser.parse_args()
    
    # Parse file
    try:
        result = parse_size_file(args.input_file)
        
        if not result.objects:
            print(f"Error: cannot parse object info from {args.input_file}", file=sys.stderr)
            print("Please verify the file is in size command output format", file=sys.stderr)
            return 2
            
    except FileNotFoundError:
        print(f"Error: file not found - {args.input_file}", file=sys.stderr)
        return 2
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        return 2
    
    # Generate report
    if args.format == 'json':
        output = generate_json_report(result)
    elif args.format == 'csv':
        output = generate_csv_report(result)
    else:
        output = generate_markdown_report(result, args)
    
    # Output
    if args.output:
        try:
            with open(args.output, 'w', encoding='utf-8') as f:
                f.write(output)
            print(f"Report generated: {args.output}", file=sys.stderr)
        except Exception as e:
            print(f"Error: cannot create output file {args.output}: {e}", file=sys.stderr)
            return 2
    else:
        print(output)
    
    # Check threshold
    if args.fail_on_threshold is not None:
        threshold_bytes = args.fail_on_threshold * 1024
        if result.total_flash > threshold_bytes:
            print(f"Flash {format_size(result.total_flash)} exceeds threshold {format_size(int(threshold_bytes))}", 
                  file=sys.stderr)
            return 1
        else:
            print(f"Flash {format_size(result.total_flash)} within threshold", file=sys.stderr)
    
    return 0


if __name__ == "__main__":
    sys.exit(main())
