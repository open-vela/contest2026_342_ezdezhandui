#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Code Size Version Comparison Tool

Features:
  - Compare differences between two size command output files or map files
  - Highlight significant changes (increases/decreases)
  - Generate tabular diff reports
  - Multiple output formats (Markdown/JSON/CSV)

Usage:
  # Compare two size output files
  python3 compare_codesize.py before.txt after.txt

  # Compare two map files
  python3 compare_codesize.py before.map after.map --type map

  # Output to file
  python3 compare_codesize.py before.txt after.txt --output diff.md

  # Set significant change threshold
  python3 compare_codesize.py before.txt after.txt --threshold 5

Author: AI Generated
Version: 1.0
Date: 2026-01-29
"""

from __future__ import annotations

import sys
import json
import csv
import argparse
from pathlib import Path
from dataclasses import dataclass, field, asdict
from typing import Dict, List, Optional, Tuple
from collections import defaultdict
from io import StringIO


@dataclass
class ModuleStats:
    """Module statistics"""
    name: str
    text: int = 0
    data: int = 0
    bss: int = 0
    rodata: int = 0
    
    @property
    def flash(self) -> int:
        """Flash usage = text + data + rodata"""
        return self.text + self.data + self.rodata
    
    @property
    def ram(self) -> int:
        """RAM usage = data + bss"""
        return self.data + self.bss

    @property
    def total(self) -> int:
        """Total size"""
        return self.text + self.data + self.bss + self.rodata


@dataclass
class DiffResult:
    """Diff result"""
    name: str
    before: ModuleStats
    after: ModuleStats
    
    @property
    def delta_text(self) -> int:
        return self.after.text - self.before.text
    
    @property
    def delta_data(self) -> int:
        return self.after.data - self.before.data
    
    @property
    def delta_bss(self) -> int:
        return self.after.bss - self.before.bss
    
    @property
    def delta_rodata(self) -> int:
        return self.after.rodata - self.before.rodata
    
    @property
    def delta_flash(self) -> int:
        return self.after.flash - self.before.flash
    
    @property
    def delta_ram(self) -> int:
        return self.after.ram - self.before.ram
    
    @property
    def delta_total(self) -> int:
        return self.after.total - self.before.total
    
    @property
    def percent_change(self) -> float:
        """Change percentage"""
        if self.before.total == 0:
            return 100.0 if self.after.total > 0 else 0.0
        return (self.delta_total / self.before.total) * 100


@dataclass
class CompareReport:
    """Comparison report"""
    before_file: str
    after_file: str
    before_total: ModuleStats
    after_total: ModuleStats
    diffs: List[DiffResult] = field(default_factory=list)
    added_modules: List[ModuleStats] = field(default_factory=list)
    removed_modules: List[ModuleStats] = field(default_factory=list)


def format_size(size_bytes: int) -> str:
    """Format size for display"""
    if abs(size_bytes) < 1024:
        return f"{size_bytes} B"
    elif abs(size_bytes) < 1024 * 1024:
        return f"{size_bytes / 1024:.2f} KB"
    else:
        return f"{size_bytes / (1024 * 1024):.2f} MB"


def format_delta(delta: int) -> str:
    """Format delta for display"""
    if delta == 0:
        return "±0"
    sign = "+" if delta > 0 else ""
    return f"{sign}{format_size(delta)}"


def format_delta_with_indicator(delta: int, threshold_pct: float = 5.0, base: int = 0) -> str:
    """Format delta with indicator"""
    if delta == 0:
        return "  ±0"
    
    sign = "+" if delta > 0 else ""
    size_str = f"{sign}{format_size(delta)}"
    
    # Calculate percentage change
    if base > 0:
        pct = abs(delta / base * 100)
        if pct >= threshold_pct:
            indicator = "🔴" if delta > 0 else "🟢"
            return f"{indicator} {size_str}"
    
    if delta > 0:
        return f"⬆️  {size_str}"
    else:
        return f"⬇️  {size_str}"


def parse_json_file(filename: str) -> Dict[str, ModuleStats]:
    """Parse JSON file output from analyze_map_file.py"""
    modules: Dict[str, ModuleStats] = {}
    
    with open(filename, 'r', encoding='utf-8') as f:
        data = json.load(f)
    
    # Parse library list
    libraries = data.get('libraries', [])
    for lib in libraries:
        name = lib.get('name', 'unknown')
        sections = lib.get('sections', {})
        
        modules[name] = ModuleStats(
            name=name,
            text=sections.get('.text', 0),
            data=sections.get('.data', 0),
            bss=sections.get('.bss', 0),
            rodata=sections.get('.rodata', 0)
        )
    
    return modules


def parse_size_file(filename: str) -> Dict[str, ModuleStats]:
    """Parse size command output file"""
    modules: Dict[str, ModuleStats] = {}
    
    with open(filename, 'r', encoding='utf-8', errors='ignore') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('text') or '(TOTALS)' in line:
                continue
            
            parts = line.split()
            if len(parts) >= 6 and '(ex ' in line:
                try:
                    text = int(parts[0])
                    data = int(parts[1])
                    bss = int(parts[2])
                    
                    # Extract library name
                    obj_info = ' '.join(parts[5:])
                    obj_match = obj_info.split('(ex ')
                    if len(obj_match) == 2:
                        lib_path = obj_match[1].rstrip(')')
                        lib_parts = lib_path.split('/')
                        lib_name = lib_parts[-1] if lib_parts else lib_path
                        
                        if lib_name not in modules:
                            modules[lib_name] = ModuleStats(name=lib_name)
                        
                        modules[lib_name].text += text
                        modules[lib_name].data += data
                        modules[lib_name].bss += bss
                except (ValueError, IndexError):
                    continue
    
    return modules


def parse_map_file(filename: str) -> Dict[str, ModuleStats]:
    """Parse map file"""
    import re
    
    modules: Dict[str, ModuleStats] = {}
    current_section: Optional[str] = None
    in_discarded = False
    
    # Section header pattern
    section_pattern = re.compile(r'^(\.\w+[\w.]*)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)', re.IGNORECASE)
    # Symbol pattern
    symbol_pattern = re.compile(r'^\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(.+)', re.IGNORECASE)
    # Library file pattern
    object_pattern = re.compile(r'([\w./-]+\.a)\((\S+)\)', re.IGNORECASE)
    
    with open(filename, 'r', encoding='utf-8', errors='ignore') as f:
        for line in f:
            line = line.rstrip()
            
            # Detect Discarded sections
            if 'Discarded input sections' in line:
                in_discarded = True
                continue
            
            # Reset when encountering new major section
            if line.startswith('Memory Configuration') or line.startswith('Linker script'):
                in_discarded = False
                continue
            
            if in_discarded:
                continue
            
            # Check section header
            section_match = section_pattern.match(line)
            if section_match:
                section_name = section_match.group(1)
                if section_name.startswith('.text'):
                    current_section = 'text'
                elif section_name.startswith('.rodata'):
                    current_section = 'rodata'
                elif section_name.startswith('.data'):
                    current_section = 'data'
                elif section_name.startswith('.bss') or 'dtcm.bss' in section_name.lower():
                    current_section = 'bss'
                else:
                    current_section = None
                continue
            
            # Check symbol line
            symbol_match = symbol_pattern.match(line)
            if symbol_match and current_section:
                addr = int(symbol_match.group(1), 16)
                size = int(symbol_match.group(2), 16)
                path = symbol_match.group(3)
                
                # Filter invalid addresses
                if addr == 0:
                    continue
                
                # Extract library file
                obj_match = object_pattern.search(path)
                if obj_match:
                    lib_path = obj_match.group(1)
                    lib_name = Path(lib_path).name
                    
                    if lib_name not in modules:
                        modules[lib_name] = ModuleStats(name=lib_name)
                    
                    if current_section == 'text':
                        modules[lib_name].text += size
                    elif current_section == 'data':
                        modules[lib_name].data += size
                    elif current_section == 'bss':
                        modules[lib_name].bss += size
                    elif current_section == 'rodata':
                        modules[lib_name].rodata += size
    
    return modules


def compare_modules(before: Dict[str, ModuleStats], 
                    after: Dict[str, ModuleStats]) -> CompareReport:
    """Compare two module sets"""
    before_names = set(before.keys())
    after_names = set(after.keys())
    
    common = before_names & after_names
    added = after_names - before_names
    removed = before_names - after_names
    
    # Calculate diffs
    diffs: List[DiffResult] = []
    for name in common:
        diff = DiffResult(name=name, before=before[name], after=after[name])
        diffs.append(diff)
    
    # Sort by absolute change
    diffs.sort(key=lambda x: abs(x.delta_total), reverse=True)
    
    # Added modules
    added_modules = [after[name] for name in added]
    added_modules.sort(key=lambda x: x.total, reverse=True)
    
    # Removed modules
    removed_modules = [before[name] for name in removed]
    removed_modules.sort(key=lambda x: x.total, reverse=True)
    
    # Calculate totals
    before_total = ModuleStats(
        name="TOTAL",
        text=sum(m.text for m in before.values()),
        data=sum(m.data for m in before.values()),
        bss=sum(m.bss for m in before.values()),
        rodata=sum(m.rodata for m in before.values())
    )
    after_total = ModuleStats(
        name="TOTAL",
        text=sum(m.text for m in after.values()),
        data=sum(m.data for m in after.values()),
        bss=sum(m.bss for m in after.values()),
        rodata=sum(m.rodata for m in after.values())
    )
    
    return CompareReport(
        before_file="",
        after_file="",
        before_total=before_total,
        after_total=after_total,
        diffs=diffs,
        added_modules=added_modules,
        removed_modules=removed_modules
    )


def generate_markdown_report(report: CompareReport, 
                             threshold: float = 5.0,
                             top_n: int = 20) -> str:
    """Generate Markdown format report"""
    lines: List[str] = []
    
    lines.append("# Code Size Version Comparison Report")
    lines.append("")
    lines.append(f"- **Baseline**: {report.before_file}")
    lines.append(f"- **Compared**: {report.after_file}")
    lines.append("")
    
    # Overall changes
    total_diff = DiffResult(
        name="TOTAL",
        before=report.before_total,
        after=report.after_total
    )
    
    lines.append("## Overall Changes")
    lines.append("")
    lines.append("| Metric | Baseline | Compared | Change | Percentage |")
    lines.append("|--------|----------|----------|--------|------------|")
    
    for metric, before_val, after_val, delta in [
        ("Flash", report.before_total.flash, report.after_total.flash, total_diff.delta_flash),
        ("RAM", report.before_total.ram, report.after_total.ram, total_diff.delta_ram),
        (".text", report.before_total.text, report.after_total.text, total_diff.delta_text),
        (".data", report.before_total.data, report.after_total.data, total_diff.delta_data),
        (".bss", report.before_total.bss, report.after_total.bss, total_diff.delta_bss),
        (".rodata", report.before_total.rodata, report.after_total.rodata, total_diff.delta_rodata),
    ]:
        pct = (delta / before_val * 100) if before_val > 0 else 0
        indicator = "🔴" if delta > 0 and abs(pct) >= threshold else ("🟢" if delta < 0 and abs(pct) >= threshold else "")
        pct_str = f"{pct:+.1f}%" if delta != 0 else "0%"
        lines.append(f"| {metric} | {format_size(before_val)} | {format_size(after_val)} | {format_delta(delta)} | {indicator} {pct_str} |")
    
    lines.append("")
    
    # Significant changes
    significant = [d for d in report.diffs if abs(d.percent_change) >= threshold]
    if significant:
        lines.append(f"## Significant Changes (>{threshold}%)")
        lines.append("")
        lines.append("| Module | Baseline | Compared | Change | Percentage |")
        lines.append("|--------|----------|----------|--------|------------|")
        for diff in significant[:top_n]:
            indicator = "🔴" if diff.delta_total > 0 else "🟢"
            lines.append(f"| {diff.name} | {format_size(diff.before.total)} | {format_size(diff.after.total)} | {format_delta(diff.delta_total)} | {indicator} {diff.percent_change:+.1f}% |")
        lines.append("")
    
    # Added modules
    if report.added_modules:
        lines.append("## Added Modules")
        lines.append("")
        lines.append("| Module | Size | Flash | RAM |")
        lines.append("|--------|------|-------|-----|")
        for mod in report.added_modules[:10]:
            lines.append(f"| {mod.name} | {format_size(mod.total)} | {format_size(mod.flash)} | {format_size(mod.ram)} |")
        if len(report.added_modules) > 10:
            lines.append(f"| ... | {len(report.added_modules)} added modules total | | |")
        lines.append("")
    
    # Removed modules
    if report.removed_modules:
        lines.append("## Removed Modules")
        lines.append("")
        lines.append("| Module | Size | Flash | RAM |")
        lines.append("|--------|------|-------|-----|")
        for mod in report.removed_modules[:10]:
            lines.append(f"| {mod.name} | {format_size(mod.total)} | {format_size(mod.flash)} | {format_size(mod.ram)} |")
        if len(report.removed_modules) > 10:
            lines.append(f"| ... | {len(report.removed_modules)} removed modules total | | |")
        lines.append("")
    
    # Top N changes
    lines.append(f"## Top {top_n} Changed Modules")
    lines.append("")
    lines.append("| Rank | Module | Baseline | Compared | Change | Flash Change | RAM Change |")
    lines.append("|------|--------|----------|----------|--------|--------------|------------|")
    for i, diff in enumerate(report.diffs[:top_n], 1):
        lines.append(f"| {i} | {diff.name} | {format_size(diff.before.total)} | {format_size(diff.after.total)} | {format_delta(diff.delta_total)} | {format_delta(diff.delta_flash)} | {format_delta(diff.delta_ram)} |")
    lines.append("")
    
    # Analysis suggestions
    lines.append("## Analysis Suggestions")
    lines.append("")
    
    if total_diff.delta_flash > 0:
        lines.append(f"- ⚠️ Flash increased by {format_size(total_diff.delta_flash)}, recommend checking:")
        growing = [d for d in report.diffs if d.delta_flash > 1024][:5]
        for d in growing:
            lines.append(f"  - `{d.name}`: +{format_size(d.delta_flash)}")
    elif total_diff.delta_flash < 0:
        lines.append(f"- ✅ Flash decreased by {format_size(abs(total_diff.delta_flash))}, optimization effective!")
    
    if total_diff.delta_ram > 0:
        lines.append(f"- ⚠️ RAM increased by {format_size(total_diff.delta_ram)}, monitor memory pressure")
    elif total_diff.delta_ram < 0:
        lines.append(f"- ✅ RAM decreased by {format_size(abs(total_diff.delta_ram))}")
    
    if report.added_modules:
        total_added = sum(m.total for m in report.added_modules)
        lines.append(f"- Added {len(report.added_modules)} modules, total {format_size(total_added)}")
    
    if report.removed_modules:
        total_removed = sum(m.total for m in report.removed_modules)
        lines.append(f"- Removed {len(report.removed_modules)} modules, freed {format_size(total_removed)}")
    
    lines.append("")
    
    return "\n".join(lines)


def generate_json_report(report: CompareReport) -> str:
    """Generate JSON format report"""
    data = {
        "before_file": report.before_file,
        "after_file": report.after_file,
        "summary": {
            "before": {
                "flash": report.before_total.flash,
                "ram": report.before_total.ram,
                "text": report.before_total.text,
                "data": report.before_total.data,
                "bss": report.before_total.bss,
                "rodata": report.before_total.rodata,
            },
            "after": {
                "flash": report.after_total.flash,
                "ram": report.after_total.ram,
                "text": report.after_total.text,
                "data": report.after_total.data,
                "bss": report.after_total.bss,
                "rodata": report.after_total.rodata,
            },
            "delta": {
                "flash": report.after_total.flash - report.before_total.flash,
                "ram": report.after_total.ram - report.before_total.ram,
                "text": report.after_total.text - report.before_total.text,
                "data": report.after_total.data - report.before_total.data,
                "bss": report.after_total.bss - report.before_total.bss,
                "rodata": report.after_total.rodata - report.before_total.rodata,
            }
        },
        "modules": [
            {
                "name": d.name,
                "before_total": d.before.total,
                "after_total": d.after.total,
                "delta_total": d.delta_total,
                "delta_flash": d.delta_flash,
                "delta_ram": d.delta_ram,
                "percent_change": round(d.percent_change, 2)
            }
            for d in report.diffs
        ],
        "added": [{"name": m.name, "total": m.total} for m in report.added_modules],
        "removed": [{"name": m.name, "total": m.total} for m in report.removed_modules]
    }
    return json.dumps(data, indent=2, ensure_ascii=False)


def generate_csv_report(report: CompareReport) -> str:
    """Generate CSV format report"""
    output = StringIO()
    writer = csv.writer(output)
    
    # Header
    writer.writerow(["Module", "Baseline Size", "Compared Size", "Delta", "Change Percentage", "Flash Change", "RAM Change", "Status"])
    
    # Data rows
    for diff in report.diffs:
        status = "increased" if diff.delta_total > 0 else ("decreased" if diff.delta_total < 0 else "unchanged")
        writer.writerow([
            diff.name,
            diff.before.total,
            diff.after.total,
            diff.delta_total,
            f"{diff.percent_change:.2f}%",
            diff.delta_flash,
            diff.delta_ram,
            status
        ])
    
    # Added modules
    for mod in report.added_modules:
        writer.writerow([mod.name, 0, mod.total, mod.total, "added", mod.flash, mod.ram, "added"])
    
    # Removed modules
    for mod in report.removed_modules:
        writer.writerow([mod.name, mod.total, 0, -mod.total, "removed", -mod.flash, -mod.ram, "removed"])
    
    return output.getvalue()


def main() -> int:
    """Main function"""
    parser = argparse.ArgumentParser(
        description='Compare code size between two versions',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Compare two size output files
  %(prog)s before.txt after.txt
  
  # Compare two map files
  %(prog)s before.map after.map --type map
  
  # Output in JSON format
  %(prog)s before.txt after.txt --format json
  
  # Set significant change threshold to 10%%
  %(prog)s before.txt after.txt --threshold 10
  
  # Output to file
  %(prog)s before.txt after.txt --output diff_report.md
        """)
    
    parser.add_argument('before', help='Baseline version file')
    parser.add_argument('after', help='Compared version file')
    parser.add_argument('--type', '-t', choices=['size', 'map', 'json'], default='auto',
                        help='File type: auto (auto-detect), size, map, json')
    parser.add_argument('--format', '-f', choices=['markdown', 'json', 'csv'], default='markdown',
                        help='Output format: markdown (default), json, csv')
    parser.add_argument('--output', '-o', help='Output file path')
    parser.add_argument('--threshold', type=float, default=5.0,
                        help='Significant change threshold percentage (default: 5.0)')
    parser.add_argument('--top', type=int, default=20,
                        help='Show Top N changed modules (default: 20)')
    parser.add_argument('--fail-on-growth', type=float, default=None,
                        help='Return non-zero exit code if Flash growth exceeds this percentage')
    
    args = parser.parse_args()
    
    # Validate files
    for filepath in [args.before, args.after]:
        if not Path(filepath).exists():
            print(f"Error: file not found - {filepath}", file=sys.stderr)
            return 2
    
    # Parse files
    try:
        # Auto-detect file type
        def detect_and_parse(filepath: str, file_type: str) -> Dict[str, ModuleStats]:
            if file_type == 'auto':
                if filepath.endswith('.json'):
                    file_type = 'json'
                elif filepath.endswith('.map'):
                    file_type = 'map'
                else:
                    # Try reading file header to determine type
                    with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
                        first_line = f.readline().strip()
                    if first_line.startswith('{'):
                        file_type = 'json'
                    elif 'text' in first_line and 'data' in first_line:
                        file_type = 'size'
                    else:
                        file_type = 'map'
            
            if file_type == 'json':
                return parse_json_file(filepath)
            elif file_type == 'map':
                return parse_map_file(filepath)
            else:
                return parse_size_file(filepath)
        
        print(f"Parsing baseline: {args.before}", file=sys.stderr)
        before_modules = detect_and_parse(args.before, args.type)
        
        print(f"Parsing compared version: {args.after}", file=sys.stderr)
        after_modules = detect_and_parse(args.after, args.type)
        
        if not before_modules and not after_modules:
            print("Error: cannot parse module info from files", file=sys.stderr)
            return 2
        
    except Exception as e:
        print(f"Error: failed to parse files - {e}", file=sys.stderr)
        return 2
    
    # Generate comparison report
    print("Generating comparison report...", file=sys.stderr)
    report = compare_modules(before_modules, after_modules)
    report.before_file = args.before
    report.after_file = args.after
    
    # Generate output
    if args.format == 'json':
        output = generate_json_report(report)
    elif args.format == 'csv':
        output = generate_csv_report(report)
    else:
        output = generate_markdown_report(report, args.threshold, args.top)
    
    # Output
    if args.output:
        with open(args.output, 'w', encoding='utf-8') as f:
            f.write(output)
        print(f"Report generated: {args.output}", file=sys.stderr)
    else:
        print(output)
    
    # Check growth threshold
    if args.fail_on_growth is not None:
        before_flash = report.before_total.flash
        delta_flash = report.after_total.flash - before_flash
        if before_flash > 0:
            growth_pct = (delta_flash / before_flash) * 100
            if growth_pct > args.fail_on_growth:
                print(f"Flash growth {growth_pct:.1f}% exceeds threshold {args.fail_on_growth}%", file=sys.stderr)
                return 1
    
    return 0


if __name__ == "__main__":
    sys.exit(main())
