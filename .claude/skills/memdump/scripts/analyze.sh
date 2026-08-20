#!/bin/bash
# Memdump Quick Analysis Script

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PARSER="${SCRIPT_DIR}/memdump_parser.py"
DIFF="${SCRIPT_DIR}/memdump_diff.py"
LEAK="${SCRIPT_DIR}/memdump_leak_detect.py"

show_usage() {
    cat << EOF
Memdump Analysis Tool

Usage:
  $0 stat <log_file> [config]        # Memory usage statistics
  $0 diff <before> <after> [config]  # Compare differences
  $0 leak <log1> <log2> ... [config] # Detect leaks

Examples:
  $0 stat startup.log
  $0 diff before.log after.log multi_core.json
  $0 leak t1.log t2.log t3.log
EOF
}

if [ $# -lt 2 ]; then
    show_usage
    exit 1
fi

COMMAND="$1"
shift

case "$COMMAND" in
    stat)
        python3 "$PARSER" "$@"
        ;;
    diff)
        python3 "$DIFF" "$@"
        ;;
    leak)
        python3 "$LEAK" "$@"
        ;;
    *)
        echo "Unknown command: $COMMAND"
        show_usage
        exit 1
        ;;
esac
