#!/usr/bin/env python3
"""
Export AI Coding sessions from staging into the demo repo's logs/ directory.

Staging path:  ~/.claude/contest-collector-staging/<github_login>/<date>/<tool>__<sid>.jsonl
Target path:   <repo_root>/logs/<github_login>/<date>/<tool>__<sid>.jsonl

Normally sessions auto-export into the demo repo's logs/ at session end
(see the adapters). This script is the manual path: list, re-export, or
selectively export staged sessions.

Usage:
  python3 tools/export-session.py --latest
  python3 tools/export-session.py --session <session-id>
  python3 tools/export-session.py --since <date>
  python3 tools/export-session.py --all
  python3 tools/export-session.py --list
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from datetime import datetime, timedelta, timezone
from pathlib import Path


def staging_root() -> Path:
    return Path.home() / ".claude" / "contest-collector-staging"


def _load_env_github_login() -> str | None:
    env_file = Path.home() / ".claude" / "contest-collector.env"
    if not env_file.is_file():
        return None
    try:
        for line in env_file.read_text(encoding="utf-8").splitlines():
            if line.startswith("GITHUB_LOGIN="):
                val = line.split("=", 1)[1].strip().strip('"').strip("'")
                return val or None
    except Exception:
        return None
    return None


def repo_root() -> Path | None:
    try:
        out = subprocess.check_output(
            ["git", "rev-parse", "--show-toplevel"],
            stderr=subprocess.PIPE, text=True,
        )
        return Path(out.strip())
    except Exception:
        return None


def list_sessions(github_login: str | None) -> list[dict]:
    root = staging_root()
    if not root.exists():
        return []
    if github_login is None:
        env_login = _load_env_github_login()
        if env_login:
            github_login = env_login
    sessions = []
    for member_dir in sorted(root.iterdir()):
        if not member_dir.is_dir() or member_dir.name == "errors":
            continue
        if github_login and member_dir.name != github_login:
            continue
        manifest_path = member_dir / "manifest.json"
        if not manifest_path.exists():
            continue
        try:
            manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        except Exception:
            continue
        for s in manifest.get("sessions", []):
            sessions.append({
                "github_login": member_dir.name,
                "session_id": s.get("session_id"),
                "tool": s.get("tool"),
                "started_at": s.get("started_at", ""),
                "last_event_at": s.get("last_event_at", ""),
                "event_count": s.get("event_count", 0),
                "file_path": s.get("file_path", ""),
                "model": s.get("model", ""),
            })
    return sessions


def filter_sessions(sessions: list[dict], args) -> list[dict]:
    if args.session:
        return [s for s in sessions if s["session_id"] == args.session]
    if args.latest:
        if not sessions:
            return []
        return [max(sessions, key=lambda s: s.get("last_event_at") or "")]
    if args.since:
        try:
            cutoff = datetime.fromisoformat(args.since.replace("Z", "+00:00"))
        except ValueError:
            cutoff = datetime.combine(
                datetime.strptime(args.since, "%Y-%m-%d").date(),
                datetime.min.time(), tzinfo=timezone.utc,
            )
        if cutoff.tzinfo is None:
            cutoff = cutoff.replace(tzinfo=timezone.utc)
        result = []
        for s in sessions:
            ts = s.get("last_event_at") or s.get("started_at") or ""
            try:
                ev_dt = datetime.fromisoformat(ts.replace("Z", "+00:00"))
                if ev_dt.tzinfo is None:
                    ev_dt = ev_dt.replace(tzinfo=timezone.utc)
                if ev_dt >= cutoff:
                    result.append(s)
            except ValueError:
                continue
        return result
    if args.today:
        today = datetime.now(timezone.utc).date()
        result = []
        for s in sessions:
            ts = s.get("last_event_at") or s.get("started_at") or ""
            try:
                ev_dt = datetime.fromisoformat(ts.replace("Z", "+00:00"))
                if ev_dt.date() == today:
                    result.append(s)
            except ValueError:
                continue
        return result
    if args.all:
        return sessions
    return []


def copy_session(s: dict, dest_repo: Path, dry_run: bool) -> tuple[bool, str]:
    src_root = staging_root() / s["github_login"]
    src_jsonl = src_root / s["file_path"].split(f"{s['github_login']}/", 1)[-1] \
        if "logs/" in s["file_path"] else None
    if src_jsonl is None or not src_jsonl.exists():
        date_part, fname = None, None
        if s["file_path"].count("/") >= 2:
            parts = s["file_path"].split("/")
            date_part, fname = parts[-2], parts[-1]
        if date_part and fname:
            src_jsonl = src_root / date_part / fname
    if not src_jsonl or not src_jsonl.exists():
        return False, f"source jsonl not found for session {s['session_id']}"

    rel_inside = src_jsonl.relative_to(src_root)
    dest_jsonl = dest_repo / "logs" / s["github_login"] / rel_inside

    if dry_run:
        return True, f"[dry-run] would copy {src_jsonl} -> {dest_jsonl}"

    dest_jsonl.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src_jsonl, dest_jsonl)

    actual_count = sum(1 for line in dest_jsonl.read_text(encoding="utf-8").splitlines() if line.strip())
    s["event_count"] = actual_count
    s["file_path"] = f"logs/{s['github_login']}/{rel_inside}"

    update_dest_manifest(s, dest_repo)
    return True, f"copied -> {dest_jsonl.relative_to(dest_repo)} ({actual_count} events)"


def update_dest_manifest(s: dict, dest_repo: Path) -> None:
    member_dir = dest_repo / "logs" / s["github_login"]
    manifest_path = member_dir / "manifest.json"
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    else:
        src_manifest_path = staging_root() / s["github_login"] / "manifest.json"
        src_manifest = json.loads(src_manifest_path.read_text(encoding="utf-8"))
        manifest = {
            "schema_version": src_manifest.get("schema_version", "1.0"),
            "team_id": src_manifest.get("team_id", ""),
            "github_login": s["github_login"],
            "generator": src_manifest.get("generator", ""),
            "sessions": [],
        }
    existing = next((x for x in manifest["sessions"]
                     if x.get("session_id") == s["session_id"]), None)
    src_manifest_path = staging_root() / s["github_login"] / "manifest.json"
    src_manifest = json.loads(src_manifest_path.read_text(encoding="utf-8"))
    src_entry = next((x for x in src_manifest.get("sessions", [])
                      if x.get("session_id") == s["session_id"]), None)
    if src_entry is None:
        return
    entry = dict(src_entry)
    if "event_count" in s:
        entry["event_count"] = s["event_count"]
    if "file_path" in s:
        entry["file_path"] = s["file_path"]
    if existing:
        existing.update(entry)
    else:
        manifest["sessions"].append(entry)
    manifest["updated_at"] = datetime.now(timezone.utc).isoformat()
    manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False),
                             encoding="utf-8")


def cmd_list(args, sessions: list[dict]) -> int:
    if not sessions:
        print("No sessions in staging.")
        print(f"Staging path: {staging_root()}")
        return 0
    print(f"{'session_id':<40} {'tool':<12} {'last_event_at':<26} "
          f"{'events':>6}  model")
    print("-" * 100)
    for s in sorted(sessions, key=lambda x: x.get("last_event_at") or ""):
        sid = (s["session_id"] or "")[:38]
        tool = s.get("tool", "")[:10]
        last = s.get("last_event_at", "")[:25]
        cnt = s.get("event_count", 0)
        model = s.get("model", "")
        print(f"{sid:<40} {tool:<12} {last:<26} {cnt:>6}  {model}")
    return 0


def cmd_backfill(args) -> int:
    dest = Path(args.dest) if args.dest else repo_root()
    if dest is None:
        sys.stderr.write("Cannot detect demo repo. "
                         "Run inside the repo or pass --dest.\n")
        return 2

    if args.force:
        staging = staging_root()
        if staging.exists():
            print(f"[--force] wiping staging at {staging}")
            shutil.rmtree(staging)

    claude_projects = Path.home() / ".claude" / "projects"
    if not claude_projects.is_dir():
        sys.stderr.write(f"No Claude Code history found at {claude_projects}\n")
        return 1

    skill_root = Path(__file__).resolve().parent.parent
    core_py = skill_root / "adapters" / "shared" / "snapshot_core.py"
    if not core_py.exists():
        sys.stderr.write(f"Cannot find snapshot_core.py at {core_py}\n")
        return 1

    transcripts = sorted(claude_projects.glob("*/*.jsonl"))
    if not transcripts:
        sys.stderr.write("No transcript files found in Claude Code history.\n")
        return 0

    env_vars = subprocess.check_output(
        ["bash", "-c",
         "source ~/.claude/contest-collector.env 2>/dev/null && "
         "echo \"TEAM_ID=${TEAM_ID:-}\" && echo \"GITHUB_LOGIN=${GITHUB_LOGIN:-}\""],
        text=True,
    ).strip()
    env_map = dict(line.split("=", 1) for line in env_vars.splitlines() if "=" in line)
    team_id = env_map.get("TEAM_ID", "").strip()
    github_login = env_map.get("GITHUB_LOGIN", "").strip()
    if not team_id:
        sys.stderr.write("TEAM_ID not found. Run install.sh first.\n")
        return 2
    if not github_login:
        sys.stderr.write("GITHUB_LOGIN not found in ~/.claude/contest-collector.env. "
                         "Re-run install.sh with --github-login <your-name>.\n")
        return 2

    print(f"Found {len(transcripts)} transcript(s) in Claude Code history.")
    print(f"Destination: {dest}/logs/")
    print(f"GitHub login: {github_login}")
    print()

    child_env = {**subprocess.os.environ, "TEAM_ID": team_id,
                 "GITHUB_LOGIN": github_login, "CWD": str(dest)}

    success = 0
    skipped = 0
    for t in transcripts:
        sid = t.stem
        payload = json.dumps({
            "session_id": sid,
            "cwd": str(dest),
            "transcript_path": str(t),
            "hook_event_name": "SessionEnd",
        })
        try:
            result = subprocess.run(
                [sys.executable, str(core_py), "--tool", "claude-code"],
                input=payload, text=True, capture_output=True,
                env=child_env,
                cwd=str(dest),
            )
            if result.returncode == 0:
                if "captured" in result.stderr:
                    success += 1
                    print(f"  ✅ {sid[:12]}  {result.stderr.strip().split('-> ')[-1] if '-> ' in result.stderr else 'ok'}")
                else:
                    skipped += 1
            else:
                print(f"  ⚠️  {sid[:12]}  {result.stderr.strip()[:80]}")
        except Exception as e:
            print(f"  ❌ {sid[:12]}  {e}")

    print(f"\nBackfill done: {success} imported, {skipped} skipped (already captured or empty).")
    if success > 0:
        print("Next: git add logs/ && git commit -s -m 'logs: backfill history' && git push")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(
        description="Export AI Coding sessions from staging to demo repo logs/.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--list", action="store_true",
                   help="List all sessions in staging without exporting.")
    p.add_argument("--latest", action="store_true",
                   help="Export only the most recent session.")
    p.add_argument("--session", metavar="SID",
                   help="Export a specific session by id.")
    p.add_argument("--today", action="store_true",
                   help="Export sessions from today (UTC).")
    p.add_argument("--since", metavar="DATE",
                   help="Export sessions since a date (YYYY-MM-DD).")
    p.add_argument("--all", action="store_true",
                   help="Export every session in staging.")
    p.add_argument("--backfill", action="store_true",
                   help="Scan Claude Code history (~/.claude/projects/) and "
                        "re-process all transcripts. Recovers sessions that "
                        "were created before the hook was installed.")
    p.add_argument("--force", action="store_true",
                   help="With --backfill, wipe the local staging first so all "
                        "transcripts are re-processed even if they were "
                        "captured before. Useful after cleaning logs/ to "
                        "rebuild from scratch.")
    p.add_argument("--github-login", metavar="LOGIN",
                   help="Restrict to one member's sessions.")
    p.add_argument("--dest", metavar="PATH",
                   help="Destination repo path. Defaults to git rev-parse --show-toplevel.")
    p.add_argument("--dry-run", action="store_true",
                   help="Force preview mode, do not write.")
    p.add_argument("--confirm", action="store_true",
                   help="Required to actually export. Without it, the tool only previews.")
    args = p.parse_args()

    sessions = list_sessions(args.github_login)

    if args.list:
        return cmd_list(args, sessions)

    if args.backfill:
        return cmd_backfill(args)

    if not any([args.latest, args.session, args.today, args.since, args.all]):
        p.print_help()
        sys.stderr.write("\nNothing to do. Pick one of "
                         "--latest, --session, --today, --since, --all, --list, --backfill.\n")
        return 2

    selected = filter_sessions(sessions, args)
    if not selected:
        sys.stderr.write("No sessions matched.\n")
        return 1

    dest = Path(args.dest) if args.dest else repo_root()
    if dest is None:
        sys.stderr.write("Cannot detect demo repo. "
                         "Run inside the repo or pass --dest.\n")
        return 2

    preview_only = args.dry_run or not args.confirm
    rc = 0
    for s in selected:
        ok, msg = copy_session(s, dest, preview_only)
        prefix = "✅" if ok else "❌"
        print(f"{prefix} {s['session_id'][:12]}  {msg}")
        if not ok:
            rc = 1

    if preview_only:
        print(f"\n[preview] {len(selected)} session(s) shown above. "
              "Nothing was written.")
        print("Re-run with --confirm to actually export, e.g.:")
        if args.latest:
            print("  python3 tools/export-session.py --latest --confirm")
        elif args.session:
            print(f"  python3 tools/export-session.py --session {args.session} --confirm")
        elif args.today:
            print("  python3 tools/export-session.py --today --confirm")
        elif args.since:
            print(f"  python3 tools/export-session.py --since {args.since} --confirm")
        elif args.all:
            print("  python3 tools/export-session.py --all --confirm")
    else:
        print(f"\n{len(selected)} session(s) exported to {dest}/logs/")
        print("Next: git add logs/ && git commit && git push")

    return rc


if __name__ == "__main__":
    sys.exit(main())
