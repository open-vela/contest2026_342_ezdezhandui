#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Codex CLI Stop hook → thin wrapper around shared snapshot_core.
# Deployed at: ~/.codex/hooks/snapshot.py (referenced from ~/.codex/hooks.json).
#
# Note: First-time use requires `/hooks → trust` inside Codex CLI.
# If hook gets modified later, re-trust is needed.

import os
import sys
from pathlib import Path

CANDIDATE_CORES = [
    Path(os.environ.get("CONTEST_SNAPSHOT_CORE", "")),
    Path.home() / ".codex" / "hooks" / "snapshot_core.py",
    Path(__file__).resolve().parent.parent.parent / "shared" / "snapshot_core.py",
    Path(__file__).resolve().parent.parent / "shared" / "snapshot_core.py",
]

CORE = next((p for p in CANDIDATE_CORES if p and p.is_file()), None)
if CORE is None:
    sys.stderr.write(
        "[session-log] FATAL: snapshot_core.py not found. "
        "Set CONTEST_SNAPSHOT_CORE env var to the absolute path.\n"
    )
    sys.exit(2)

os.execvp("python3", ["python3", str(CORE), "--tool", "codex"])
