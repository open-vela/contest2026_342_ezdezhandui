# logs —— 会话与调试记录

| 文件 | 内容 |
|---|---|
| `2026-10-30_wifi-sdio-session.md` | WiFi（C6 + SDIO + ESP-Hosted）驱动实现全程会话记录（用户指令 / 助手正文 / 工具调用摘要）|
| `2026-10-30_session-raw.jsonl.zstd` | 同一会话的**原始未加工**记录（DSH session v4 JSONL，zstd 压缩，33,723 行）|

## 查看原始记录

```bash
zstd -d -c 2026-10-30_session-raw.jsonl.zstd | less
```

每行一条事件，`type` 字段区分类型：`user/message`、`assistant/message`、
`tool/call`、`tool/result`、`step/start`、`step/end`、`goal/change` 等。

## 说明

- 可读版只保留对话正文与工具调用摘要；**工具结果未展开**（原始记录里完整保留）。
- 时间戳来自会话内部计时，仅用于排序。
