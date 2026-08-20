#!/usr/bin/env python3
"""
Compare differences between two memdumps
"""

import sys
from memdump_parser import MemdumpParser


def compare_memdumps(before_file: str, after_file: str, config_file: str = None):
    """Compare two memdump files"""
    parser_before = MemdumpParser(config_file)
    parser_after = MemdumpParser(config_file)
    
    parser_before.parse_log(before_file)
    parser_after.parse_log(after_file)
    
    print("\n" + "="*80)
    print(f"Diff: {before_file} -> {after_file}")
    print("="*80)
    
    all_tasks = set(parser_before.allocations.keys()) | set(parser_after.allocations.keys())
    
    for task in sorted(all_tasks):
        before = parser_before.allocations.get(task, [])
        after = parser_after.allocations.get(task, [])
        
        before_seqs = {a['sequence'] for a in before}
        after_seqs = {a['sequence'] for a in after}
        
        freed = before_seqs - after_seqs
        new = after_seqs - before_seqs
        kept = before_seqs & after_seqs
        
        before_size = sum(a['size'] for a in before)
        after_size = sum(a['size'] for a in after)
        delta = after_size - before_size
        
        print(f"\nTask: {task}")
        print(f"  Before: {len(before):4d} allocations, {before_size:8,} bytes")
        print(f"  After:  {len(after):4d} allocations, {after_size:8,} bytes")
        print(f"  Change: {len(after)-len(before):+4d} allocations, {delta:+8,} bytes")
        print(f"  Detail: kept {len(kept)}, freed {len(freed)}, new {len(new)}")
        
        # Show new large allocations
        if new:
            new_allocs = [a for a in after if a['sequence'] in new]
            new_allocs.sort(key=lambda x: x['size'], reverse=True)
            
            print(f"\n  New Allocations Top 5:")
            for i, alloc in enumerate(new_allocs[:5], 1):
                core_id = alloc.get('core', 'unknown')
                print(f"    {i}. {alloc['size']:6,} bytes (core: {core_id})")
                bt = parser_after.resolve_backtrace(alloc['backtrace'][:3], core_id)
                print(f"       {' -> '.join(bt)}")


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <before.log> <after.log> [config.json]")
        sys.exit(1)
    
    compare_memdumps(sys.argv[1], sys.argv[2], 
                     sys.argv[3] if len(sys.argv) > 3 else None)


if __name__ == "__main__":
    main()
