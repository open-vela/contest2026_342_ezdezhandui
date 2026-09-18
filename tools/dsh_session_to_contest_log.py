#!/usr/bin/env python3
"""dsh_session_to_contest_log.py —— 把 DSH 会话日志导出为大赛 AI Coding 日志

背景
    大赛的 `contest-log-collector` 只覆盖 Claude Code / OpenCode / Codex 三种工具，
    本项目实际使用的是 **DSH（DeepSeek Harness）**，其会话日志在
    `$DSH_HOME/sessions/<工作区 mangled>/session-<id>/session.jsonl[.zstd]`，
    与赛事的事件格式不同，需要转换。

事件筛选
    DSH 日志里 66k 行大多是**流式增量**（reasoning-chunks / assistant/chunk /
    tool-call-chunks / text-chunks）。这些必须丢弃 —— 聚合后的 `assistant/message`
    已经包含同样内容，保留增量会让日志体积翻十几倍且难以阅读。

    保留：user/message、assistant/message、tool/call、tool/result

tool 字段
    赛事 schema 的 `tool` 是**枚举**：opencode / claude-code / codex / kiro，
    **不含 dsh**。默认写 "dsh"（诚实），可用 --tool 覆盖；
    真实来源另记在每条事件的 metadata.source_tool 里。

用法
    tools/dsh_session_to_contest_log.py --session <session-id> [--tool dsh]
    tools/dsh_session_to_contest_log.py --session <id> --workdir <工作区> --confirm
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from datetime import datetime, timezone

DSH_HOME = os.environ.get("DSH_HOME") or os.path.expanduser("~/.dsh")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TEAM_ID = "contest2026-342"
GITHUB_LOGIN = "ez-xu"

# 需要保留的事件类型 -> (role, 处理方式)
KEEP = {
    "user/message": "user",
    "assistant/message": "assistant",
    "tool/call": "tool",
    "tool/result": "tool",
}

# 默认脱敏（与赛事 collector 一致，另加 DSH 常见项）
REDACT = [
    (re.compile(r"tp-[A-Za-z0-9]{20,}"), "tp-<REDACTED>"),  # 小米 MiMo Token Plan key（曾两次泄漏）
    (re.compile(r"sk-[A-Za-z0-9_-]{16,}"), "sk-<REDACTED>"),
    (re.compile(r"ghp_[A-Za-z0-9]{20,}"), "ghp_<REDACTED>"),
    (re.compile(r"gho_[A-Za-z0-9]{20,}"), "gho_<REDACTED>"),
    (re.compile(r"(?i)bearer\s+[A-Za-z0-9._-]{16,}"), "Bearer <REDACTED>"),
]


def redact(text):
    n = 0
    for pat, rep in REDACT:
        text, k = pat.subn(rep, text)
        n += k
    return text, n


def find_session(session_id, workdir=None):
    """在 DSH_HOME 下按 id 找会话目录。

    兼容两种目录命名：新格式 `session-<id>` 与旧格式裸 `<id>`。
    """
    base = os.path.join(DSH_HOME, "sessions")
    for mangled in os.listdir(base):
        if workdir and workdir not in mangled:
            continue
        for name in (f"session-{session_id}", session_id):
            d = os.path.join(base, mangled, name)
            if not os.path.isdir(d):
                continue
            for fn in ("session.jsonl", "session.jsonl.zstd"):
                p = os.path.join(d, fn)
                if os.path.isfile(p):
                    return p
    return None


def open_log(path):
    if path.endswith(".zstd"):
        p = subprocess.run(["zstd", "-dc", path], capture_output=True)
        if p.returncode != 0:
            raise SystemExit("zstd 解压失败: " + p.stderr.decode()[:200])
        return p.stdout.decode("utf-8", "replace").splitlines()
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read().splitlines()


def text_of(msg):
    """从 DSH message.content 提取纯文本。"""
    if not isinstance(msg, dict):
        return ""
    out = []
    for part in msg.get("content") or []:
        if isinstance(part, dict):
            t = part.get("text")
            if isinstance(t, str):
                out.append(t)
    return "\n".join(out).strip()


def to_events(raw_lines, session_id, tool):
    events = []
    seq = 0
    redacted_total = 0
    # callId -> 工具名。tool/result 自身**不带**工具名，必须回查前面的 tool/call，
    # 否则 role=tool 的事件缺 tool_name，schema 校验会失败。
    call_names = {}

    for line in raw_lines:
        line = line.strip()
        if not line:
            continue
        try:
            rec = json.loads(line)
        except Exception:
            continue

        etype = rec.get("type")
        role = KEEP.get(etype)
        if role is None:
            continue

        data = rec.get("data") or {}
        ts_ms = rec.get("time")
        ts = (datetime.fromtimestamp(ts_ms / 1000.0, tz=timezone.utc)
              .isoformat().replace("+00:00", "Z")) if ts_ms else None

        ev = {
            "schema_version": "1.0",
            "session_id": session_id,
            "team_id": TEAM_ID,
            "github_login": GITHUB_LOGIN,
            "tool": tool,
            "ts": ts,
            "role": role,
            "seq": seq,
            "metadata": {"source_tool": "dsh", "source_event": etype},
        }

        if etype in ("user/message", "assistant/message"):
            txt = text_of(data.get("message") or data)
            txt, n = redact(txt)
            redacted_total += n
            if txt:
                ev["text"] = txt
            m = (data.get("message") or {}).get("model")
            if m:
                ev["model"] = m

        elif etype == "tool/call":
            cid = data.get("callId")
            name = data.get("name")
            if not cid or not name:
                continue                      # role=tool 缺这两个字段不合法
            call_names[cid] = name
            ev["tool_name"] = name
            ev["tool_call_id"] = cid
            args = data.get("arguments")
            if isinstance(args, str):
                a, n = redact(args)
                redacted_total += n
                try:
                    args = json.loads(a)
                except Exception:
                    args = a
            ev["input"] = args

        elif etype == "tool/result":
            msg = data.get("message") or {}
            cid = ((msg.get("source") or {}).get("callId"))
            name = call_names.get(cid)
            if not cid or not name:
                continue                      # 找不到对应 call -> 不合法，跳过
            out = msg.get("content", data.get("output"))
            if isinstance(out, str):
                out, n = redact(out)
                redacted_total += n
            elif out is not None:
                txt, n = redact(json.dumps(out, ensure_ascii=False))
                redacted_total += n
                out = txt[:20000]
            ev["output"] = out
            ev["tool_name"] = name
            ev["tool_call_id"] = cid

        if redacted_total:
            ev["redacted_count"] = redacted_total

        events.append(ev)
        seq += 1

    return events


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--session", required=True, help="DSH session id")
    ap.add_argument("--workdir", default=None,
                    help="限定工作区关键字（如 openvela）")
    ap.add_argument("--tool", default="dsh",
                    help="写入事件的 tool 字段（赛事枚举不含 dsh，默认如实写 dsh）")
    ap.add_argument("--date", default=None, help="YYYY-MM-DD，默认取事件首条时间")
    ap.add_argument("--confirm", action="store_true", help="真正写入")
    args = ap.parse_args()

    path = find_session(args.session, args.workdir)
    if not path:
        raise SystemExit(f"找不到会话 {args.session}（DSH_HOME={DSH_HOME}）")
    print(f"源   : {path}")

    raw = open_log(path)
    print(f"原始 : {len(raw)} 行")
    events = to_events(raw, args.session, args.tool)
    print(f"提取 : {len(events)} 条事件（已丢弃流式增量）")

    if not events:
        raise SystemExit("没有可导出的事件")

    date = args.date or (events[0]["ts"] or "")[:10] or \
        datetime.now(timezone.utc).strftime("%Y-%m-%d")

    outdir = os.path.join(REPO, "logs", GITHUB_LOGIN, date)
    outfile = os.path.join(outdir, f"{args.tool}__{args.session}.jsonl")

    print(f"目标 : {os.path.relpath(outfile, REPO)}")

    if not args.confirm:
        print("\n这是预览（加 --confirm 写入）。前 2 条示例：")
        for e in events[:2]:
            print("  " + json.dumps(e, ensure_ascii=False)[:240])
        return 0

    os.makedirs(outdir, exist_ok=True)
    with open(outfile, "w", encoding="utf-8") as fh:
        for e in events:
            fh.write(json.dumps(e, ensure_ascii=False) + "\n")

    size = os.path.getsize(outfile)
    print(f"✅ 已写入 {len(events)} 条，{size/1024:.0f} KB")

    # --- 更新 manifest.json ---
    mpath = os.path.join(REPO, "logs", GITHUB_LOGIN, "manifest.json")
    man = {"schema_version": "1.0", "team_id": TEAM_ID,
           "github_login": GITHUB_LOGIN, "generator": "dsh-exporter@1.0",
           "sessions": []}
    if os.path.isfile(mpath):
        with open(mpath, encoding="utf-8") as fh:
            man = json.load(fh)
    man["generator"] = "dsh-exporter@1.0"
    man["updated_at"] = datetime.now(timezone.utc).isoformat()

    rel = os.path.relpath(outfile, REPO).replace(os.sep, "/")
    man["sessions"] = [s for s in man.get("sessions", [])
                       if s.get("session_id") != args.session]
    man["sessions"].append({
        "session_id": args.session,
        "tool": args.tool,
        "started_at": events[0]["ts"],
        "last_event_at": events[-1]["ts"],
        "event_count": len(events),
        "raw_event_count": len(raw),
        "file_path": rel,
        "collection_mode": "cli",
        "health": "ok",
    })
    with open(mpath, "w", encoding="utf-8") as fh:
        json.dump(man, fh, ensure_ascii=False, indent=2)
        fh.write("\n")
    print(f"✅ manifest 已更新（共 {len(man['sessions'])} 个 session）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
