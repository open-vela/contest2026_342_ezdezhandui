# Tabby 终端 MCP 直连工具

用户把 P4 和 C6 的串口开在 Tabby 的标签页里，**Tabby 因此独占 `/dev/ttyUSB0`
与 `/dev/ttyUSB1`** —— 别的进程（包括 `board.py`、esptool、pyserial）
打不开这两个口。这时读板子日志的唯一通路就是 Tabby 的 MCP。

刻意记下这个坑：我曾经因为"打不开串口、读到 0 字节"而**误判板子没有上电**，
实际板子一直在正常跑 NSH。**读到 0 字节只说明"我读不到"，不说明"对面没在跑"。**

## 服务端

- 地址：`http://127.0.0.1:3001/mcp`（配置在 `~/.dsh/profiles/headless/cordis.patch.yml`
  的 `mcp-tabby`，注意是 **headless** profile，不在 web profile 里）
- 与 pxview 不同，它是**有状态**的：`initialize` 的响应头带 `Mcp-Session-Id`，
  之后每次调用都要回传这个头，否则服务端回
  `Initialize the session before sending MCP messages`。

## 用法

```sh
python3 tools/test/tabby/tabby.py get_session_list
python3 tools/test/tabby/tabby.py get_terminal_buffer '{"sessionId":"...","lines":80}'
python3 tools/test/tabby/tabby.py exec_command '{"sessionId":"...","command":"ls /dev"}'
```

当库用：

```python
import sys; sys.path.insert(0, "tools/test/tabby")
from tabby import init, call
init()
print(call("get_session_list"))
```

## 本机会话（实测）

| sessionId | 串口 | 用途 |
|---|---|---|
| `d9cf29e5-b30b-4364-be78-552c44fca37a` | `/dev/ttyUSB0` | **P4 console**（NuttX NSH）|
| `e92ddaaa-8876-48d4-9dfa-99871476b03f` | `/dev/ttyUSB1` | **C6 console**（ESP-Hosted 从机）|

`/dev/ttyACM0`（P4 USB-JTAG，烧录用）**不在 Tabby 里**，因此烧录不受影响。

## 常用工具

`get_session_list`、`get_terminal_buffer`、`exec_command`、`send_input`、
`list_tabs`、`open_profile`、`sftp_*`（共 35 个）。
