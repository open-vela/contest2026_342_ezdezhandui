#!/usr/bin/env python3
"""Minimal MCP client for the PXView signal analyzer (streamable-http)."""
import json, sys, time, requests

URL = "http://127.0.0.1:10110/mcp"
H = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
_id = [0]

def _post_once(body):
    r = requests.post(URL, headers=H, json=body, timeout=60)
    data = None
    for line in r.text.splitlines():
        if line.startswith("data:"):
            data = line[5:].strip(); break
    if data is None:
        data = r.text.strip()
    try:
        return json.loads(data)
    except Exception:
        return {"_raw": r.text[:400]}


def _post(body):
    """POST with retries.

    The server sits behind an SSH tunnel and occasionally emits a stray SSE
    line that makes requests fail with BadStatusLine; without a retry a long
    capture loop dies on the first hiccup.
    """

    last = None
    for attempt in range(4):
        try:
            return _post_once(body)
        except Exception as exc:                              # noqa: BLE001
            last = exc
            time.sleep(0.4 * (attempt + 1))
    raise last

def init():
    _post({"jsonrpc":"2.0","id":1,"method":"initialize",
           "params":{"protocolVersion":"2024-11-05","capabilities":{},
                     "clientInfo":{"name":"px","version":"1"}}})
    _post({"jsonrpc":"2.0","method":"notifications/initialized"})

def call(name, **args):
    _id[0] += 1
    j = _post({"jsonrpc":"2.0","id":_id[0],"method":"tools/call",
               "params":{"name":name,"arguments":args}})
    if "error" in j:
        return {"error": j["error"]}
    res = j.get("result", {})
    out = []
    for c in res.get("content", []):
        if c.get("type") == "text":
            out.append(c.get("text"))
    txt = "\n".join(out)
    try:
        return json.loads(txt)
    except Exception:
        return txt if txt else res

if __name__ == "__main__":
    init()
    name = sys.argv[1]
    args = json.loads(sys.argv[2]) if len(sys.argv) > 2 else {}
    print(json.dumps(call(name, **args), ensure_ascii=False, indent=1)[:3000])
