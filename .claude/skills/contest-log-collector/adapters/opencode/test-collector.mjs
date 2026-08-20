// Smoke test for adapters/opencode/collector.js
// Strategy: import the plugin's default export, build a mock OpenCode `client`,
// invoke the event handler with a fake session.idle event, then validate the
// resulting JSONL + manifest against the schema using the python validator.
//
// Run from repo root:
//   TEAM_ID=team-099-test node adapters/opencode/test-collector.mjs

import { mkdtempSync, rmSync, readFileSync, existsSync, writeFileSync } from "fs";
import { tmpdir } from "os";
import { join } from "path";
import { execFileSync } from "child_process";

import collectorModule from "./collector.js";

const tmpRepo = mkdtempSync(join(tmpdir(), "contest-collector-test-"));
process.chdir(tmpRepo);
execFileSync("git", ["init", "-q", "-b", "main"], { cwd: tmpRepo });
execFileSync("git", ["config", "user.email", "test@local"], { cwd: tmpRepo });
execFileSync("git", ["config", "user.name", "test"], { cwd: tmpRepo });
writeFileSync(join(tmpRepo, "README.md"), "test\n");
execFileSync("git", ["add", "."], { cwd: tmpRepo });
execFileSync("git", ["commit", "-q", "-m", "init"], { cwd: tmpRepo });

process.env.TEAM_ID = process.env.TEAM_ID || "team-099-test";
process.env.LOG_PUSH_INTERVAL = "9999";

const FAKE_SESSION = {
  id: "ses_smoketest_001",
  title: "Smoke test session",
  directory: tmpRepo,
  time_created: Date.now(),
  model: "test-model",
};

const FAKE_MESSAGES = [
  {
    info: { role: "user", time_created: Date.now() },
    parts: [{ type: "text", text: "Please review my token sk-1234567890abcdefghij1234567890" }],
  },
  {
    info: { role: "assistant", time_created: Date.now() + 1000, model: "test-model" },
    parts: [
      { type: "reasoning", text: "User pasted an API key, I should warn them." },
      { type: "text", text: "You posted an API key, I have internally redacted it." },
      {
        type: "tool",
        toolName: "read",
        callID: "call_001",
        args: { file: "src/foo.c" },
        result: { content: "int main(){}", lines: 1 },
      },
    ],
  },
];

const fakeClient = {
  session: {
    get: async ({ path }) => {
      if (path.id !== FAKE_SESSION.id) throw new Error("unknown session");
      return FAKE_SESSION;
    },
    messages: async ({ path }) => {
      if (path.id !== FAKE_SESSION.id) throw new Error("unknown session");
      return FAKE_MESSAGES;
    },
    list: async () => [FAKE_SESSION],
  },
};

const hooks = await collectorModule.server({
  client: fakeClient,
  directory: tmpRepo,
  worktree: tmpRepo,
});

await hooks.event({
  event: { type: "session.idle", properties: { sessionID: FAKE_SESSION.id } },
});

const githubLogin = process.env.GITHUB_LOGIN || "testuser";
const logsDir = join(tmpRepo, "logs");
const memberDir = join(logsDir, githubLogin);
if (!existsSync(memberDir)) {
  console.error(`FAIL: logs/${githubLogin}/ not created`);
  rmSync(tmpRepo, { recursive: true, force: true });
  process.exit(1);
}

const manifestPath = join(memberDir, "manifest.json");
const manifest = JSON.parse(readFileSync(manifestPath, "utf8"));
console.log("manifest.team_id =", manifest.team_id);
console.log("manifest.github_login =", manifest.github_login);
console.log("manifest.sessions.length =", manifest.sessions.length);
const session = manifest.sessions[0];
console.log("session.event_count =", session.event_count);
console.log("session.file_path =", session.file_path);
console.log("session.collection_mode =", session.collection_mode);
console.log("session.health =", session.health);

const jsonlPath = join(tmpRepo, session.file_path);
const jsonlContent = readFileSync(jsonlPath, "utf8");
const lines = jsonlContent.trim().split("\n");
console.log(`\njsonl has ${lines.length} lines:`);
for (const line of lines) {
  const ev = JSON.parse(line);
  const summary = `seq=${ev.seq} role=${ev.role}`;
  const detail =
    ev.role === "tool"
      ? `tool=${ev.tool_name}`
      : ev.thinking
        ? `thinking[${ev.thinking.length}c]`
        : ev.text
          ? `text[${ev.text.length}c]${ev.redacted_count ? ` REDACTED×${ev.redacted_count}` : ""}`
          : "";
  console.log(`  ${summary} ${detail}`);
}

const repoRoot = "/home/mi/contest-log-upload";
const validator = join(repoRoot, "tools", "validate-log.py");
console.log("\n=== running validator ===");
try {
  const out = execFileSync("python3", [validator, memberDir], {
    encoding: "utf8",
    cwd: repoRoot,
  });
  console.log(out);
  rmSync(tmpRepo, { recursive: true, force: true });
  console.log("✅ end-to-end test PASSED");
} catch (e) {
  console.error("validator output:", e.stdout || "");
  console.error("validator error: ", e.stderr || e.message);
  rmSync(tmpRepo, { recursive: true, force: true });
  process.exit(1);
}
