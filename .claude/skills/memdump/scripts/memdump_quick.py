#!/usr/bin/env python3
"""
memdump_quick.py - Quick Memory Analysis Tool (standalone)

Features:
- No external module dependencies
- Auto-detects log format
- Analyzes without ELF files
- Provides detailed diagnostics
- Multiple output formats
"""

import re
import sys
import json
from collections import defaultdict
from typing import Dict, List, Optional, Tuple


class LogFormat:
    """Log format definitions"""
    PATTERNS = {
        'vela_timestamped': {
            'pattern': r'^\[[\d/\s:\.]+\]\s+\[\s*\d+\]\s+\[(?:ap|cp)\]\s+(\d+)\s+(\d+)\s+(\d+)\s+(0x[0-9a-fA-F]+)\s+(.*)',
            'fields': ['pid', 'size', 'overhead', 'address', 'backtrace'],
            'description': 'Vela with timestamp and core prefix'
        },
        'vela_standard': {
            'pattern': r'^\s*(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(0x[0-9a-fA-F]+)\s+(.*?)(?:\s*$)',
            'fields': ['pid', 'size', 'overhead', 'sequence', 'address', 'backtrace'],
            'description': 'Vela standard format with sequence'
        },
        'nuttx_simple': {
            'pattern': r'^\s*(\d+)\s+(\d+)\s+(\d+)\s+(0x[0-9a-fA-F]+)\s+(.*?)(?:\s*$)',
            'fields': ['pid', 'size', 'overhead', 'address', 'backtrace'],
            'description': 'NuttX simple format'
        }
    }


class QuickAnalyzer:
    """Quick analyzer"""
    
    def __init__(self, verbose=False):
        self.verbose = verbose
        self.format_pattern = None
        self.format_name = None
        self.pid_stats = defaultdict(lambda: {
            'count': 0,
            'total_size': 0,
            'total_overhead': 0,
            'allocations': []
        })
        self.parse_errors = 0
        self.total_lines = 0
        
    def detect_format(self, log_file: str) -> Optional[Tuple[str, str]]:
        """Auto-detect log format"""
        format_matches = defaultdict(int)
        sample_size = 10000  # Increase sample size
        checked_lines = 0
        
        with open(log_file, 'r', encoding='utf-8', errors='ignore') as f:
            for i, line in enumerate(f):
                if checked_lines >= 100:  # 100 matches is enough
                    break
                if i >= sample_size:  # Scan at most 10000 lines
                    break
                    
                for fmt_name, fmt_info in LogFormat.PATTERNS.items():
                    if re.match(fmt_info['pattern'], line):
                        format_matches[fmt_name] += 1
                        checked_lines += 1
                        break
        
        if not format_matches:
            return None
            
        # Select the format with most matches
        best_format = max(format_matches.items(), key=lambda x: x[1])
        return best_format[0], LogFormat.PATTERNS[best_format[0]]['pattern']
    
    def parse_line(self, line: str) -> Optional[Dict]:
        """Parse a single line of data"""
        if not self.format_pattern:
            return None
            
        match = re.match(self.format_pattern, line)
        if not match:
            return None
        
        try:
            groups = match.groups()
            # Determine parsing method based on number of matched fields
            if len(groups) >= 5:
                pid = int(groups[0])
                size = int(groups[1])
                overhead = int(groups[2])
                
                # Check if sequence field exists
                if len(groups) >= 6 and groups[3].startswith('0x'):
                    # Has sequence
                    address = groups[4]
                    backtrace = groups[5].strip() if len(groups) > 5 else ''
                else:
                    # No sequence
                    address = groups[3]
                    backtrace = groups[4].strip() if len(groups) > 4 else ''
                
                return {
                    'pid': pid,
                    'size': size,
                    'overhead': overhead,
                    'address': address,
                    'backtrace': backtrace,
                    'total': size + overhead
                }
        except (ValueError, IndexError) as e:
            if self.verbose:
                print(f"Parse error: {line[:50]}... - {e}", file=sys.stderr)
            return None
        
        return None
    
    def analyze(self, log_file: str, target_pid: Optional[int] = None):
        """Analyze log file"""
        # Detect format
        detected = self.detect_format(log_file)
        if not detected:
            raise ValueError("Cannot identify log format! Please check if the log file contains memdump data")
        
        self.format_name, self.format_pattern = detected
        
        if self.verbose:
            print(f"Detected format: {self.format_name}")
            print(f"  Description: {LogFormat.PATTERNS[self.format_name]['description']}")
        
        # Parse log
        with open(log_file, 'r', encoding='utf-8', errors='ignore') as f:
            for line in f:
                self.total_lines += 1
                data = self.parse_line(line)
                
                if data:
                    pid = data['pid']
                    # If target_pid specified, only process that PID
                    if target_pid is not None and pid != target_pid:
                        continue
                        
                    self.pid_stats[pid]['count'] += 1
                    self.pid_stats[pid]['total_size'] += data['size']
                    self.pid_stats[pid]['total_overhead'] += data['overhead']
                    self.pid_stats[pid]['allocations'].append(data)
                else:
                    # Not a data line, may be header or other content
                    if re.search(r'\d+.*0x[0-9a-fA-F]+', line):
                        self.parse_errors += 1
    
    def print_diagnostics(self):
        """Print diagnostic information"""
        print(f"\n{'='*60}")
        print("Diagnostics")
        print(f"{'='*60}")
        print(f"Total lines: {self.total_lines:,}")
        print(f"Detected format: {self.format_name}")
        print(f"Successfully parsed: {sum(s['count'] for s in self.pid_stats.values()):,} records")
        print(f"Parse failures: {self.parse_errors} lines")
        
        if self.parse_errors > 0:
            error_rate = self.parse_errors / self.total_lines * 100
            print(f"Failure rate: {error_rate:.2f}%")
            if error_rate > 1.0:
                print("Warning: high failure rate, possible format issues")
        else:
            print("Parse quality: excellent")
        
        print(f"\nProcesses found: {len(self.pid_stats)}")
    
    def print_summary(self, target_pid: Optional[int] = None):
        """Print statistics summary"""
        if target_pid:
            self._print_pid_detail(target_pid)
        else:
            self._print_all_pids()
    
    def _print_pid_detail(self, pid: int):
        """Print detailed info for a specific PID"""
        if pid not in self.pid_stats:
            print(f"\nNo data found for PID {pid}")
            return
        
        s = self.pid_stats[pid]
        total_mem = s['total_size'] + s['total_overhead']
        
        print(f"\n{'='*60}")
        print(f"PID {pid} Memory Analysis")
        print(f"{'='*60}")
        print(f"Allocation count: {s['count']:,}")
        print(f"Effective data: {s['total_size']:,} bytes ({s['total_size']/1024:.2f} KB)")
        print(f"Management overhead: {s['total_overhead']:,} bytes ({s['total_overhead']/1024:.2f} KB)")
        print(f"Total memory: {total_mem:,} bytes ({total_mem/1024:.2f} KB)")
        print(f"Average allocation: {s['total_size']/s['count']:.1f} bytes")
        print(f"Overhead ratio: {s['total_overhead']/s['total_size']*100:.1f}%")
        
        # Analyze call stack hotspots
        self._print_hotspots(pid)
        
        # Analyze allocation size distribution
        self._print_size_distribution(pid)
    
    def _print_all_pids(self):
        """Print overview of all PIDs"""
        print(f"\n{'='*60}")
        print("All Process Memory Statistics")
        print(f"{'='*60}")
        print(f"{'PID':<6} {'Count':>8} {'Data Size':>12} {'Overhead':>10} {'Total':>12}")
        print(f"{'-'*60}")
        
        # Sort by total memory
        sorted_pids = sorted(
            self.pid_stats.items(),
            key=lambda x: x[1]['total_size'] + x[1]['total_overhead'],
            reverse=True
        )
        
        total_mem_all = 0
        for pid, s in sorted_pids:
            total = s['total_size'] + s['total_overhead']
            total_mem_all += total
            print(f"{pid:<6} {s['count']:>8} {s['total_size']:>10} B {s['total_overhead']:>8} B {total:>10} B")
        
        print(f"{'-'*60}")
        print(f"{'Total':<6} {sum(s['count'] for s in self.pid_stats.values()):>8} "
              f"{sum(s['total_size'] for s in self.pid_stats.values()):>10} B "
              f"{sum(s['total_overhead'] for s in self.pid_stats.values()):>8} B "
              f"{total_mem_all:>10} B")
    
    def _print_hotspots(self, pid: int, top_n: int = 10):
        """Print memory hotspots"""
        s = self.pid_stats[pid]
        
        # Aggregate by first address of backtrace
        bt_stats = defaultdict(lambda: {'count': 0, 'size': 0})
        for alloc in s['allocations']:
            bt = alloc['backtrace'].split()[0] if alloc['backtrace'] else 'unknown'
            bt_stats[bt]['count'] += 1
            bt_stats[bt]['size'] += alloc['size']
        
        print(f"\n{'='*60}")
        print(f"Top {top_n} Allocation Hotspots (by size)")
        print(f"{'='*60}")
        print(f"{'Call Address':<15} {'Count':>8} {'Total Size':>12} {'Average':>10} {'Ratio':>8}")
        print(f"{'-'*60}")
        
        sorted_bt = sorted(bt_stats.items(), key=lambda x: x[1]['size'], reverse=True)[:top_n]
        total_size = s['total_size']
        
        for bt, info in sorted_bt:
            avg = info['size'] / info['count']
            pct = info['size'] / total_size * 100
            print(f"{bt:<15} {info['count']:>8} {info['size']:>10} B {avg:>8.1f} B {pct:>7.1f}%")
    
    def _print_size_distribution(self, pid: int):
        """Print allocation size distribution"""
        s = self.pid_stats[pid]
        
        # Define size buckets
        buckets = [
            (0, 64, "0-64B"),
            (64, 256, "64-256B"),
            (256, 1024, "256B-1KB"),
            (1024, 4096, "1-4KB"),
            (4096, float('inf'), ">4KB")
        ]
        
        bucket_stats = {label: {'count': 0, 'size': 0} for _, _, label in buckets}
        
        for alloc in s['allocations']:
            size = alloc['size']
            for min_size, max_size, label in buckets:
                if min_size <= size < max_size:
                    bucket_stats[label]['count'] += 1
                    bucket_stats[label]['size'] += size
                    break
        
        print(f"\n{'='*60}")
        print("Allocation Size Distribution")
        print(f"{'='*60}")
        print(f"{'Range':<12} {'Count':>8} {'Total Size':>12} {'Ratio':>8}")
        print(f"{'-'*60}")
        
        total_size = s['total_size']
        for label in [l for _, _, l in buckets]:
            info = bucket_stats[label]
            if info['count'] > 0:
                pct = info['size'] / total_size * 100
                print(f"{label:<12} {info['count']:>8} {info['size']:>10} B {pct:>7.1f}%")
    
    def export_json(self, output_file: str, target_pid: Optional[int] = None):
        """Export as JSON format"""
        data = {
            'format': self.format_name,
            'total_lines': self.total_lines,
            'parse_errors': self.parse_errors,
            'processes': {}
        }
        
        pids_to_export = [target_pid] if target_pid else self.pid_stats.keys()
        
        for pid in pids_to_export:
            if pid in self.pid_stats:
                s = self.pid_stats[pid]
                data['processes'][pid] = {
                    'count': s['count'],
                    'total_size': s['total_size'],
                    'total_overhead': s['total_overhead'],
                    'allocations': [
                        {
                            'size': a['size'],
                            'overhead': a['overhead'],
                            'address': a['address'],
                            'backtrace': a['backtrace']
                        }
                        for a in s['allocations']
                    ]
                }
        
        with open(output_file, 'w') as f:
            json.dump(data, f, indent=2)
        
        print(f"\nExported to: {output_file}")


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 memdump_quick.py <log_file> [pid] [options]")
        print("\nOptions:")
        print("  --verbose, -v      Show detailed information")
        print("  --diagnose, -d     Show diagnostic information")
        print("  --json <file>      Export as JSON format")
        print("\nExamples:")
        print("  python3 memdump_quick.py log.txt              # Analyze all processes")
        print("  python3 memdump_quick.py log.txt 12           # Analyze PID 12")
        print("  python3 memdump_quick.py log.txt 12 -v        # Verbose mode")
        print("  python3 memdump_quick.py log.txt --diagnose   # Diagnostic mode")
        print("  python3 memdump_quick.py log.txt 12 --json report.json  # Export JSON")
        sys.exit(1)
    
    log_file = sys.argv[1]
    target_pid = None
    verbose = False
    diagnose = False
    json_output = None
    
    # Parse arguments
    i = 2
    while i < len(sys.argv):
        arg = sys.argv[i]
        if arg in ('--verbose', '-v'):
            verbose = True
        elif arg in ('--diagnose', '-d'):
            diagnose = True
        elif arg in ('--json', '-j'):
            if i + 1 < len(sys.argv):
                json_output = sys.argv[i + 1]
                i += 1
        elif arg.isdigit():
            target_pid = int(arg)
        i += 1
    
    # Run analysis
    analyzer = QuickAnalyzer(verbose=verbose)
    
    try:
        print(f"Analyzing: {log_file}")
        if target_pid:
            print(f"Target PID: {target_pid}")
        
        analyzer.analyze(log_file, target_pid)
        
        if diagnose:
            analyzer.print_diagnostics()
        
        analyzer.print_summary(target_pid)
        
        if json_output:
            analyzer.export_json(json_output, target_pid)
        
        print(f"\n{'='*60}")
        print("Analysis complete")
        print(f"{'='*60}")
        
    except Exception as e:
        print(f"\nError: {e}", file=sys.stderr)
        if verbose:
            import traceback
            traceback.print_exc()
        sys.exit(1)


if __name__ == '__main__':
    main()
