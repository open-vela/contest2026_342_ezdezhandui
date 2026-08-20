#!/usr/bin/env python3
"""
Memory Leak Detector
Analyzes memdumps from multiple time points, groups by backtrace to detect growth trends
"""

import sys
from collections import defaultdict
from memdump_parser import MemdumpParser


def detect_leaks(log_files: list, config_file: str = None):
    """Detect memory leaks"""
    parsers = []
    
    for log_file in log_files:
        parser = MemdumpParser(config_file)
        parser.parse_log(log_file)
        parsers.append(parser)
    
    print("\n" + "="*80)
    print(f"Memory Leak Detection ({len(log_files)} time points)")
    print("="*80)
    
    # Total memory at each time point
    for i, parser in enumerate(parsers, 1):
        total_allocs = sum(len(allocs) for allocs in parser.allocations.values())
        total_size = sum(sum(a['size'] for a in allocs) 
                        for allocs in parser.allocations.values())
        print(f"T{i} ({log_files[i-1]}): {total_allocs:4d} allocations, {total_size:8,} bytes")
    
    # Group by backtrace for detection
    print("\nAnalysis by call stack groups:")
    
    for task in parsers[0].allocations.keys():
        bt_groups = []
        
        for parser in parsers:
            groups = defaultdict(list)
            for alloc in parser.allocations.get(task, []):
                # Use first 3 levels of backtrace as key
                bt_key = tuple(alloc['backtrace'][:3])
                groups[bt_key].append(alloc)
            bt_groups.append(groups)
        
        # Find growing backtraces
        all_keys = set()
        for groups in bt_groups:
            all_keys.update(groups.keys())
        
        leaking_keys = []
        for bt_key in all_keys:
            counts = [len(groups.get(bt_key, [])) for groups in bt_groups]
            if counts[-1] > counts[0]:  # Last time point > first time point
                growth = counts[-1] - counts[0]
                leaking_keys.append((bt_key, counts, growth))
        
        if leaking_keys:
            print(f"\n  Task {task}: found {len(leaking_keys)} suspicious growth points:")
            
            # Sort by growth amount
            leaking_keys.sort(key=lambda x: x[2], reverse=True)
            
            for bt_key, counts, growth in leaking_keys[:5]:
                print(f"\n  Growth: {counts[0]} -> {counts[-1]} (+{growth})")
                print(f"  Trend: {' -> '.join(map(str, counts))}")
                
                # Resolve backtrace
                core_id = parsers[0].allocations[task][0].get('core', 'default')
                resolved = parsers[0].resolve_backtrace(list(bt_key), core_id)
                print(f"  Backtrace: {' -> '.join(resolved)}")


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <log1> <log2> [log3] ... [config.json]")
        print(f"\nExamples:")
        print(f"  {sys.argv[0]} baseline.log mid.log final.log")
        sys.exit(1)
    
    # Check if last argument is a config file
    if sys.argv[-1].endswith('.json'):
        log_files = sys.argv[1:-1]
        config_file = sys.argv[-1]
    else:
        log_files = sys.argv[1:]
        config_file = None
    
    detect_leaks(log_files, config_file)


if __name__ == "__main__":
    main()
