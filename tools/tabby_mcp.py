#!/usr/bin/env python3
"""Minimal Streamable-HTTP MCP client for the Tabby MCP server.

Usage:
  tabby_mcp.py init              # handshake, persist Mcp-Session-Id
  tabby_mcp.py tools             # list tool names + first line of description
  tabby_mcp.py sessions          # get_session_list (pretty)
  tabby_mcp.py call TOOL [JSON]  # tools/call, prints raw result JSON
  tabby_mcp.py text TOOL [JSON]  # tools/call, prints only concatenated text

Env: TABBY_MCP_URL (default http://localhost:3001/mcp)
"""
import json
import os
import sys
import urllib.error
import urllib.request

URL = os.environ.get("TABBY_MCP_URL", "http://localhost:3001/mcp")
SIDFILE = "/tmp/.tabby_mcp_sid"
PROTO = "2025-03-26"


def _post(body, sid=None, timeout=180):
    data = json.dumps(body).encode()
    headers = {
        "Content-Type": "application/json",
        "Accept": "application/json, text/event-stream",
    }
    if sid:
        headers["Mcp-Session-Id"] = sid
    req = urllib.request.Request(URL, data=data, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.read().decode("utf-8", "replace"), resp.headers.get("Mcp-Session-Id")
    except urllib.error.HTTPError as e:
        return "HTTP %d: %s" % (e.code, e.read().decode("utf-8", "replace")), None
    except Exception as e:  # noqa: BLE001
        return "ERR: %r" % (e,), None


def _parse(raw):
    """Decode either a plain JSON body or an SSE stream into a list of objects."""
    out = []
    s = raw.lstrip()
    if s.startswith("{"):
        try:
            out.append(json.loads(s))
            return out
        except ValueError:
            pass
    for line in raw.splitlines():
        line = line.strip()
        if line.startswith("data:"):
            try:
                out.append(json.loads(line[5:].strip()))
            except ValueError:
                pass
    return out


def _save_sid(sid):
    if sid:
        with open(SIDFILE, "w") as fh:
            fh.write(sid)


def _load_sid():
    try:
        with open(SIDFILE) as fh:
            return fh.read().strip() or None
    except OSError:
        return None


def _rpc(method, params=None, notify=False):
    sid = _load_sid()
    body = {"jsonrpc": "2.0", "method": method}
    if params is not None:
        body["params"] = params
    if not notify:
        body["id"] = 1
    raw, newsid = _post(body, sid)
    _save_sid(newsid)
    return _parse(raw), raw


def cmd_init():
    objs, raw = _rpc(
        "initialize",
        {
            "protocolVersion": PROTO,
            "capabilities": {},
            "clientInfo": {"name": "dsh-captain", "version": "1.0"},
        },
    )
    if not objs:
        print("initialize failed: %s" % raw[:400])
        return 1
    info = objs[0].get("result", {})
    print("session-id: %s" % (_load_sid() or "(none)"))
    print("server    : %s %s" % (info.get("serverInfo", {}).get("name"),
                                info.get("serverInfo", {}).get("version")))
    print("protocol  : %s" % info.get("protocolVersion"))
    _rpc("notifications/initialized", {}, notify=True)
    return 0


def cmd_tools():
    objs, raw = _rpc("tools/list", {})
    if not objs or "result" not in objs[0]:
        print("tools/list failed: %s" % raw[:400])
        return 1
    for t in objs[0]["result"].get("tools", []):
        desc = (t.get("description") or "").split("\n")[0][:70]
        print("%-26s %s" % (t["name"], desc))
    return 0


def _call(tool, args):
    objs, raw = _rpc("tools/call", {"name": tool, "arguments": args})
    if not objs:
        print("call failed: %s" % raw[:400])
        return None
    if "error" in objs[0]:
        print("rpc error: %s" % json.dumps(objs[0]["error"])[:500])
        return None
    return objs[0].get("result", {})


def _text_of(result):
    parts = []
    for c in result.get("content", []):
        if c.get("type") == "text":
            parts.append(c.get("text", ""))
        else:
            parts.append(json.dumps(c))
    return "\n".join(parts)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    cmd = sys.argv[1]

    if cmd == "init":
        return cmd_init()
    if cmd == "tools":
        return cmd_tools()
    if cmd in ("call", "text", "sessions"):
        if cmd == "sessions":
            tool, args = "get_session_list", {}
        else:
            if len(sys.argv) < 3:
                print("need TOOL name")
                return 2
            tool = sys.argv[2]
            args = json.loads(sys.argv[3]) if len(sys.argv) > 3 else {}
        if _load_sid() is None:
            cmd_init()
        result = _call(tool, args)
        if result is None:
            return 1
        if result.get("isError"):
            sys.stderr.write("TOOL ERROR\n")
        if cmd == "text":
            print(_text_of(result))
        else:
            print(json.dumps(result, ensure_ascii=False, indent=2))
        return 1 if result.get("isError") else 0
    print("unknown command: %s" % cmd)
    return 2


if __name__ == "__main__":
    sys.exit(main())
