#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Claude Code Stop / SessionEnd hook → forwards stdin JSON to shared snapshot_core.
# Deployed at: <demo-repo>/.claude/hooks/snapshot.sh (referenced from .claude/settings.json)

set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CORE="$SCRIPT_DIR/../../shared/snapshot_core.py"

if [ ! -f "$CORE" ]; then
  echo "[session-log] FATAL: snapshot_core.py not found at $CORE" >&2
  exit 2
fi

exec python3 "$CORE" --tool claude-code
