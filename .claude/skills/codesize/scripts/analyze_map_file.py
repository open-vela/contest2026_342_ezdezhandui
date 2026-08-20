#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Linker Map File Analysis Tool (Enhanced)

Features:
  - Parse linker-generated .map files
  - Collect statistics for each section (.text, .data, .bss, .rodata)
  - **Symbol-level analysis**: extract function and variable names
  - Auto-detect architecture (ARM/Xtensa/RISC-V)
  - Auto-filter Discarded sections
  - Statistics by library and object file
  - Identify large objects and optimization opportunities
  - Multiple output formats (Markdown/JSON/CSV)

Usage:
  python3 analyze_map_file.py <map_file> [options]

Examples:
  # Basic analysis
  python3 analyze_map_file.py vela_audio.map
  
  # Show Top 30 largest symbols (including function names)
  python3 analyze_map_file.py vela_audio.map --top 30 --show-symbols
  
  # Output in JSON format
  python3 analyze_map_file.py vela_audio.map --format json
  
  # Output to file
  python3 analyze_map_file.py vela_audio.map --output report.md

Author: AI Generated
Version: 2.0
Date: 2026-01-29
"""

from __future__ import annotations

import sys
import re
import json
import csv
import argparse
from collections import defaultdict
from pathlib import Path
from dataclasses import dataclass, field, asdict
from typing import Dict, List, Optional, Tuple, Set, Any
from io import StringIO

# Try to import common module
try:
    from codesize_utils import (
        format_size, format_percentage, detect_architecture,
        classify_section, demangle_symbol, extract_symbol_name,
        Architecture, SectionType, SymbolInfo, LibraryStats,
        SectionStats, AnalysisReport
    )
except ImportError:
    # If import fails, use built-in implementation
    from enum import Enum
    
    class Architecture(Enum):
        ARM = "arm"
        XTENSA = "xtensa"
        RISCV = "riscv"
        UNKNOWN = "unknown"
    
    class SectionType(Enum):
        TEXT = ".text"
        DATA = ".data"
        BSS = ".bss"
        RODATA = ".rodata"
        DTCM_BSS = ".dtcm.bss"
        OTHER = "other"
    
    def format_size(size_bytes: int) -> str:
        if abs(size_bytes) < 1024:
            return f"{size_bytes} B"
        elif abs(size_bytes) < 1024 * 1024:
            return f"{size_bytes / 1024:.2f} KB"
        else:
            return f"{size_bytes / (1024 * 1024):.2f} MB"


@dataclass
class EnhancedSymbolInfo:
    """Enhanced symbol information (including symbol name)"""
    name: str                    # Symbol name (function/variable name)
    address: int
    size: int
    section: str                 # Parent section
    library: str                 # Parent library
    object_file: str             # Object file
    section_full: str = ""       # Full section name (e.g. .text.function_name)
    is_function: bool = False    # Whether it is a function
    is_cpp: bool = False         # Whether it is a C++ symbol


@dataclass
class ParsedMapData:
    """Parsed Map file data"""
    architecture: Architecture = Architecture.UNKNOWN
    sections: Dict[str, int] = field(default_factory=dict)
    section_addresses: Dict[str, int] = field(default_factory=dict)
    libraries: Dict[str, Dict[str, Any]] = field(default_factory=dict)
    symbols: List[EnhancedSymbolInfo] = field(default_factory=list)
    discarded_size: int = 0


def detect_arch_from_content(content: str) -> Architecture:
    """Detect architecture from map file content"""
    content_lower = content.lower()
    
    if '.arm.' in content_lower or 'arm.attributes' in content_lower or '.ARM.' in content:
        return Architecture.ARM
    elif '.xt.' in content_lower or 'xtensa' in content_lower:
        return Architecture.XTENSA
    elif '.riscv.' in content_lower:
        return Architecture.RISCV
    
    return Architecture.UNKNOWN


def extract_symbol_name_from_section(section_name: str) -> Tuple[str, bool, bool]:
    """Extract symbol name from section name
    
    Args:
        section_name: e.g. ".text.FunctionName" or ".rodata.str1.1"
        
    Returns:
        (symbol_name, is_function, is_cpp)
    """
    if not section_name.startswith('.'):
        return section_name, False, False
    
    parts = section_name.split('.')
    is_function = parts[1] == 'text' if len(parts) > 1 else False
    
    # Extract symbol name
    if len(parts) >= 3:
        symbol_name = '.'.join(parts[2:])
        # Detect C++ symbol
        is_cpp = symbol_name.startswith('_Z')
        return symbol_name, is_function, is_cpp
    
    return section_name, is_function, False


def parse_map_file(filename: str) -> ParsedMapData:
    """Parse a map file
    
    Args:
        filename: Map file path
        
    Returns:
        ParsedMapData object
    """
    result = ParsedMapData()
    
    # Initialize section statistics
    result.sections = {
        '.text': 0, '.data': 0, '.bss': 0, 
        '.rodata': 0, '.dtcm.bss': 0, 'other': 0
    }
    result.section_addresses = {
        '.text': 0, '.data': 0, '.bss': 0,
        '.rodata': 0, '.dtcm.bss': 0, 'other': 0
    }
    
    current_section: Optional[str] = None
    in_discarded: bool = False
    
    # Regex patterns
    # Section header: .text           0x11580000   0x3e3e4c
    section_pattern = re.compile(
        r'^(\.\S+)\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)',
        re.IGNORECASE
    )
    
    # Symbol line (with section name): .text.func  0x11580000   0x1234  path/lib.a(obj.o)
    symbol_with_section = re.compile(
        r'^\s+(\.\S+)\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+(.+)',
        re.IGNORECASE
    )
    
    # Symbol line (without section name): 0x11580000   0x1234  path/lib.a(obj.o)
    symbol_pattern = re.compile(
        r'^\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)\s+(.+)',
        re.IGNORECASE
    )
    
    # fill line: *fill*  0x... 0x...
    fill_pattern = re.compile(r'^\s+\*fill\*', re.IGNORECASE)
    
    # Object file: path/to/lib.a(obj.o)
    object_pattern = re.compile(r'([\w./-]+\.a)\(([^)]+)\)', re.IGNORECASE)
    
    # Directly linked .o file
    direct_obj_pattern = re.compile(r'([\w./-]+\.o)\s*$', re.IGNORECASE)
    
    try:
        with open(filename, 'r', encoding='utf-8', errors='ignore') as f:
            content = f.read()
        
        # Detect architecture
        result.architecture = detect_arch_from_content(content)
        
        for line in content.split('\n'):
            line_stripped = line.rstrip()
            
            # Detect Discarded sections
            if 'Discarded input sections' in line_stripped:
                in_discarded = True
                continue
            
            # Detect main section start (reset discarded state)
            if (line_stripped.startswith('Memory Configuration') or 
                line_stripped.startswith('Linker script and memory map') or
                line_stripped.startswith('.text') and not in_discarded):
                in_discarded = False
            
            # Skip discarded sections
            if in_discarded:
                # Count discarded size
                match = symbol_pattern.match(line_stripped)
                if match:
                    try:
                        size = int(match.group(2), 16)
                        result.discarded_size += size
                    except ValueError:
                        pass
                continue
            
            # Skip fill lines
            if fill_pattern.match(line_stripped):
                continue
            
            # Check section header
            section_match = section_pattern.match(line_stripped)
            if section_match:
                section_name = section_match.group(1)
                section_addr = int(section_match.group(2), 16)
                section_size = int(section_match.group(3), 16)
                
                # Classify into major sections
                if section_name.startswith('.text'):
                    current_section = '.text'
                elif section_name.startswith('.rodata'):
                    current_section = '.rodata'
                elif 'dtcm.bss' in section_name.lower():
                    current_section = '.dtcm.bss'
                elif section_name.startswith('.data'):
                    current_section = '.data'
                elif section_name.startswith('.bss'):
                    current_section = '.bss'
                else:
                    current_section = 'other'
                
                result.sections[current_section] += section_size
                if result.section_addresses[current_section] == 0:
                    result.section_addresses[current_section] = section_addr
                continue
            
            # Check symbol line with section name
            symbol_sec_match = symbol_with_section.match(line_stripped)
            if symbol_sec_match:
                full_section = symbol_sec_match.group(1)
                addr = int(symbol_sec_match.group(2), 16)
                size = int(symbol_sec_match.group(3), 16)
                path = symbol_sec_match.group(4)
                
                # Filter invalid entries
                if addr == 0 or size == 0:
                    continue
                
                # Determine section type
                if full_section.startswith('.text'):
                    section = '.text'
                elif full_section.startswith('.rodata'):
                    section = '.rodata'
                elif 'dtcm.bss' in full_section.lower():
                    section = '.dtcm.bss'
                elif full_section.startswith('.data'):
                    section = '.data'
                elif full_section.startswith('.bss'):
                    section = '.bss'
                else:
                    section = 'other'
                
                # Extract symbol name
                symbol_name, is_function, is_cpp = extract_symbol_name_from_section(full_section)
                
                # Extract library and object file
                lib_name = "unknown"
                obj_name = "unknown"
                
                obj_match = object_pattern.search(path)
                if obj_match:
                    lib_path = obj_match.group(1)
                    obj_name = obj_match.group(2)
                    lib_name = Path(lib_path).name
                else:
                    # Check for directly linked .o file
                    direct_match = direct_obj_pattern.search(path)
                    if direct_match:
                        obj_name = Path(direct_match.group(1)).name
                        lib_name = "[direct link]"
                
                # Create symbol info
                sym = EnhancedSymbolInfo(
                    name=symbol_name,
                    address=addr,
                    size=size,
                    section=section,
                    library=lib_name,
                    object_file=obj_name,
                    section_full=full_section,
                    is_function=is_function,
                    is_cpp=is_cpp
                )
                result.symbols.append(sym)
                
                # Update library statistics
                if lib_name not in result.libraries:
                    result.libraries[lib_name] = {
                        'objects': set(),
                        'size': 0,
                        'sections': defaultdict(int),
                        'symbols': []
                    }
                
                result.libraries[lib_name]['objects'].add(obj_name)
                result.libraries[lib_name]['size'] += size
                result.libraries[lib_name]['sections'][section] += size
                result.libraries[lib_name]['symbols'].append(sym)
                continue
            
            # Check symbol line without section name
            symbol_match = symbol_pattern.match(line_stripped)
            if symbol_match and current_section:
                addr = int(symbol_match.group(1), 16)
                size = int(symbol_match.group(2), 16)
                path = symbol_match.group(3)
                
                # Filter invalid entries
                if addr == 0 or size == 0:
                    continue
                
                # Extract library and object file
                lib_name = "unknown"
                obj_name = "unknown"
                
                obj_match = object_pattern.search(path)
                if obj_match:
                    lib_path = obj_match.group(1)
                    obj_name = obj_match.group(2)
                    lib_name = Path(lib_path).name
                else:
                    direct_match = direct_obj_pattern.search(path)
                    if direct_match:
                        obj_name = Path(direct_match.group(1)).name
                        lib_name = "[direct link]"
                
                # Create symbol info (no specific symbol name)
                sym = EnhancedSymbolInfo(
                    name=f"[{obj_name}]",
                    address=addr,
                    size=size,
                    section=current_section,
                    library=lib_name,
                    object_file=obj_name,
                    is_function=(current_section == '.text')
                )
                result.symbols.append(sym)
                
                # Update library statistics
                if lib_name not in result.libraries:
                    result.libraries[lib_name] = {
                        'objects': set(),
                        'size': 0,
                        'sections': defaultdict(int),
                        'symbols': []
                    }
                
                result.libraries[lib_name]['objects'].add(obj_name)
                result.libraries[lib_name]['size'] += size
                result.libraries[lib_name]['sections'][current_section] += size
    
    except Exception as e:
        print(f"Error: failed to parse file - {e}", file=sys.stderr)
        raise
    
    return result


def generate_markdown_report(data: ParsedMapData, args: argparse.Namespace) -> str:
    """Generate Markdown format report"""
    lines: List[str] = []
    
    top_n = args.top
    threshold = args.threshold * 1024
    show_symbols = args.show_symbols
    
    lines.append("# Map File Analysis Report")
    lines.append("")
    lines.append(f"**Architecture**: {data.architecture.value.upper()}")
    if data.discarded_size > 0:
        lines.append(f"**Filtered unlinked code**: {format_size(data.discarded_size)}")
    lines.append("")
    
    # Calculate total sizes
    total_flash = data.sections['.text'] + data.sections['.rodata'] + data.sections['.data']
    total_ram = data.sections['.data'] + data.sections['.bss'] + data.sections['.dtcm.bss']
    total_size = total_flash + data.sections['.bss'] + data.sections['.dtcm.bss']
    
    # Overview
    lines.append("## Overview")
    lines.append("")
    lines.append("```")
    lines.append("Flash Usage (code and data burned to Flash)")
    
    text_pct = data.sections['.text'] / total_flash * 100 if total_flash > 0 else 0
    rodata_pct = data.sections['.rodata'] / total_flash * 100 if total_flash > 0 else 0
    data_pct = data.sections['.data'] / total_flash * 100 if total_flash > 0 else 0
    
    lines.append(f"  .text (code):              {format_size(data.sections['.text']):>12}  ({text_pct:.1f}%)")
    lines.append(f"  .rodata (read-only data):  {format_size(data.sections['.rodata']):>12}  ({rodata_pct:.1f}%)")
    lines.append(f"  .data (initialized data):  {format_size(data.sections['.data']):>12}  ({data_pct:.1f}%)")
    lines.append(f"  Flash Total:               {format_size(total_flash):>12}")
    lines.append("")
    lines.append("RAM Usage (runtime memory)")
    
    if total_ram > 0:
        data_ram_pct = data.sections['.data'] / total_ram * 100
        bss_pct = data.sections['.bss'] / total_ram * 100
        dtcm_pct = data.sections['.dtcm.bss'] / total_ram * 100
    else:
        data_ram_pct = bss_pct = dtcm_pct = 0
    
    lines.append(f"  .data (initialized data):  {format_size(data.sections['.data']):>12}  ({data_ram_pct:.1f}%)")
    lines.append(f"  .bss (uninitialized):      {format_size(data.sections['.bss']):>12}  ({bss_pct:.1f}%)")
    lines.append(f"  .dtcm.bss:                 {format_size(data.sections['.dtcm.bss']):>12}  ({dtcm_pct:.1f}%)")
    lines.append(f"  RAM Total:                 {format_size(total_ram):>12}")
    lines.append("")
    lines.append(f"Grand Total:                 {format_size(total_size):>12}")
    lines.append("```")
    lines.append("")
    
    # By library
    lines.append("## By Library")
    lines.append("")
    lines.append("| Library | Objects | Total Size | .text | .rodata | .data | .bss |")
    lines.append("|---------|---------|------------|-------|---------|-------|------|")
    
    sorted_libs = sorted(data.libraries.items(), key=lambda x: x[1]['size'], reverse=True)
    for lib_name, stats in sorted_libs[:30]:
        text_size = stats['sections']['.text']
        rodata_size = stats['sections']['.rodata']
        data_size = stats['sections']['.data']
        bss_size = stats['sections']['.bss'] + stats['sections']['.dtcm.bss']
        
        lines.append(f"| {lib_name} | {len(stats['objects'])} | {format_size(stats['size'])} | "
                    f"{format_size(text_size)} | {format_size(rodata_size)} | "
                    f"{format_size(data_size)} | {format_size(bss_size)} |")
    
    lines.append("")
    
    # Visual proportions
    lines.append("### Flash Proportion by Library (Top 15)")
    lines.append("")
    lines.append("```")
    for lib_name, stats in sorted_libs[:15]:
        lib_flash = stats['sections']['.text'] + stats['sections']['.rodata'] + stats['sections']['.data']
        pct = lib_flash / total_flash * 100 if total_flash > 0 else 0
        bar_len = int(pct / 2)
        bar = "█" * bar_len
        lines.append(f"{lib_name:<50} {bar} {pct:5.1f}% ({format_size(lib_flash)})")
    lines.append("```")
    lines.append("")
    
    # Top N largest symbols (with symbol names)
    if show_symbols:
        lines.append(f"## Top {top_n} Largest Symbols (with function names)")
    else:
        lines.append(f"## Top {top_n} Largest Symbols")
    lines.append("")
    
    if show_symbols:
        lines.append("| Rank | Symbol | Section | Size | Library | Object File |")
        lines.append("|------|--------|---------|------|---------|-------------|")
    else:
        lines.append("| Rank | Section | Size | Library | Object File |")
        lines.append("|------|---------|------|---------|-------------|")
    
    sorted_symbols = sorted(data.symbols, key=lambda x: x.size, reverse=True)
    for i, sym in enumerate(sorted_symbols[:top_n], 1):
        # Handle C++ symbol markers
        name_display = sym.name
        if sym.is_cpp:
            name_display = f"`{sym.name}` 🔷"  # C++ marker
        elif sym.is_function:
            name_display = f"`{sym.name}()`"
        else:
            name_display = f"`{sym.name}`"
        
        if show_symbols:
            lines.append(f"| {i} | {name_display} | {sym.section} | {format_size(sym.size)} | "
                        f"{sym.library} | {sym.object_file} |")
        else:
            lines.append(f"| {i} | {sym.section} | {format_size(sym.size)} | "
                        f"{sym.library} | {sym.object_file} |")
    
    lines.append("")
    
    # Optimization suggestions
    lines.append("## Optimization Suggestions")
    lines.append("")
    
    large_symbols = [sym for sym in sorted_symbols if sym.size > threshold]
    if large_symbols:
        lines.append(f"### 1. Large Object Analysis (size > {format_size(int(threshold))})")
        lines.append("")
        
        # Group by section
        by_section: Dict[str, List[EnhancedSymbolInfo]] = defaultdict(list)
        for sym in large_symbols[:30]:
            by_section[sym.section].append(sym)
        
        for section, syms in by_section.items():
            lines.append(f"**{section} section** ({len(syms)} large objects):")
            lines.append("")
            for sym in syms[:10]:
                name_str = f"`{sym.name}`" if sym.name else sym.object_file
                lines.append(f"- {format_size(sym.size):<10} {sym.library} / {name_str}")
                
                # Targeted suggestions
                if section in ('.bss', '.dtcm.bss'):
                    if sym.size > 10240:
                        lines.append(f"  - Consider dynamic allocation or reducing buffer size")
                elif section == '.rodata':
                    if sym.size > 4096:
                        lines.append(f"  - Large read-only data, consider compression or external storage")
                elif section == '.text':
                    if sym.is_cpp:
                        lines.append(f"  - C++ symbol, check for template bloat and vtable overhead")
                    else:
                        lines.append(f"  - Large function, check if it can be split or algorithm optimized")
            lines.append("")
    
    # Compilation optimization suggestions
    lines.append("### 2. Compilation Optimization Suggestions")
    lines.append("")
    lines.append("- [ ] Ensure `-Os` or `-Oz` (optimize for size) is enabled")
    lines.append("- [ ] Enable LTO (Link Time Optimization)")
    lines.append("- [ ] Use `-ffunction-sections -fdata-sections`")
    lines.append("- [ ] Use `--gc-sections` at link time (remove unused code)")
    lines.append("- [ ] Check if debug code has been removed")
    lines.append("")
    
    # Architecture-specific suggestions
    if data.architecture == Architecture.ARM:
        lines.append("### 3. ARM Architecture Specific Suggestions")
        lines.append("")
        lines.append("- Use Thumb-2 instruction set (`-mthumb`)")
        lines.append("- Check exception handling table size (`.ARM.extab`, `.ARM.exidx`)")
    elif data.architecture == Architecture.XTENSA:
        lines.append("### 3. Xtensa Architecture Specific Suggestions")
        lines.append("")
        lines.append("- Check literal pool size")
        lines.append("- Optimize DSP-related code")
    
    lines.append("")
    
    return "\n".join(lines)


def generate_json_report(data: ParsedMapData) -> str:
    """Generate JSON format report"""
    total_flash = data.sections['.text'] + data.sections['.rodata'] + data.sections['.data']
    total_ram = data.sections['.data'] + data.sections['.bss'] + data.sections['.dtcm.bss']
    
    report = {
        "architecture": data.architecture.value,
        "discarded_size": data.discarded_size,
        "summary": {
            "flash": total_flash,
            "ram": total_ram,
            "sections": {k: v for k, v in data.sections.items() if v > 0}
        },
        "libraries": [
            {
                "name": name,
                "size": stats['size'],
                "object_count": len(stats['objects']),
                "sections": dict(stats['sections'])
            }
            for name, stats in sorted(data.libraries.items(), key=lambda x: x[1]['size'], reverse=True)
        ],
        "top_symbols": [
            {
                "name": sym.name,
                "size": sym.size,
                "section": sym.section,
                "library": sym.library,
                "object_file": sym.object_file,
                "is_function": sym.is_function,
                "is_cpp": sym.is_cpp
            }
            for sym in sorted(data.symbols, key=lambda x: x.size, reverse=True)[:100]
        ]
    }
    return json.dumps(report, indent=2, ensure_ascii=False)


def generate_csv_report(data: ParsedMapData) -> str:
    """Generate CSV format report"""
    output = StringIO()
    writer = csv.writer(output)
    
    writer.writerow(["Symbol Name", "Size", "Section", "Library", "Object File", "Is Function", "Is C++"])
    
    for sym in sorted(data.symbols, key=lambda x: x.size, reverse=True):
        writer.writerow([
            sym.name,
            sym.size,
            sym.section,
            sym.library,
            sym.object_file,
            sym.is_function,
            sym.is_cpp
        ])
    
    return output.getvalue()


def main() -> int:
    """Main function"""
    parser = argparse.ArgumentParser(
        description='Analyze linker map files and generate code size reports (enhanced: symbol name resolution)',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Basic analysis
  %(prog)s vela_audio.map
  
  # Show symbol names (function/variable names)
  %(prog)s vela_audio.map --show-symbols
  
  # Show Top 30
  %(prog)s vela_audio.map --top 30
  
  # Output in JSON format
  %(prog)s vela_audio.map --format json
  
  # Output to file
  %(prog)s vela_audio.map --output report.md
        """)
    
    parser.add_argument('map_file', 
                        help='Path to the linker-generated .map file')
    parser.add_argument('--top', type=int, default=20,
                        help='Show Top N largest symbols (default: 20)')
    parser.add_argument('--threshold', type=float, default=1.0,
                        help='Large object threshold in KB (default: 1.0)')
    parser.add_argument('--output', '-o',
                        help='Output report to specified file')
    parser.add_argument('--format', '-f', choices=['markdown', 'json', 'csv'], default='markdown',
                        help='Output format: markdown (default), json, csv')
    parser.add_argument('--show-symbols', '-s', action='store_true',
                        help='Show symbol names (function/variable names) in the report')
    parser.add_argument('--fail-on-threshold', type=float, default=None,
                        help='Return non-zero exit code if Flash exceeds this threshold (KB)')
    
    args = parser.parse_args()
    
    # Parse file
    try:
        print(f"Parsing map file: {args.map_file}", file=sys.stderr)
        data = parse_map_file(args.map_file)
        
        total_size = sum(data.sections.values())
        if total_size == 0:
            print(f"Warning: no valid data parsed from map file", file=sys.stderr)
            return 2
        
        print(f"Parsing complete: arch={data.architecture.value}, symbols={len(data.symbols)}", file=sys.stderr)
        
    except FileNotFoundError:
        print(f"Error: file not found - {args.map_file}", file=sys.stderr)
        return 2
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        return 2
    
    # Generate report
    if args.format == 'json':
        output = generate_json_report(data)
    elif args.format == 'csv':
        output = generate_csv_report(data)
    else:
        output = generate_markdown_report(data, args)
    
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
        total_flash = data.sections['.text'] + data.sections['.rodata'] + data.sections['.data']
        threshold_bytes = args.fail_on_threshold * 1024
        if total_flash > threshold_bytes:
            print(f"Flash {format_size(total_flash)} exceeds threshold {format_size(int(threshold_bytes))}", file=sys.stderr)
            return 1
    
    return 0


if __name__ == "__main__":
    sys.exit(main())
