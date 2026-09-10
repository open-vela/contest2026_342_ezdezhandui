# 项目状态（STATUS）

> 最后更新：2026-09-07 · 主线已闭环，交付形态=文件树+manifest copyfile（PR #7）

## 一、主线达成状态

| 里程碑 | 状态 | 证据 |
|---|---|---|
| ESP32-P4X 平台移植 | ✅ | nuttx 281 文件 + 8 删除收拢作品仓，编译通过 |
| 烧录链路（0x2000） | ✅ | esptool write_flash 0x2000，正常 boot 到 NSH |
| console（USJ/UART0） | ✅ | UART0 console 完整 NuttX 输出 |
| eth0 RJ45 联网 | ✅ | 10.0.0.2 RUNNING，自动获取 IP |
| PSRAM 32MB heap | ✅ | free：Umem total=33,576,240 |
| ai_agent 完整启动 | ✅ | P0→P5 全 rc=0，36 tools / 10 skills |
| ai_agent 稳定运行 | ✅ | 30s+ 无 PMP fault（Halt cause 11）|
| 交付复现（README 四步） | ✅ | init→sync→deploy→build 全通过，293 copyfile 落位 |

## 二、当前交付

- **PR #7**（open-vela/contest2026_342_ezdezhandui）：esp32p4 全部工作，19+ commits，MERGEABLE+CLEAN
- **交付形态**：`board/esp32p4_vela/` 文件树（nuttx 281 + apps 3 + ai_agent 9）+ `contest2026_342_ezdezhandui.xml` 293 copyfile 自动映射
- **复现路径**：README §四（repo init → sync → deploy → build）

## 三、演示脚本（demo_aiagent.sh）

一键演示 ai_agent：烧录最新固件 → 复位 → 启动 ai_agent → 捕获 READY + 功能输出。
```bash
tools/demo_aiagent.sh [firmware.bin]
```

## 四、待办（9/10 演示冻结前）

- [ ] ai_agent 真实 LLM 请求验证（TLS 闭环；MiMo key 已泄露，**须先轮换**再用 agent_secrets.h 注入）
- [ ] ai_agent 接入方式决策：NSH 前台（当前）vs init 自启/服务化
- [ ] 演示叙事固化：CLI 对话主链路（语音/摄像头视硬件到货情况）
- [ ] `ai_agent &` ROM fault（次要，后台运行场景）

## 五、安全提醒

- ⚠️ MiMo API key 曾误入公开仓历史（commit 4d7f318）——**必须轮换该 key**；
  新 key 一律经本地 `ai_agent/include/agent_secrets.h`（.gitignore 忽略）注入，严禁写进 agent_config.h
