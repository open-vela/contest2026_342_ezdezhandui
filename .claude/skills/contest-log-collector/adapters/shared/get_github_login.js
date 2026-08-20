// SPDX-License-Identifier: Apache-2.0
// Detect current user's GitHub username via 5-step priority chain.
// Used by OpenCode collector; Claude Code/Codex use the equivalent .py version.
//
// Public API: resolveGithubLogin(repoRoot) → { login, source } | null
// See get_github_login.py header comment for the priority chain (must stay consistent).

import { execFileSync } from "node:child_process";
import { existsSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { homedir } from "node:os";

const GITHUB_LOGIN_RE = /^[A-Za-z0-9][A-Za-z0-9-]{0,38}$/;

function fromEnv() {
  const val = (process.env.GITHUB_LOGIN || "").trim();
  if (val && GITHUB_LOGIN_RE.test(val)) return { login: val, source: "env" };
  return null;
}

function fromGhHosts() {
  const candidates = [
    join(homedir(), ".config", "gh", "hosts.yml"),
    join(process.env.APPDATA || "", "GitHub CLI", "hosts.yml"),
  ];
  for (const path of candidates) {
    if (!path || !existsSync(path)) continue;
    try {
      const txt = readFileSync(path, "utf8");
      const m = txt.match(/^\s{4}user:\s*(\S+)\s*$/m);
      if (m && GITHUB_LOGIN_RE.test(m[1])) return { login: m[1], source: "gh-hosts" };
    } catch {}
  }
  return null;
}

function fromGhApi() {
  try {
    const out = execFileSync("gh", ["api", "user", "--jq", ".login"], {
      encoding: "utf8", stdio: ["ignore", "pipe", "ignore"], timeout: 3000,
    }).trim();
    if (out && GITHUB_LOGIN_RE.test(out)) return { login: out, source: "gh-api" };
  } catch {}
  return null;
}

function gitRemoteUrl(repoRoot) {
  try {
    return execFileSync("git", ["config", "--get", "remote.origin.url"], {
      cwd: repoRoot, encoding: "utf8", stdio: ["ignore", "pipe", "ignore"], timeout: 3000,
    }).trim() || null;
  } catch { return null; }
}

function fromRemoteBasicAuth(repoRoot) {
  const url = gitRemoteUrl(repoRoot);
  if (!url) return null;
  const m = url.match(/^https?:\/\/([^@:/]+)@github\.com\//);
  if (m && GITHUB_LOGIN_RE.test(m[1])) return { login: m[1], source: "remote-basic-auth" };
  return null;
}

function fromRemoteOwner(repoRoot) {
  const url = gitRemoteUrl(repoRoot);
  if (!url) return null;
  const patterns = [
    /^git@github\.com:([^/]+)\//,
    /^https?:\/\/(?:[^@]+@)?github\.com\/([^/]+)\//,
  ];
  for (const pat of patterns) {
    const m = url.match(pat);
    if (m && GITHUB_LOGIN_RE.test(m[1])) return { login: m[1], source: "remote-owner" };
  }
  return null;
}

function fromNoreplyEmail(repoRoot) {
  try {
    const email = execFileSync("git", ["config", "user.email"], {
      cwd: repoRoot, encoding: "utf8", stdio: ["ignore", "pipe", "ignore"], timeout: 3000,
    }).trim();
    const m = email.match(/^(?:\d+\+)?([^@]+)@users\.noreply\.github\.com$/);
    if (m && GITHUB_LOGIN_RE.test(m[1])) return { login: m[1], source: "noreply-email" };
  } catch {}
  return null;
}

export function resolveGithubLogin(repoRoot) {
  const candidates = [
    fromEnv,
    fromGhHosts,
    fromGhApi,
    () => fromRemoteBasicAuth(repoRoot),
    () => fromRemoteOwner(repoRoot),
    () => fromNoreplyEmail(repoRoot),
  ];
  for (const fn of candidates) {
    const r = fn();
    if (r) return r;
  }
  return null;
}
