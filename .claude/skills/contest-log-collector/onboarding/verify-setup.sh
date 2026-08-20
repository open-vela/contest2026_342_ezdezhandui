#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# openvela AI Contest - collector install verification script
# Run from anywhere after install.sh finishes. Verifies the machine-wide
# install (no per-repo files are expected by design).

set -u

if ! command -v git >/dev/null; then
  echo "❌ git not found"; exit 1
fi
if ! command -v python3 >/dev/null; then
  echo "❌ python3 not found"; exit 1
fi
if ! command -v node >/dev/null; then
  echo "⚠️  node not found (only needed if using OpenCode)"
fi

PASS=0
FAIL=0

check_file() {
  local path="$1"
  local desc="$2"
  if [ -f "$path" ]; then
    echo "✅ $desc: $path"
    PASS=$((PASS+1))
  else
    echo "❌ $desc MISSING: $path"
    FAIL=$((FAIL+1))
  fi
}

check_executable() {
  local path="$1"
  local desc="$2"
  if [ -f "$path" ] && [ -x "$path" ]; then
    echo "✅ $desc: $path (executable)"
    PASS=$((PASS+1))
  elif [ -f "$path" ]; then
    echo "⚠️  $desc: $path (not executable, run: chmod +x $path)"
    FAIL=$((FAIL+1))
  else
    echo "❌ $desc MISSING: $path"
    FAIL=$((FAIL+1))
  fi
}

echo "─── global hook (machine-wide) ───"
check_executable "$HOME/.claude/contest-shared/contest-snapshot.sh" "Global hook script"
check_file "$HOME/.claude/contest-shared/snapshot_core.py" "Global snapshot core"
check_file "$HOME/.claude/contest-shared/get_github_login.py" "GitHub login detector"
if [ -f "$HOME/.claude/settings.json" ] \
   && grep -q "contest-shared/contest-snapshot.sh" "$HOME/.claude/settings.json"; then
  echo "✅ ~/.claude/settings.json registers global Stop+SessionEnd hook"
  PASS=$((PASS+1))
else
  echo "❌ ~/.claude/settings.json does NOT register the global hook"
  echo "   Re-run install.sh to fix."
  FAIL=$((FAIL+1))
fi

echo ""
echo "─── opencode global plugin ───"
check_file "$HOME/.config/opencode/plugin/contest-collector.js" "OpenCode global plugin"

echo ""
echo "─── contest-snapshot shortcut ───"
check_executable "$HOME/.local/bin/contest-snapshot" "Shortcut script"
case ":$PATH:" in
  *":$HOME/.local/bin:"*)
    echo "✅ ~/.local/bin is on PATH"
    PASS=$((PASS+1))
    ;;
  *)
    echo "⚠️  ~/.local/bin is NOT on PATH; use full python3 path instead, or append it:"
    echo "      echo 'export PATH=\"\$HOME/.local/bin:\$PATH\"' >> ~/.bashrc"
    ;;
esac

echo ""
echo "─── identity (~/.claude/contest-collector.env) ───"
GLOBAL_ENV="$HOME/.claude/contest-collector.env"
if [ -f "$GLOBAL_ENV" ]; then
  ENV_TEAM=$(grep -E '^TEAM_ID=' "$GLOBAL_ENV" | head -1 | cut -d= -f2-)
  ENV_LOGIN=$(grep -E '^GITHUB_LOGIN=' "$GLOBAL_ENV" | head -1 | cut -d= -f2-)
  if [ -n "$ENV_TEAM" ] && echo "$ENV_TEAM" | grep -qE '^[a-zA-Z][a-zA-Z0-9_-]+$'; then
    echo "✅ TEAM_ID: $ENV_TEAM"
    PASS=$((PASS+1))
  else
    echo "❌ TEAM_ID missing or malformed in $GLOBAL_ENV"
    FAIL=$((FAIL+1))
  fi
  if [ -n "$ENV_LOGIN" ] && echo "$ENV_LOGIN" | grep -qE '^[A-Za-z0-9][A-Za-z0-9-]{0,38}$'; then
    echo "✅ GITHUB_LOGIN: $ENV_LOGIN"
    PASS=$((PASS+1))
  else
    echo "❌ GITHUB_LOGIN missing or malformed in $GLOBAL_ENV"
    FAIL=$((FAIL+1))
  fi
else
  echo "❌ $GLOBAL_ENV does not exist - re-run install.sh"
  FAIL=$((FAIL+1))
fi

echo ""
echo "─── staging directory ───"
STAGING="$HOME/.claude/contest-collector-staging"
if [ -d "$STAGING" ]; then
  echo "✅ staging dir: $STAGING"
  PASS=$((PASS+1))
  COUNT=$(find "$STAGING" -name "*.jsonl" -type f 2>/dev/null | wc -l)
  if [ "$COUNT" -gt 0 ]; then
    echo "   currently holds $COUNT session(s)"
  fi
else
  echo "⚠️  staging dir does not exist yet (will be created on first AI session)"
fi

echo ""
echo "─── jsonschema (required by validate-log.py) ───"
if python3 -c "import jsonschema" 2>/dev/null; then
  echo "✅ jsonschema available"
  PASS=$((PASS+1))
else
  echo "❌ jsonschema not installed; run: pip3 install --user jsonschema"
  FAIL=$((FAIL+1))
fi

echo ""
echo "─── non-whitelisted VS Code AI extensions ───"
WARN_EXTS=()
for ext_dir in ~/.vscode/extensions ~/.vscode-server/extensions; do
  [ -d "$ext_dir" ] || continue
  if find "$ext_dir" -maxdepth 1 -name "saoudrizwan.claude-dev*" 2>/dev/null | grep -q .; then
    WARN_EXTS+=("Cline (saoudrizwan.claude-dev)")
  fi
  if find "$ext_dir" -maxdepth 1 -name "rooveterinaryinc.roo-cline*" 2>/dev/null | grep -q .; then
    WARN_EXTS+=("Roo Code")
  fi
  if find "$ext_dir" -maxdepth 1 -name "github.copilot-chat*" 2>/dev/null | grep -q .; then
    WARN_EXTS+=("GitHub Copilot Chat")
  fi
done

if [ ${#WARN_EXTS[@]} -gt 0 ]; then
  echo "⚠️  Detected non-whitelisted VS Code AI extensions:"
  for ext in "${WARN_EXTS[@]}"; do
    echo "      - $ext"
  done
  echo "   Only OpenCode / Claude Code (CLI) / Codex / Kiro are supported."
  echo "   Sessions from these extensions WILL NOT be collected; may impact scoring."
fi

echo ""
echo "─── summary ───"
echo "Passed: $PASS"
echo "Failed: $FAIL"

if [ "$FAIL" -gt 0 ]; then
  exit 1
fi
echo ""
echo "✅ All checks passed."
echo "   Demo repo is intentionally untouched - no per-repo files installed."
echo "   Sessions inside the openvela workspace are auto-written into logs/"
echo "   when they end; just commit them with your code:"
echo "     git add logs/ && git commit -s -m 'logs: capture session' && git push"
echo "   (Optional) list or re-export manually:"
echo "     contest-snapshot --list                                                # short form"
echo "     python3 ../.claude/skills/contest-log-collector/tools/export-session.py --list   # full path fallback"
