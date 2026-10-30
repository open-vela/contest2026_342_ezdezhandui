# pxview 逻辑分析仪直连工具

DSH 的 `mcp-pxview` MCP 客户端挂在 SSH 隧道后面，隧道断过一次之后客户端会
显示 `server is disconnected` 且不再自动恢复。**但服务端本身是活的**，
所以这里提供一个直接走 HTTP 的极小 MCP 客户端，不依赖 DSH 的 MCP 连接。

## 用法

```sh
python3 tools/test/pxview/px.py get_devices
python3 tools/test/pxview/px.py get_work_mode
python3 tools/test/pxview/px.py set_sample_config '{"sampleRate":4000000,"sampleLimit":8000000}'
python3 tools/test/pxview/px.py start_capture
python3 tools/test/pxview/px.py wait_capture '{"timeoutMs":60000}'
python3 tools/test/pxview/px.py find_next_edge '{"channelIndex":0,"fromSample":0,"risingEdge":true}'
```

也可以当库用：

```python
import sys; sys.path.insert(0, "tools/test/pxview")
from px import init, call
init()
print(call("get_devices"))
```

## 帧解码：`frames.py`（**优先用它，而不是自带解码器**）

```sh
python3 tools/test/pxview/frames.py <startSample> <windowSamples> [sampleRate]
# 例：解码 17.0-18.0 s 这 1 s（1 MHz 采样）
python3 tools/test/pxview/frames.py 17000000 1000000 1000000
```

- 直接从样本解 48 位命令/响应帧，**CRC7 校验**后才算数，不需要挂解码器，也不改采样配置；
- CMD53 会打印**方向位本身**（`READ`/`WRITE` = 参数 bit31，IDF `SD_ARG_CMD53_READ = 0<<31`）、
  功能号、地址、块模式、count —— 这些都是推理要用的字段。

> ⚠️ 实测（2026-10-31）：分析仪**自带** SDIO 解码器把我们的 CMD53 读命令标成
> `R/W flag: Write` / `Block mode`，与线上真实位相反/不符。凡涉及方向、块模式、count 的结论，
> 一律以 `frames.py` 的原始位解码为准。

## 服务端

- 地址：`http://127.0.0.1:10110/mcp`（streamable-http）
- 配置来源：`~/.dsh/profiles/web/cordis.patch.yml` 里的 `mcp-pxview`
- 已确认设备：`PX_Tool PX-Logic U2 channel 32`（driver `pxlogic`，32 通道逻辑）

## 待解决

Logic 模式下触发采集需要 `configure_trigger(stageCount=N, configJson=...)`，
但 `stages` 数组的字段名尚未找到（试过 `value/mask`、`cv/msk/edg`、
`trigger_value/trigger_type`，GET 回来 `stages` 恒为空、采集也立即完成）。
没有触发就只能靠"边录边让板子复位"来抓启动瞬间。
