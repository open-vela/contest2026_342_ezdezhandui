# 2026-10-31 · CMD53 读方向：波形判定原始证据

本轮目标（用户指令）：**先按 B 方案把 CMD53 尺寸扫描改成周期触发，然后判定 CMD53 读方向。**

- 固件：本作品仓 `esp_hosted_sdio.c`（提交见当日 `feat(sdio): CMD53 读阶梯周期触发`）
- 采集：PX-Logic U2，`PX_Tool PX-Logic U2 channel 32`，逻辑模式，**1 MHz × 32 M = 32 s**
- 通道：`ch0=CMD ch1=CLK ch2=D0 ch3=D1 ch4=D2 ch5=D3`
  （与 `add_analyzer(decoderId="sdio", channelMap={cmd:0,clk:1,dat0:2,dat1:3,dat2:4,dat3:5})` 一致）
- 采集起止：`start_capture` → 1 s 后 `tools/build_flash/usb_stable.sh "" "" reset`（复位耗时 4 s）

## 一、周期触发生效（串口证据）

```
esp_hosted_sdio: sweep[0] FN1@0x60 size= 1 ret=-5   first=5a 5a 5a 5a     ← bring-up 首次全阶梯
esp_hosted_sdio: sweep[0] FN1@0x60 size= 2 ret=-110 first=5a 5a 5a 5a
...
esp_hosted_sdio: sweep[6] summary max_ok=-1 r1=-110 r4=-110 r20=-110 r60=-110
esp_hosted_sdio: sweep[7] probe FN1@0x60 size= 1 ret=-110 first=5a 5a 5a 5a  ← 周期探针
esp_hosted_sdio: sweep[8] probe FN1@0x60 size= 1 ret=-110 first=5a 5a 5a 5a
esp_hosted_sdio: sweep[9] probe FN1@0x60 size= 1 ret=-110 first=5a 5a 5a 5a
esp_hosted_sdio: rxloop[40] card-intr=0 ab=-110 ee ee sw=-110/-110/-110 max=-1 p10=-110/5a ...
```

- 探针 `sweep[n] probe`（1 字节）每周期一次；`seq % 6 == 0` 时跑完整阶梯（`sweep[6]`、`sweep[18]` 已在串口看到）。
- 结果全部 `-110`、缓冲区仍是驱动预填的 `0x5a` —— **主机侧一个字节都没收到**。
- 每次失败的读都要等到 `SDMMC_LONGTIMEOUT = 5 s` 才返回，因此**线上观测到的读命令节拍正好是 5 s 一次**。

## 二、线上的 CMD53 读命令（CRC7 校验过的原始解码）

用 `tools/test/pxview/frames.py`（本次新增）直接从样本解码 48 位帧，**不依赖分析仪自带解码器的字段标注**：

```
window 17s : CLK edges 49999, frames 34
   17654886  17.6549 s  CMD53 cmd READ  blk=0 fn=1 addr=0x00060 count=1 op=1 arg=0x1400c001
   17656126  17.6561 s  CMD53 rsp READ  ...                          arg=0x00001000
   17675067  17.6751 s  CMD52 cmd arg=0x00000000          ← NuttX 的 CMD52 "ABRT"
   17774510  17.7745 s  CMD53 cmd READ  blk=0 fn=1 addr=0x00060 count=2 op=1 arg=0x1400c002
   17775750  17.7757 s  CMD53 rsp READ  ...                          arg=0x00001000
window 22s : 4 frames
   22804832  22.8048 s  CMD52 cmd arg=0x00000000
   22885135  22.8851 s  CMD53 cmd READ  blk=0 fn=1 addr=0x00060 count=4 op=1 arg=0x1400c004
   22886375  22.8864 s  CMD53 rsp READ  ...                          arg=0x00001000
window 27s : 4 frames
   27914997  27.9150 s  CMD52 cmd arg=0x00000000
   27995300  27.9953 s  CMD53 cmd READ  blk=0 fn=1 addr=0x00060 count=8 op=1 arg=0x1400c008
   27996540  27.9965 s  CMD53 rsp READ  ...                          arg=0x00001000
```

判定：

| 项 | 值 | 结论 |
|---|---|---|
| 命令字 | `arg=0x1400c001`…`0x1400c008`，**bit31 = 0** | ✅ 读方向正确（IDF：`SD_ARG_CMD53_READ` = `0<<31`） |
| 功能/地址/模式 | `fn=1 addr=0x060` 字节模式、`op=1`（地址自增） | ✅ 与代码请求一致 |
| count | 1、2、4、8 递增 | ✅ 就是阶梯的尺寸序列 |
| 响应 | `R5 arg=0x00001000`，错误位全 0 | ✅ **从机接受了这条读命令** |
| 节拍 | 每次失败后 5 s 才下一条 | ✅ 与 `SDMMC_LONGTIMEOUT` 一致 |

> ⚠️ 附带纠正：分析仪自带 SDIO 解码器把同样这些帧标成 `R/W flag: Write` / `Block mode`。
> 本项目一律以**原始位解码 + CRC7 校验**为准（`frames.py`），自带解码器的字段标注在本板上不可信。

## 三、数据阶段：从机在驱动数据线（本轮关键判定）

每条 CMD53 读命令之后，**四条 DAT 线都出现数据阶段**，长度随请求字节数增长：

| 读命令 | 请求 | 其后 200 ms 内 DAT0/D1/D2/D3 跳变 | 数据阶段起始（时钟号，1 时钟 = 20 样本 @50 kHz） |
|---|---|---|---|
| 17.6549 s | 1 B | 20 / 23 / 18 / 18 | 第 117 拍 `DAT=0000`（起始位） |
| 17.7745 s | 2 B | 8 / 9 / 9 / 9 | 第 98 拍起始位 |
| 22.8851 s | 4 B | 3 / 4 / 3 / 3 | 第 102 拍起始位 |
| 27.9953 s | 8 B | 11 / 12 / 9 / 9 | 第 96 拍起始位 |

1 字节读（FN1 `0x60`）的数据阶段逐拍展开（`时钟号:C MD3D2D1D0`）：

```
116:0F 117:00 118:03 119:0C 120:00 121:00 122:03 123:0C 124:00 125:00 ...
```

- 第 117 拍 `0x0` = **起始位**（四条 DAT 全低），符合 SDIO 数据阶段格式；
- 随后两拍 `0x3`、`0xC` → 高半字节 3、低半字节 C = **0x3C**；
- 2 字节读同样以 `0x3C 0x00` 开头。

**0x3C = 60**，正是从机 `PACKET_LEN`（FN1 `0x60`）的值 —— 同一时刻用 CMD52 读同一寄存器得到
`PKT_LEN = 0x3c`（`rxloop[…] w0=0040cc00/3c00c003`）。**从机送出的就是正确数据。**

对照：无命令在飞的空闲窗口（19.0–19.2 s）`CMD / DAT0 / DAT1` 跳变**均为 0**（线路稳定），
说明上面的数据阶段确实由读命令触发，不是线路噪声。

## 三·补 周期探针本身也在线上（B 方案的效果）

另起一次采集（**不复位**，32 s 窗口，板子已在 rxloop 内），原始解码到：

```
31032416  31.0324 s  CMD52 cmd arg=0x1000c000     ← rxloop 的 INT_RAW/PKT_LEN 轮询（每 ~40 ms 一对）
31072198  31.0722 s  CMD52 cmd arg=0x1000c200
31111999  31.1120 s  CMD52 cmd arg=0x1000c400
31422309  31.4223 s  CMD53 cmd READ blk=0 fn=1 addr=0x060 count=1 op=1 arg=0x1400c001  ← 周期探针
31423549  31.4235 s  CMD53 rsp READ  arg=0x00001000
```

- 周期触发**确实把 1 字节读命令送上了线**（`arg=0x1400c001`，与 `sweep[n] probe` 一一对应），
  这是附录 AG 缺的「稳定可抓的读方向 CMD53」。
- 这一条探针**之后 20 ms 内 DAT0–D3 没有数据阶段**（DAT0 仅 2 次跳变，且都发生在响应窗口内），
  与 bring-up 时阶梯读（§三）形成对照 —— 与附录 AB 记录的「长时间运行后退化」现象一致，
  即：**链路只在 bring-up 初期肯送数据**。判定仍以 §三 的首次读为准（那是唯一有正确数据的样本）。

## 四、判定结论

**CMD53 读方向：从机侧正常，主机侧不收。**

1. 读命令线上正确、从机以合法 R5（`0x1000`）应答；
2. 从机**在数据阶段驱动了四条 DAT 线**，起始位规范、首字节等于寄存器真值（`PACKET_LEN = 0x3C`）；
3. 主机缓冲区保持预填值（`5a` / `ee`），IDMAC 不消费描述符（`idsts=00002000`）、5 s 超时。

→ 与交接备忘（附录 AI）的判定式一致：**走「主机接收路径」分支**，而不是「从机不驱动」分支。
因此附录 AI 第 2 条（补 `clkena` 的 `0x00020000`）**不是**本卡点的解 —— 见附录 AJ 的纠正。

## 五、测量复现命令

```bash
# 1) 采样配置（1 MHz × 32 M = 32 s，覆盖复位 + bring-up + 阶梯）
python3 tools/test/pxview/px.py set_sample_config '{"sampleRate":1000000,"sampleLimit":32000000}'
# 2) 起采 → 复位（复位耗时约 4 s）
python3 tools/test/pxview/px.py start_capture
tools/build_flash/usb_stable.sh "" "" reset
python3 tools/test/pxview/px.py wait_capture '{"timeoutMs":90000}'
# 3) 原始帧解码（CRC7 校验；只看样本，不改采样配置）
python3 tools/test/pxview/frames.py 17000000 1000000 1000000
# 4) 串口侧（Tabby 占用 /dev/ttyUSB0 时）：
python3 tools/engineering/tabby_mcp.py text get_terminal_buffer \
  '{"sessionId":"ccfce850-0bae-4920-aab0-68ffae31cc83","lastNLines":800}'
```
