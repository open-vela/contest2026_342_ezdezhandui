# 05 应用层（DeskMate 中控 UI）

> 面向 openvela 大赛「AI 硬件产品创新」赛道的应用层：在已完成的 ESP32-P4 平台移植之上，
> 把 **摄像头 / WiFi / 网口 / 触摸屏** 四条能力收进一个 LVGL 中控界面，并接上 `ai_agent`；
> 同一套代码可在 **真机**、**NuttX 模拟器（宿主二进制）** 与 **goldfish 官方模拟器** 上运行。

| 文档 | 内容 |
|---|---|
| [应用层设计与模拟器闭环](应用层设计与模拟器闭环.md) | 产品形态、五页 UI、HAL 抽象、agent bridge、事件引擎、双模拟器验证路径与证据 |

代码与运行说明见应用自身的 [README](../../../app/apps/examples/deskmate/README.md)
（仓内路径 `app/apps/examples/deskmate/README.md`）。

## 一句话定位

移植赛道证明「**这块新板子能跑 openvela**」；应用层证明「**跑起来之后有一个能用的产品**」：
上电即是中控屏——看得到（摄像头预览）、连得上（eth0/wlan0 状态与配网）、
点得动（触摸导航）、说得通（agent 对话 + 主动事件）。

## 与赛题四点的对应

| 赛题要求 | 应用层交付 |
|---|---|
| ① Agent 在硬件上跑起来 | `deskmate` 内置 agent bridge（`ws` 模式对接 ai_agent 28789，协议与 `tools/test/ws_demo.py` 一致）；UI 内直接对话 |
| ② ≥1 个自定义 Skill | 沿用 ai_agent 的 `center-assistant` / `quick-note`；Chat 页预设短语就是这两条技能的真实入口 |
| ③ ≥1 个「主动 + 执行」 | 摄像头**帧差运动检测** → 主动消息 + 顶栏提示条；agent 侧亦可用 `ws_server_send("deskmate-ui", …)` 反向推送 |
| ④ 完整应用场景说明 | 本文档 + 应用 README + 五页 UI 的真实截图证据（模拟器快照 PNG） |
