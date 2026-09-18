# tools/ —— 工具索引（按用途分类）

工具按三类组织，**评审/复现只需用到 `build_flash/` 与 `test/` 两个子目录**；
`engineering/` 是开发期维护工具（与复现无关）。

## 📐 engineering/ —— 工程相关（仓库 / 清单 / 日志 / 导出）

| 工具 | 用途 |
|---|---|
| `export.py` | 把 openvela 工作区的改动导出到作品仓 4 棵文件树（dry-run / `--apply` / `--apply --manifest`） |
| `gen_manifest_copyfiles.py` | 再生/校验 manifest 的 `<copyfile>` 清单（`--check` 校验与文件树严格一一对应） |
| `lock-revision.sh` | 锁定 manifest 中作品仓 self-ref 的 revision 到当前 HEAD（并整份同步 `.repo/manifests` 副本） |
| `repo_baseline_reset.py` | 把所有 repo 仓库 HEAD 归位到 manifest 固定节点，统一"导出 diff"形态（默认 dry-run） |
| `dsh_session_to_contest_log.py` | 把 DSH 会话日志转换为大赛 AI Coding 日志格式（`logs/` 目录用） |
| `export-legacy.sh` | ⚠️ 已废弃：旧布局 `board/esp32p4_vela/` 专用，新开发请用 `export.py` |
| `tabby_mcp.py` | Tabby MCP 服务：关闭占用串口的标签页，释放 USB 端口（配合真机调试） |

## 🔥 build_flash/ —— 构建烧录相关（评审复现路径）

| 工具 | 用途 |
|---|---|
| `usb_stable.sh` | **MCUboot 两镜像稳定烧录/复位**：`0x2000` 引导 + `0x20000` 应用，双镜像 hash 校验（`flash` / `reset` / `chip_id`） |
| `demo_aiagent.sh` | 一键演示：烧录 → 复位 → 启动 ai_agent → 里程碑断言 → 输出摘要 |
| `README_usb.md` | USB 端口拓扑与烧录说明（ttyACM0 烧录 / ttyUSB0 console） |

## 🔬 test/ —— 测试相关（真机验证 / 诊断 / 取证 / 证据）

| 工具 | 用途 |
|---|---|
| `board.py` | 串口 harness：`reset`（复位抓启动日志）/ `run`（发命令并断言）/ `script` / `watch` / `login` |
| `verify_aiagent.sh` | ai_agent 启动里程碑闭环断言（P0→P6 + 12 skills + eth0 + cron，多轮采集容忍 USB 丢字节） |
| `camera_diag.sh` | 摄像头链路一次性判定：传感器 → MIPI-CSI → 桥 → DW-GDMA（`MOVING` = 出帧） |
| `wedge_diag.sh` | 「ai_agent 启动后整机失聪」现场 JTAG 取证与判读（只读，不写设备） |
| `fb2png.py` | 把 JTAG dump 的帧缓冲（RAW RGB565）转 PNG 并打印画面统计 |
| `ws_demo.py` | WebSocket 客户端：向板端 28789 发消息、收 LLM 回答（端到端对话复现） |
| `make_demo_video.py` | 用真机素材合成演示视频（证据版，1080p mp4，输出到 `提交材料/`） |

## 快速入口

```bash
# 评审复现（构建烧录）
tools/build_flash/usb_stable.sh <app.bin> <boot.bin>   # 两镜像烧录
tools/test/board.py reset                               # 复位 → nsh>

# 真机验证
tools/test/verify_aiagent.sh                            # ai_agent 启动断言
tools/test/camera_diag.sh                               # 摄像头链路判定
tools/test/ws_demo.py --host <ip> --port 28789 --send "你好" --wait 60   # LLM 对话

# 开发期维护
python3 tools/engineering/gen_manifest_copyfiles.py --check   # 清单一致性自检
tools/engineering/lock-revision.sh                            # 锁定 manifest revision
```
