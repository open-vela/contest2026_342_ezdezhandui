// SPDX-License-Identifier: Apache-2.0
// openvela AI Contest - OpenCode session log collector
//
// Adapted from ~/.config/opencode/plugins/auto_snapshot.js.
// See openspec/changes/contest-log-upload/tasks.md Group 2 (16 items) for changes.
// Deploy location: <demo-repo>/.opencode/plugins/collector.js
// Required env: TEAM_ID
// Optional env: SESSION_LOG_DIR

import {
  readFileSync,
  writeFileSync,
  appendFileSync,
  mkdirSync,
  readdirSync,
  existsSync,
} from "fs";
import { join, dirname, resolve } from "path";
import { platform, homedir } from "os";
import { execFileSync } from "child_process";
import { resolveGithubLogin } from "../shared/get_github_login.js";

const VERSION = "1.3.0";
const SCHEMA_VERSION = "1.0";
const TOOL_ID = "opencode";

const GIT_CMD = platform() === "win32" ? "git.exe" : "git";

// AIoT-IDE / VS Code does not inherit user shell exports when launching plugins;
// the plugin must actively load TEAM_ID/GITHUB_LOGIN from the .env file.
// Without this, hooks fire but log writes are refused due to missing TEAM_ID.
function loadDotenv(searchDir) {
  if (!searchDir) return null;
  const path = join(searchDir, ".env");
  if (!existsSync(path)) return null;
  try {
    const text = readFileSync(path, "utf8");
    for (const line of text.split("\n")) {
      const trimmed = line.trim();
      if (!trimmed || trimmed.startsWith("#")) continue;
      const m = trimmed.match(/^([A-Za-z_][A-Za-z0-9_]*)=(.*)$/);
      if (m && !process.env[m[1]]) {
        let val = m[2];
        if ((val.startsWith('"') && val.endsWith('"')) ||
            (val.startsWith("'") && val.endsWith("'"))) {
          val = val.slice(1, -1);
        }
        process.env[m[1]] = val;
      }
    }
    return path;
  } catch {
    return null;
  }
}

const DEFAULT_REDACT_RULES = [
  { pattern: /sk-[A-Za-z0-9_-]{20,}/g, replacement: "sk-***REDACTED***" },
  { pattern: /ghp_[A-Za-z0-9]{36}/g, replacement: "ghp_***REDACTED***" },
  { pattern: /Bearer\s+[A-Za-z0-9._\-+/=]+/g, replacement: "Bearer ***REDACTED***" },
];

function pad(n) {
  return String(n).padStart(2, "0");
}

function isoNow() {
  return new Date().toISOString();
}

function localDate() {
  const d = new Date();
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`;
}

function timestampFs() {
  const d = new Date();
  return `${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}_${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}`;
}

function gitExec(args, cwd, opts = {}) {
  try {
    const out = execFileSync(GIT_CMD, args, {
      cwd,
      encoding: "utf8",
      stdio: ["pipe", "pipe", "pipe"],
      ...opts,
    });
    return { code: 0, out: (out || "").trim(), err: "" };
  } catch (e) {
    return {
      code: e.status || 1,
      out: (e.stdout || "").trim(),
      err: (e.stderr || "").trim(),
    };
  }
}

function getRepoRoot() {
  const r = gitExec(["rev-parse", "--show-toplevel"], process.cwd());
  return r.code === 0 ? r.out : null;
}

// Privacy gate: only collect inside an openvela workspace, identified by a
// `.repo/` dir at the workspace root. Sessions outside it (personal projects)
// must never be collected. Returns the workspace root, or null if not inside.
function findOpenvelaWorkspaceRoot(startDir) {
  try {
    let cur = resolve(startDir || process.cwd());
    while (true) {
      if (existsSync(join(cur, ".repo"))) return cur;
      const parent = dirname(cur);
      if (parent === cur) return null;
      cur = parent;
    }
  } catch {
    return null;
  }
}

function getAuthorFromSshPubkey() {
  const sshDir = join(homedir(), ".ssh");
  try {
    const pubFiles = readdirSync(sshDir).filter((f) => f.endsWith(".pub"));
    for (const f of pubFiles) {
      const content = readFileSync(join(sshDir, f), "utf8").trim();
      const parts = content.split(/\s+/);
      const comment = parts[parts.length - 1] || "";
      if (comment.includes("@") && !comment.startsWith("ssh-")) {
        return { name: comment.split("@")[0], email: comment };
      }
    }
  } catch {}
  return null;
}

function getFallbackAuthor() {
  if (FALLBACK_EMAIL) {
    return { name: FALLBACK_EMAIL.split("@")[0], email: FALLBACK_EMAIL };
  }
  const ssh = getAuthorFromSshPubkey();
  if (ssh) return ssh;
  return { name: "contest-collector", email: "contest-collector@auto-commit.local" };
}

function loadRedactRules(logRoot) {
  const customPath = join(logRoot, "redact.json");
  if (!existsSync(customPath)) return DEFAULT_REDACT_RULES;
  try {
    const custom = JSON.parse(readFileSync(customPath, "utf8"));
    const compiled = custom.map((r) => ({
      pattern: new RegExp(r.pattern, r.flags || "g"),
      replacement: r.replacement || "***REDACTED***",
    }));
    return [...DEFAULT_REDACT_RULES, ...compiled];
  } catch {
    return DEFAULT_REDACT_RULES;
  }
}

function redactValue(value, rules, counter) {
  if (typeof value === "string") {
    let out = value;
    for (const r of rules) {
      out = out.replace(r.pattern, () => {
        counter.count += 1;
        return r.replacement;
      });
    }
    return out;
  }
  if (Array.isArray(value)) {
    return value.map((v) => redactValue(v, rules, counter));
  }
  if (value && typeof value === "object") {
    const out = {};
    for (const [k, v] of Object.entries(value)) {
      out[k] = redactValue(v, rules, counter);
    }
    return out;
  }
  return value;
}

function reportError(logRoot, kind, ctx, err) {
  try {
    mkdirSync(join(logRoot, "errors"), { recursive: true });
    const errFile = join(logRoot, "errors", `${timestampFs()}_${kind}.err`);
    const payload = {
      ts: isoNow(),
      kind,
      tool: TOOL_ID,
      team_id: process.env.TEAM_ID,
      version: VERSION,
      context: ctx,
      message: err && err.message ? err.message : String(err),
      stack: err && err.stack ? err.stack : null,
    };
    writeFileSync(errFile, JSON.stringify(payload, null, 2) + "\n", "utf8");
  } catch {
    // last-resort: only stderr remains
  }
  console.error(
    `[session-log] ERROR kind=${kind} tool=${TOOL_ID} team=${process.env.TEAM_ID || "?"}: ${
      err && err.message ? err.message : err
    }`
  );
}

function readManifest(memberDir, githubLogin) {
  const path = join(memberDir, "manifest.json");
  if (!existsSync(path)) {
    return {
      schema_version: SCHEMA_VERSION,
      team_id: process.env.TEAM_ID,
      github_login: githubLogin,
      generator: `${TOOL_ID}-collector@${VERSION}`,
      updated_at: isoNow(),
      sessions: [],
    };
  }
  try {
    const data = JSON.parse(readFileSync(path, "utf8"));
    if (data.team_id !== process.env.TEAM_ID) {
      reportError(memberDir, "manifest_team_mismatch", { path }, {
        message: `manifest.team_id=${data.team_id} but TEAM_ID env=${process.env.TEAM_ID}`,
      });
    }
    if (data.github_login && data.github_login !== githubLogin) {
      reportError(memberDir, "manifest_login_mismatch", { path }, {
        message: `manifest.github_login=${data.github_login} but detected=${githubLogin}`,
      });
    }
    return data;
  } catch (e) {
    reportError(memberDir, "manifest_read", { path }, e);
    return {
      schema_version: SCHEMA_VERSION,
      team_id: process.env.TEAM_ID,
      github_login: githubLogin,
      generator: `${TOOL_ID}-collector@${VERSION}`,
      updated_at: isoNow(),
      sessions: [],
    };
  }
}

function writeManifest(memberDir, manifest, githubLogin) {
  manifest.updated_at = isoNow();
  manifest.team_id = process.env.TEAM_ID;
  manifest.github_login = githubLogin;
  manifest.schema_version = SCHEMA_VERSION;
  manifest.generator = `${TOOL_ID}-collector@${VERSION}`;
  const path = join(memberDir, "manifest.json");
  try {
    writeFileSync(path, JSON.stringify(manifest, null, 2) + "\n", "utf8");
  } catch (e) {
    reportError(memberDir, "manifest_write", { path }, e);
  }
}

function findManifestEntry(manifest, sessionId) {
  return manifest.sessions.find((s) => s.session_id === sessionId);
}

function partsFromMessage(entry) {
  const parts = entry.parts || [];
  return Array.isArray(parts) ? parts : [];
}

// Flatten OpenCode SDK's message+parts structure into the contest event stream;
// one user/assistant message may contain multiple parts (text / reasoning / tool / etc.),
// we split each part into a separate JSONL line; role inherits from the parent message (except tool parts).
function flattenModel(modelField) {
  if (!modelField) return undefined;
  if (typeof modelField === "string") return modelField;
  if (typeof modelField === "object") {
    const provider = modelField.providerID || modelField.provider || "";
    const id = modelField.modelID || modelField.id || modelField.model || "";
    if (provider && id) return `${provider}/${id}`;
    return id || provider || undefined;
  }
  return undefined;
}

const SKIP_PART_TYPES = new Set(["step-start", "step-finish", "step", "patch"]);

function expandMessageToEvents(entry, ctx) {
  const out = [];
  const msg = entry.info || entry;
  const role = (msg.role || "unknown").toLowerCase();
  const baseTs =
    msg.time_created || msg.timeCreated || msg.createdAt || ctx.fallbackTs;
  const baseTsIso = baseTs
    ? new Date(typeof baseTs === "number" ? baseTs : baseTs).toISOString()
    : isoNow();
  const modelStr = flattenModel(msg.model);

  for (const part of partsFromMessage(entry)) {
    const type = part.type || part.kind || "text";

    if (SKIP_PART_TYPES.has(type)) continue;

    if (type === "text" && part.text) {
      out.push({
        role,
        ts: baseTsIso,
        text: part.text,
        ...(modelStr ? { model: modelStr } : {}),
        ...(typeof msg.tokens_in === "number" ? { tokens_in: msg.tokens_in } : {}),
        ...(typeof msg.tokens_out === "number" ? { tokens_out: msg.tokens_out } : {}),
      });
    } else if (type === "reasoning" || type === "thinking") {
      out.push({
        role,
        ts: baseTsIso,
        thinking: part.text || part.content || "",
        ...(modelStr ? { model: modelStr } : {}),
      });
    } else if (type === "tool" || type === "tool-invocation") {
      const toolName = part.toolName || part.tool || part.name || "unknown";
      const callId = part.callID || part.callId || part.tool_call_id || `call_${out.length}`;
      const input = part.args || part.state?.input || part.input || null;
      const output = part.result || part.state?.output || part.output || null;
      out.push({
        role: "tool",
        ts: baseTsIso,
        tool_name: toolName,
        tool_call_id: callId,
        input,
        output,
        ...(part.metadata ? { metadata: part.metadata } : {}),
      });
    } else {
      out.push({
        role,
        ts: baseTsIso,
        text: `[unsupported part type: ${type}]`,
        metadata: { dropped: true, original_type: type },
      });
    }
  }

  return out;
}

function appendEvents(jsonlPath, events, sessionId, githubLogin, startSeq, redactRules) {
  let seq = startSeq;
  let redactedTotal = 0;
  const lines = [];
  for (const ev of events) {
    const counter = { count: 0 };
    const redacted = redactValue(ev, redactRules, counter);
    redactedTotal += counter.count;
    const out = {
      schema_version: SCHEMA_VERSION,
      session_id: sessionId,
      team_id: process.env.TEAM_ID,
      github_login: githubLogin,
      tool: TOOL_ID,
      seq,
      ...redacted,
    };
    if (counter.count > 0) {
      out.redacted_count = counter.count;
    }
    lines.push(JSON.stringify(out));
    seq += 1;
  }
  if (lines.length === 0) return { written: 0, lastSeq: startSeq - 1, redacted: 0 };
  appendFileSync(jsonlPath, lines.join("\n") + "\n", "utf8");
  return { written: lines.length, lastSeq: seq - 1, redacted: redactedTotal };
}

async function fetchMessages(client, sessionId, logRoot) {
  try {
    const r = await client.session.messages({ path: { id: sessionId } });
    const arr = r?.data ?? r ?? [];
    return Array.isArray(arr) ? arr : [];
  } catch (e) {
    reportError(logRoot, "sdk_messages", { sessionId }, e);
    return null;
  }
}

async function fetchSessionMeta(client, sessionId, logRoot) {
  try {
    const r = await client.session.get({ path: { id: sessionId } });
    return r?.data ?? r ?? null;
  } catch (e) {
    reportError(logRoot, "sdk_session_get", { sessionId }, e);
    return null;
  }
}



async function onSessionIdle(client, repoRoot, sessionId) {
  if (!process.env.TEAM_ID) {
    console.error(
      "[session-log] FATAL: TEAM_ID env var not set, refusing to write log to avoid mis-attribution"
    );
    return;
  }
  if (!sessionId) {
    return;
  }

  const githubLoginResult = resolveGithubLogin(repoRoot);
  if (!githubLoginResult) {
    console.error(
      "[session-log] FATAL: cannot detect GITHUB_LOGIN. " +
      "Set GITHUB_LOGIN env, or run 'gh auth login', or fix git remote URL."
    );
    return;
  }
  const githubLogin = githubLoginResult.login;

  const baseDir = process.env.SESSION_LOG_DIR || join(repoRoot, "logs");
  const memberDir = join(baseDir, githubLogin);

  try {
    mkdirSync(memberDir, { recursive: true });
  } catch (e) {
    reportError(memberDir, "mkdir_logs", { memberDir }, e);
    return;
  }

  const dateDir = join(memberDir, localDate());
  try {
    mkdirSync(dateDir, { recursive: true });
  } catch (e) {
    reportError(memberDir, "mkdir_date", { dateDir }, e);
    return;
  }

  const jsonlPath = join(dateDir, `${TOOL_ID}__${sessionId}.jsonl`);
  const manifest = readManifest(memberDir, githubLogin);
  let entry = findManifestEntry(manifest, sessionId);
  const isNew = !entry;
  const rawWatermark = entry ? entry.raw_message_count || 0 : 0;
  const startSeq = entry ? entry.event_count || 0 : 0;

  const messages = await fetchMessages(client, sessionId, memberDir);
  if (messages === null) {
    return;
  }
  if (messages.length === 0) {
    return;
  }

  const newMessages = messages.slice(rawWatermark);
  if (newMessages.length === 0) {
    if (entry) {
      entry.raw_message_count = messages.length;
      writeManifest(memberDir, manifest, githubLogin);
    }
    return;
  }

  const sessionMeta = await fetchSessionMeta(client, sessionId, memberDir);
  const redactRules = loadRedactRules(memberDir);
  const events = newMessages.flatMap((entry) =>
    expandMessageToEvents(entry, { fallbackTs: Date.now() })
  );

  let writeResult;
  try {
    writeResult = appendEvents(jsonlPath, events, sessionId, githubLogin, startSeq, redactRules);
  } catch (e) {
    reportError(memberDir, "jsonl_append", { jsonlPath }, e);
    return;
  }

  const newEventCount = (entry?.event_count || 0) + writeResult.written;
  const relPath = `logs/${githubLogin}/${localDate()}/${TOOL_ID}__${sessionId}.jsonl`;
  if (isNew) {
    const startTimeRaw =
      sessionMeta?.time_created ||
      sessionMeta?.timeCreated ||
      sessionMeta?.createdAt ||
      sessionMeta?.created_at ||
      sessionMeta?.time?.created ||
      messages[0]?.info?.time_created ||
      messages[0]?.info?.timeCreated;
    const startedAt = startTimeRaw
      ? new Date(typeof startTimeRaw === "number" ? startTimeRaw : startTimeRaw).toISOString()
      : isoNow();
    entry = {
      session_id: sessionId,
      tool: TOOL_ID,
      started_at: startedAt,
      last_event_at: isoNow(),
      event_count: newEventCount,
      raw_message_count: messages.length,
      file_path: relPath,
      collection_mode: "cli",
      health: "ok",
    };
    if (sessionMeta?.title) entry.title = sessionMeta.title;
    const sessionModel = flattenModel(sessionMeta?.model);
    if (sessionModel) entry.model = sessionModel;
    manifest.sessions.push(entry);
  } else {
    entry.last_event_at = isoNow();
    entry.event_count = newEventCount;
    entry.raw_message_count = messages.length;
    entry.file_path = relPath;
    if (writeResult.redacted > 0) {
      entry.redacted_count_total =
        (entry.redacted_count_total || 0) + writeResult.redacted;
    }
  }

  writeManifest(memberDir, manifest, githubLogin);

  console.error(
    `[session-log] captured ${writeResult.written} event(s) -> ${relPath} ` +
    `(remember to 'git add logs/' when committing)`
  );
}

export default {
  id: "contest-log-collector",
  server({ client, directory, worktree }) {
    const baseDir = worktree || directory || process.cwd();
    const loadedFrom = loadDotenv(baseDir);
    if (loadedFrom) {
      console.error(`[session-log] loaded .env from ${loadedFrom}`);
    }

    const teamId = process.env.TEAM_ID || "";
    if (!teamId) {
      console.error(
        "[session-log] FATAL: TEAM_ID not set (checked env + .env). " +
        "Add TEAM_ID=team-XXX-yourname to .env in repo root."
      );
    } else {
      console.error(
        `[session-log] tool=${TOOL_ID} team=${teamId} version=${VERSION}`
      );
    }

    const cwdRepoRoot = getRepoRoot();
    // Use path.resolve for normalized comparison, fixing the bug at auto_snapshot.js L231
    // where Windows case/slash mismatch caused match failures.
    const repoRoot =
      cwdRepoRoot && (worktree || directory)
        ? resolve(cwdRepoRoot) === resolve(worktree || directory)
          ? cwdRepoRoot
          : worktree || directory
        : cwdRepoRoot || worktree || directory;

    if (!findOpenvelaWorkspaceRoot(repoRoot || baseDir)) {
      console.error(
        "[session-log] not inside an openvela workspace (no .repo/ found); " +
        "collection disabled for this session."
      );
      return {};
    }

    return {
      event: async ({ event }) => {
        if (event.type !== "session.idle") return;
        const sessionId = event.properties?.sessionID || event.properties?.sessionId || "";
        try {
          await onSessionIdle(client, repoRoot, sessionId);
        } catch (e) {
          const baseDir = process.env.SESSION_LOG_DIR || join(repoRoot, "logs");
          reportError(baseDir, "uncaught_handler", { sessionId }, e);
        }
      },
    };
  },
};
