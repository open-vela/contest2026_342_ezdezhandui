#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Code Size Analysis Tool - Common Module

Provides type definitions, data structures, and utility functions
for use by other analysis tools.

Author: AI Generated
Version: 1.0
Date: 2026-01-29
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, List, Optional, Any
from enum import Enum


class Architecture(Enum):
    """Supported architecture types"""
    ARM = "arm"
    XTENSA = "xtensa"
    RISCV = "riscv"
    UNKNOWN = "unknown"


class SectionType(Enum):
    """Section types"""
    TEXT = ".text"
    DATA = ".data"
    BSS = ".bss"
    RODATA = ".rodata"
    DTCM_BSS = ".dtcm.bss"
    OTHER = "other"


@dataclass
class SymbolInfo:
    """Symbol information"""
    name: str
    address: int
    size: int
    section: SectionType
    library: str
    object_file: str
    source_file: Optional[str] = None
    
    def __repr__(self) -> str:
        return f"<Symbol {self.name}: {self.size} bytes in {self.section.value}>"


@dataclass 
class ObjectStats:
    """Object file statistics"""
    name: str
    library: str
    lib_path: str = ""
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
class LibraryStats:
    """Library file statistics"""
    name: str
    path: str = ""
    text: int = 0
    data: int = 0
    bss: int = 0
    rodata: int = 0
    dtcm_bss: int = 0
    objects: List[ObjectStats] = field(default_factory=list)
    symbols: List[SymbolInfo] = field(default_factory=list)
    
    @property
    def flash(self) -> int:
        """Flash usage"""
        return self.text + self.data + self.rodata
    
    @property
    def ram(self) -> int:
        """RAM usage"""
        return self.data + self.bss + self.dtcm_bss
    
    @property
    def total(self) -> int:
        """Total size"""
        return self.text + self.data + self.bss + self.rodata + self.dtcm_bss


@dataclass
class SectionStats:
    """Section statistics"""
    name: str
    size: int = 0
    address: int = 0
    symbols: List[SymbolInfo] = field(default_factory=list)


@dataclass
class AnalysisReport:
    """Analysis report"""
    # Basic information
    source_file: str
    architecture: Architecture = Architecture.UNKNOWN
    
    # Section statistics
    sections: Dict[str, SectionStats] = field(default_factory=dict)
    
    # Library statistics
    libraries: Dict[str, LibraryStats] = field(default_factory=dict)
    
    # Object file statistics
    objects: List[ObjectStats] = field(default_factory=list)
    
    # All symbols
    symbols: List[SymbolInfo] = field(default_factory=list)
    
    @property
    def total_text(self) -> int:
        return self.sections.get('.text', SectionStats('.text')).size
    
    @property
    def total_data(self) -> int:
        return self.sections.get('.data', SectionStats('.data')).size
    
    @property
    def total_bss(self) -> int:
        return self.sections.get('.bss', SectionStats('.bss')).size
    
    @property
    def total_rodata(self) -> int:
        return self.sections.get('.rodata', SectionStats('.rodata')).size
    
    @property
    def total_flash(self) -> int:
        return self.total_text + self.total_data + self.total_rodata
    
    @property
    def total_ram(self) -> int:
        dtcm = self.sections.get('.dtcm.bss', SectionStats('.dtcm.bss')).size
        return self.total_data + self.total_bss + dtcm
    
    @property
    def total_size(self) -> int:
        return sum(s.size for s in self.sections.values())


def format_size(size_bytes: int) -> str:
    """Format size for display
    
    Args:
        size_bytes: Number of bytes
        
    Returns:
        Formatted size string (e.g. "1.23 KB")
    """
    if abs(size_bytes) < 1024:
        return f"{size_bytes} B"
    elif abs(size_bytes) < 1024 * 1024:
        return f"{size_bytes / 1024:.2f} KB"
    else:
        return f"{size_bytes / (1024 * 1024):.2f} MB"


def format_percentage(value: int, total: int, decimals: int = 1) -> str:
    """Format percentage
    
    Args:
        value: The value
        total: The total
        decimals: Number of decimal places
        
    Returns:
        Formatted percentage string
    """
    if total == 0:
        return "0%"
    pct = (value / total) * 100
    return f"{pct:.{decimals}f}%"


def detect_architecture(content: str) -> Architecture:
    """Detect architecture from map file content
    
    Args:
        content: Map file content
        
    Returns:
        Detected architecture type
    """
    content_lower = content.lower()
    
    if '.arm.' in content_lower or 'arm.attributes' in content_lower:
        return Architecture.ARM
    elif '.xt.' in content_lower or 'xtensa' in content_lower:
        return Architecture.XTENSA
    elif '.riscv.' in content_lower:
        return Architecture.RISCV
    
    return Architecture.UNKNOWN


def classify_section(section_name: str) -> SectionType:
    """Classify a section name into a major section type
    
    Args:
        section_name: Section name
        
    Returns:
        Section type enum
    """
    name_lower = section_name.lower()
    
    if name_lower.startswith('.text'):
        return SectionType.TEXT
    elif name_lower.startswith('.rodata'):
        return SectionType.RODATA
    elif 'dtcm.bss' in name_lower:
        return SectionType.DTCM_BSS
    elif name_lower.startswith('.data'):
        return SectionType.DATA
    elif name_lower.startswith('.bss'):
        return SectionType.BSS
    
    return SectionType.OTHER


def demangle_symbol(name: str) -> str:
    """C++ symbol name demangling (simple implementation)
    
    For complex C++ symbols, use the c++filt tool instead.
    
    Args:
        name: Possibly mangled symbol name
        
    Returns:
        Attempted demangled symbol name
    """
    # Simple handling: if it starts with _Z, mark as C++ symbol
    if name.startswith('_Z'):
        return f"[C++] {name}"
    return name


def extract_symbol_name(text: str) -> Optional[str]:
    """Extract symbol name from a map file line
    
    Supported formats:
    - .text.FunctionName
    - .rodata.str1.1
    - .bss.variable_name
    
    Args:
        text: Section/symbol description from map file
        
    Returns:
        Extracted symbol name, or None if extraction fails
    """
    if not text:
        return None
    
    parts = text.strip().split()
    if not parts:
        return None
    
    section_or_symbol = parts[0]
    
    # Handle .text.FunctionName format
    if section_or_symbol.startswith('.'):
        segments = section_or_symbol.split('.')
        if len(segments) >= 3:
            # .text.FunctionName -> FunctionName
            # .rodata.str1.1 -> str1.1
            return '.'.join(segments[2:])
    
    return section_or_symbol


def generate_optimization_suggestions(report: AnalysisReport) -> List[str]:
    """Generate optimization suggestions based on analysis results
    
    Args:
        report: Analysis report
        
    Returns:
        List of optimization suggestions
    """
    suggestions: List[str] = []
    
    # Flash optimization suggestions
    if report.total_flash > 512 * 1024:  # > 512KB
        suggestions.append("Flash usage is large, consider enabling LTO and -Os optimization")
    
    # rodata optimization suggestions
    rodata_pct = report.total_rodata / report.total_flash * 100 if report.total_flash > 0 else 0
    if rodata_pct > 20:
        suggestions.append(f".rodata accounts for {rodata_pct:.1f}%, check if large lookup tables can be compressed")
    
    # BSS optimization suggestions
    if report.total_bss > 100 * 1024:  # > 100KB
        suggestions.append("BSS section is large, check if large static buffers can be dynamically allocated")
    
    # General suggestions
    suggestions.append("Ensure -ffunction-sections -fdata-sections and --gc-sections are enabled")
    
    return suggestions


def print_visual_bar(value: int, total: int, width: int = 50) -> str:
    """Generate a visual progress bar
    
    Args:
        value: Current value
        total: Total value
        width: Progress bar width
        
    Returns:
        Visual progress bar string
    """
    if total == 0:
        return "█" * 0 + "░" * width
    
    filled = int((value / total) * width)
    empty = width - filled
    return "█" * filled + "░" * empty
