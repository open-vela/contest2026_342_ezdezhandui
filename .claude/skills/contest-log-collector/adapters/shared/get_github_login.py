#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Detect the current user's GitHub username via a 5-step reliability priority chain.
# Used by Claude Code / Codex collectors (called by snapshot_core.py).
#
# Public API:
#   resolve_github_login(repo_root) -> (login, source) | (None, None)
#
# Priority:
#   0. GITHUB_LOGIN env (explicit override, highest priority)
#   1. ~/.config/gh/hosts.yml active user (offline + authoritative)
#   2. gh api user --jq .login (network + authoritative)
#   3. basic auth user in git remote URL (strong signal)
#   4. git config user.email noreply pattern (last resort)
#
# Deliberately NOT used: git remote owner. The owner of a fork can be an
# organization (e.g. iotpi/foo, open-vela/foo), which is NOT the individual
# contributor's github login and would mis-attribute logs.
#
# Returns (None, None) on failure to let caller fallback.

from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

GITHUB_LOGIN_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9-]{0,38}$")


def _from_env() -> tuple[str | None, str | None]:
    val = os.environ.get("GITHUB_LOGIN", "").strip()
    if val and GITHUB_LOGIN_RE.match(val):
        return (val, "env")
    return (None, None)


def _from_gh_hosts() -> tuple[str | None, str | None]:
    candidates = [
        Path.home() / ".config" / "gh" / "hosts.yml",
        Path(os.environ.get("APPDATA", "")) / "GitHub CLI" / "hosts.yml",
    ]
    for path in candidates:
        if not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8")
            m = re.search(r"^\s{4}user:\s*(\S+)\s*$", text, re.MULTILINE)
            if m:
                login = m.group(1)
                if GITHUB_LOGIN_RE.match(login):
                    return (login, "gh-hosts")
        except Exception:
            continue
    return (None, None)


def _from_gh_api() -> tuple[str | None, str | None]:
    try:
        r = subprocess.run(
            ["gh", "api", "user", "--jq", ".login"],
            capture_output=True, text=True, timeout=3,
        )
        if r.returncode == 0:
            login = r.stdout.strip()
            if login and GITHUB_LOGIN_RE.match(login):
                return (login, "gh-api")
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass
    return (None, None)


def _git_remote_url(repo_root: Path) -> str | None:
    try:
        r = subprocess.run(
            ["git", "config", "--get", "remote.origin.url"],
            cwd=str(repo_root), capture_output=True, text=True, timeout=3,
        )
        if r.returncode == 0:
            return r.stdout.strip() or None
    except Exception:
        pass
    return None


def _from_remote_basic_auth(repo_root: Path) -> tuple[str | None, str | None]:
    url = _git_remote_url(repo_root)
    if not url:
        return (None, None)
    m = re.match(r"^https?://([^@:/]+)@github\.com/", url)
    if m:
        login = m.group(1)
        if GITHUB_LOGIN_RE.match(login):
            return (login, "remote-basic-auth")
    return (None, None)


def _from_noreply_email(repo_root: Path) -> tuple[str | None, str | None]:
    try:
        r = subprocess.run(
            ["git", "config", "user.email"],
            cwd=str(repo_root), capture_output=True, text=True, timeout=3,
        )
        if r.returncode != 0:
            return (None, None)
        email = r.stdout.strip()
        m = re.match(r"^(?:\d+\+)?([^@]+)@users\.noreply\.github\.com$", email)
        if m:
            login = m.group(1)
            if GITHUB_LOGIN_RE.match(login):
                return (login, "noreply-email")
    except Exception:
        pass
    return (None, None)


def resolve_github_login(repo_root: Path) -> tuple[str | None, str | None]:
    for fn in (
        _from_env,
        _from_gh_hosts,
        _from_gh_api,
        lambda: _from_remote_basic_auth(repo_root),
        lambda: _from_noreply_email(repo_root),
    ):
        login, source = fn()
        if login:
            return (login, source)
    return (None, None)


if __name__ == "__main__":
    import sys
    target = Path(sys.argv[1]) if len(sys.argv) > 1 else Path.cwd()
    login, source = resolve_github_login(target)
    if login:
        print(f"{login}\t{source}")
        sys.exit(0)
    print("ERROR: cannot detect GitHub login. Set GITHUB_LOGIN env var.", file=sys.stderr)
    sys.exit(1)
