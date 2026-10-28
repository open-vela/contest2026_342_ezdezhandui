#!/usr/bin/env python3
"""Minimal MCP client for the Tabby terminal server.

Tabby holds the board's serial ports open, so this is the way to reach the P4
and C6 consoles while the user has them in tabs. Unlike the pxview server this
one is stateful: the initialize response carries an Mcp-Session-Id header that
every later call must send back, or the server answers
"Initialize the session before sending MCP messages".

  python3 tools/test/tabby/tabby.py get_session_list
  python3 tools/test/tabby/tabby.py get_terminal_buffer '{"sessionId":"...","lines":40}'
"""
import json, sys, time, requests

URL = "http://127.0.0.1:3001/mcp"
H = {"Content-Type": "application/json",
     "Accept": "application/json, text/event-stream"}

_session = [None]
_id = [0]


def _parse(r):
    data = None
    for line in r.text.splitlines():
        if line.startswith("data:"):
            data = line[5:].strip()
            break
    if data is None:
        data = r.text.strip()
    try:
        return json.loads(data)
    except Exception:
        return {"_raw": r.text[:400]}


def _post(body, sid):
    h = dict(H)
    if sid:
        h["Mcp-Session-Id"] = sid
    return requests.post(URL, headers=h, json=body, timeout=60)


def _post_retry(body, sid):
    last = None
    for attempt in range(4):
        try:
            return _post(body, sid)
        except Exception as exc:                              # noqa: BLE001
            last = exc
            time.sleep(0.4 * (attempt + 1))
    raise last


def init():
    r = _post_retry({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                     "params": {"protocolVersion": "2024-11-05",
                                "capabilities": {},
                                "clientInfo": {"name": "tabby-cli", "version": "1"}}}, None)
    sid = r.headers.get("Mcp-Session-Id") or r.headers.get("mcp-session-id")
    if not sid:
        raise RuntimeError("server returned no Mcp-Session-Id")
    _session[0] = sid
    _post_retry({"jsonrpc": "2.0", "method": "notifications/initialized"}, sid)
    return sid


def call(name, **args):
    if _session[0] is None:
        init()
    _id[0] += 1
    r = _post_retry({"jsonrpc": "2.0", "id": _id[0], "method": "tools/call",
                     "params": {"name": name, "arguments": args}}, _session[0])
    j = _parse(r)
    if "error" in j:
        return {"error": j["error"]}
    out = [c.get("text") for c in j.get("result", {}).get("content", [])
           if c.get("type") == "text"]
    txt = "\n".join(t for t in out if t)
    try:
        return json.loads(txt)
    except Exception:
        return txt if txt else j.get("result")


if __name__ == "__main__":
    init()
    tool = sys.argv[1]
    a = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
    print(json.dumps(call(tool, **a), ensure_ascii=False, indent=1)[:4000])
