#!/usr/bin/env python3
"""
Memdump Log Parser
Supports single-core and multi-core scenarios, auto-detects cores and selects corresponding ELF files
"""

import re
import sys
import json
import subprocess
from collections import defaultdict
from typing import List, Dict, Tuple, Optional


class MemdumpParser:
    def __init__(self, config_file: Optional[str] = None):
        """Initialize parser, load configuration"""
        self.config = self.load_config(config_file)
        self.allocations = defaultdict(list)
        self.core_mapping = {}  # Record which core each task belongs to
    
    def load_config(self, config_file: Optional[str]) -> Dict:
        """Load ELF configuration file"""
        if config_file:
            with open(config_file) as f:
                return json.load(f)
        # Default single-core configuration
        return {
            "cores": {
                "default": {
                    "elf_file": "./vela_audio.elf",
                    "addr2line": "addr2line"
                }
            }
        }
    
    def detect_core(self, lines: List[str], task_name: str) -> str:
        """Detect core ID from log context"""
        # Strategy 1: Look for log prefix [CPU0] or [CPU1]
        for line in lines:
            if match := re.search(r'\[(CPU\d+)\]', line, re.IGNORECASE):
                return match.group(1).lower()
        
        # Strategy 2: Task name matching
        for core_id, core_config in self.config["cores"].items():
            if "tasks" in core_config:
                if task_name in core_config["tasks"]:
                    return core_id
        
        # Strategy 3: Address range (need to parse one record)
        for line in lines:
            if match := re.search(r'0x([0-9a-fA-F]+)', line):
                addr = int(match.group(1), 16)
                for core_id, core_config in self.config["cores"].items():
                    if "address_prefix" in core_config:
                        prefix = int(core_config["address_prefix"], 16)
                        if (addr & 0xF0000000) == (prefix & 0xF0000000):
                            return core_id
        
        return "default"
    
    def parse_log(self, log_file: str):
        """Parse log file"""
        with open(log_file) as f:
            lines = f.readlines()
        
        current_task = None
        task_start_line = 0
        
        for i, line in enumerate(lines):
            # Detect task start marker
            if match := re.search(r'Memdump task (\S+)', line):
                if current_task:
                    # Process previous task
                    self.process_task(lines[task_start_line:i], current_task)
                
                current_task = match.group(1)
                task_start_line = i
        
        # Process last task
        if current_task:
            self.process_task(lines[task_start_line:], current_task)
    
    def process_task(self, lines: List[str], task_name: str):
        """Process memdump data for a single task"""
        # Detect core
        core_id = self.detect_core(lines, task_name)
        self.core_mapping[task_name] = core_id
        
        print(f"Processing task: {task_name} (core: {core_id})")
        
        # Parse allocation records
        for line in lines:
            if alloc := self.parse_allocation(line):
                alloc['task'] = task_name
                alloc['core'] = core_id
                self.allocations[task_name].append(alloc)
    
    def parse_allocation(self, line: str) -> Optional[Dict]:
        """Parse a single allocation record"""
        # Format: PID Size Overhead Sequence Address Backtrace...
        parts = line.split()
        if len(parts) < 6:
            return None
        
        try:
            return {
                'pid': int(parts[0]),
                'size': int(parts[1]),
                'overhead': int(parts[2]),
                'sequence': int(parts[3]),
                'address': parts[4],
                'backtrace': parts[5:13]  # Up to 8 levels
            }
        except ValueError:
            return None
    
    def resolve_backtrace(self, backtrace: List[str], core_id: str) -> List[str]:
        """Resolve backtrace using addr2line"""
        core_config = self.config["cores"].get(core_id, self.config["cores"]["default"])
        elf_file = core_config["elf_file"]
        addr2line = core_config["addr2line"]
        
        result = []
        for addr in backtrace:
            try:
                cmd = [addr2line, "-e", elf_file, "-f", "-C", addr]
                output = subprocess.check_output(cmd, text=True, stderr=subprocess.DEVNULL)
                lines = output.strip().split('\n')
                if len(lines) >= 2:
                    func = lines[0]
                    location = lines[1]
                    result.append(f"{func} [{location}]")
                else:
                    result.append(addr)
            except:
                result.append(addr)
        
        return result
    
    def print_statistics(self):
        """Print memory usage statistics"""
        print("\n" + "="*80)
        print("Memory Usage Statistics")
        print("="*80)
        
        for task_name, allocs in sorted(self.allocations.items()):
            core_id = self.core_mapping.get(task_name, "unknown")
            total_size = sum(a['size'] for a in allocs)
            total_overhead = sum(a['overhead'] for a in allocs)
            
            print(f"\nTask: {task_name} (core: {core_id})")
            print(f"  Total memory: {total_size:,} bytes ({total_size/1024:.2f} KB)")
            print(f"  Management overhead: {total_overhead:,} bytes ({total_overhead/total_size*100:.1f}%)")
            print(f"  Allocation count: {len(allocs)}")
            print(f"  Average allocation: {total_size/len(allocs):.1f} bytes")
    
    def print_top_allocations(self, n: int = 10):
        """Print the N largest allocations"""
        print("\n" + "="*80)
        print(f"Top {n} Largest Allocations")
        print("="*80)
        
        all_allocs = []
        for task_name, allocs in self.allocations.items():
            for a in allocs:
                all_allocs.append(a)
        
        all_allocs.sort(key=lambda x: x['size'], reverse=True)
        
        for i, alloc in enumerate(all_allocs[:n], 1):
            print(f"\n#{i}: {alloc['size']:,} bytes (task: {alloc['task']}, core: {alloc['core']})")
            print(f"  Address: {alloc['address']} sequence: {alloc['sequence']}")
            print(f"  Backtrace:")
            
            resolved = self.resolve_backtrace(alloc['backtrace'], alloc['core'])
            for depth, frame in enumerate(reversed(resolved), 1):
                print(f"    [{depth}] {frame}")


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <log_file> [config.json]")
        print(f"\nExamples:")
        print(f"  {sys.argv[0]} tmp.log")
        print(f"  {sys.argv[0]} tmp.log multi_core_config.json")
        sys.exit(1)
    
    log_file = sys.argv[1]
    config_file = sys.argv[2] if len(sys.argv) > 2 else None
    
    parser = MemdumpParser(config_file)
    parser.parse_log(log_file)
    parser.print_statistics()
    parser.print_top_allocations(15)


if __name__ == "__main__":
    main()
