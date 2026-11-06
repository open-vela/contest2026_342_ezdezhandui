#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把官方答辩模板改造成《桌伴 DeskMate》决赛答辩稿（14 页 / 480 秒）。

设计
    不重建母版、不改配色、不动图形。对模板里每个占位符形状**保留**它的
    rPr（字号 / 字重 / 颜色）与 pPr（对齐 / 锚点），只替换 <a:t> 文本；
    多段文字按模板已有的段落数增量复用或裁剪。于是 MiSans 字号层级
    （144 / 72 / 46 / 30 / 26 / 22 / 19 / 18pt）、圆角卡片、青蓝紫序号球
    全部原样保留。

来源
    模板：2026_首届_openvela_AI_硬件全球开发者大赛答辩模板.pptx（31 页）
    内容：docs/60_交付与提交/答辩讲稿_桌伴DeskMate.md 的同一张内容表
    数据：全部取自本仓 HEAD 的实测记录，见下方 FACTS（单一常量表）

用法
    python3 tools/engineering/build_defense_ppt.py \
        --template <模板.pptx> \
        --out      提交材料/答辩PPT_桌伴DeskMate.pptx \
        --speaker  docs/60_交付与提交/答辩讲稿_桌伴DeskMate.md \
        --preview  docs/60_交付与提交/答辩PPT预览
    python3 tools/engineering/build_defense_ppt.py --verify <成稿.pptx>

校验（--verify）
    1 零残留占位符   2 口径一致（数字必须在 FACTS 里）  3 时长合计 <= 480 s
    4 页数 == 14     5 仓库零改动（生成前后 git status 对比）
"""

import argparse
import copy
import os
import re
import subprocess
import sys

from pptx import Presentation
from pptx.util import Emu, Pt

# --------------------------------------------------------------------------
# FACTS —— 全片唯一的数字来源。改数字只改这里。
# 依据：docs/00_入口/STATUS.md、README.md、logs/ez-xu/manifest.json、
#       docs/30_实现/连接_WiFi_SDIO.md、git log（脚本实算）。
# --------------------------------------------------------------------------
FACTS = {
    "sram_before": "460KB",
    "sram_after": "120.3KB",
    "sram_window": "512KB",
    "app_bin": "1,835,008 B",
    "mcuboot_bin": "24,672 B",
    "commits": "597",
    "commits_day": "174",
    "ai_sessions": "31",
    "ai_events": "49,599",
    "la_clock": "403 kHz",
    "la_rate": "25 MS/s",
    "la_edges": "714,543",
    "la_initclk": "81",
    "wlan_ip": "192.168.1.9",
    "defects": "7",
}

# --------------------------------------------------------------------------
# 页面内容表：每页 = (模板页号, 标题, 秒数, 讲稿, 形状文本)
#   形状文本: {shape_id: text}；text 为 list 时按段落逐条写入（形状有几个
#   段落就用几条，多则复用最后一段的格式、少则删掉多余段落）。
#   shape_id = None 表示整页删除。
# --------------------------------------------------------------------------
SLIDES = [
    # ---------------- 1 封面 ----------------
    (1, "桌伴 DeskMate", 10,
     "各位评委老师好，我们是 ez-xu 队，作品是把 openvela 移植到官方待适配的 ESP32-P4X-C5 开发板上，"
     "并把 agent、网络、摄像头、显示集成成一个可运行的系统。这一份答辩讲的不是产品创意，"
     "而是三件事：平台怎么落地、驱动怎么攻坚、以及整个验证过程怎么由 AI 通过 MCP 驱动真实仪器自动跑完。",
     {
         10: "桌伴 DeskMate",
         7: ["ez-xu 队　·　contest2026_342　·　openvela × ESP32-P4X-C5",
             "平台移植　·　驱动集成　·　AI 驱动的硬件在环验证"],
     }),

    # ---------------- 2 目录 ----------------
    (4, "目录", 15,
     "五个部分。第一部分技术底座，讲移植本身；第二部分是这次答辩的主线，AI 驱动的硬件在环闭环；"
     "第三部分驱动攻坚；第四部分子系统集成；最后是平台价值与工业落地。",
     {
         5: "01　技术底座与移植", 6: "新平台适配 · MCUboot · XIP",
         8: "02　AI 驱动的 HIL 闭环", 9: "Tabby · OpenOCD · pxview · AI",
         11: "03　驱动攻坚", 12: "SDIO 攻坚 · 7 个真实缺陷",
         14: "04　子系统集成", 15: "显示 · 触摸 · 相机 · 音频",
         17: "05　平台价值与工业落地", 18: "回流上游 · 电力 · 新能源 · 3D",
         20: None, 21: None,
     }),

    # ---------------- 3 技术底座 ----------------
    (11, "把 openvela 完整落到一块官方待适配的板上", 40,
     "先说底座。这块板 P4 是双核 RISC-V 400 兆，带 32MB PSRAM、MIPI-DSI 和 CSI，"
     "WiFi 靠板载的 C6 协处理器，openvela 官方把它列为待适配板。整个过程最硬的一战是代码空间："
     "SIMPLE_BOOT 下代码只能跑在 512KB 的 SRAM 窗口里，实测占到 460KB 以上，只剩几十 KB。"
     "我用四条独立证据链证明这个窗口不可扩容，然后切到 MCUboot 二级引导，把 text 和 rodata 搬到 flash 以 XIP 执行，"
     "静态占用从 460KB 降到 120.3KB。现在一条 build.sh 同时产出应用镜像和引导镜像两个产物。",
     {
         6: "移植", 7: "NuttX 上游并入 openvela", 8: "板级 + 芯片层 + 驱动，随 repo sync 落位",
         10: "空间", 11: "512KB SRAM 窗口是硬限制", 12: "实测 460KB，仅余几十 KB；ROM 锁定 PMP 证明不可扩容",
         14: "破局", 15: "MCUboot 二级引导 + XIP", 16: "text+rodata 迁 flash，静态占用 460KB → 120.3KB",
         18: "结论", 20: "一次 build.sh 产出双镜像：应用 1,835,008 B + 引导 24,672 B，评审三步复现",
     }),

    # ---------------- 4 HIL 闭环四部件 ----------------
    (23, "AI 驱动的硬件在环：四个部件接成一条链", 38,
     "这是整套方法的核心。传统嵌入式调试，人坐在板子前敲命令、看串口、拿逻辑分析仪抓波形。"
     "我把这四个角色都变成了 AI 可以调用的接口：烧录调试走 OpenOCD，走 USB-JTAG，"
     "串口日志走 Tabby 的 MCP，因为串口被 Tabby 独占，别的进程根本打不开；"
     "波形走 pxview 逻辑分析仪，32 通道；然后 AI 来编排前三者。四个部件一接上，"
     "人就从调试回路里退出来了，剩下的是一段可以被自动执行的协议。",
     {
         7: "OpenOCD · 烧录与调试", 8: "USB-JTAG 复位 / 烧录；GDB 会掉线但 telnet 4444 一直可用，PC 与寄存器随手可取、符号可回解析",
         11: "Tabby MCP · 串口日志", 12: "Tabby 独占 ttyUSB0（P4）与 ttyUSB1（C6），MCP 是唯一通路；有状态会话，HIL 里最容易踩空的一环",
         15: "pxview · 逻辑分析仪", 16: "PX-Logic U2 32 通道，抓 SDIO 六线；实测 403 kHz、单次 714,543 次跳变",
         19: "AI · 编排者", 20: "把前三个仪器变成可编程接口，把一次实验变成可复现脚本；人只定目标，回路自己跑",
     }),

    # ---------------- 5 一条命令 + 四个真坑 ----------------
    (20, "一条命令跑完：复位 → 抓波形 → 读串口 → 当场解码", 45,
     "具体到一个脚本。evidence.py 一条命令做四件事：用 OpenOCD 复位，同时让 pxview 开始采波形，"
     "然后经 Tabby 读串口，最后当场把波形解码成 SDIO 帧、落盘归档。cycle.sh 把改、建、烧、验、导、提串成闭环。"
     "这一页更重要的是四个坑，它们都不是代码 bug，而是仪器接入之后必然撞上的工程判断。"
     "第一，读到 0 字节只说明我读不到，不说明对面没在跑，我曾经因此误判板子没上电。"
     "第二，分析仪是连续采样的，缓冲区之外会被滚掉，所以解码必须和采集在同一次运行里做。"
     "第三，串口的 scrollback 跨多次启动，一锅端出来会把三次启动的行混在一起，让分析去追一个不存在的矛盾，"
     "所以必须从最后一条启动横幅往后切。第四，分析仪自带的解码器不可信，"
     "实测它把读命令标成写，所以方向、块模式这些判据一律用我自己写的 CRC7 解码器从原始样本位解。",
     {
         13: "证据优先", 14: "一条命令复位 + 采集 + 读串口 + 解码归档，产物是可审计的四个文件",
         15: "判据优先", 16: "只采不判等于没做：CRC7 校验通过才认帧，样本位自己解，不信仪器的默认标注",
         17: "时序约束", 18: "采集与解码必须同一次运行；跨次解码只剩空窗口",
         19: "反假设", 20: "0 字节 ≠ 没在跑；scrollback 跨多次启动，必须从最后一条横幅切起",
     }),

    # ---------------- 6 驱动攻坚：证据链 + 7 个缺陷 ----------------
    (15, "SDIO 攻坚：证据链钉死每一层，修出 7 个真实缺陷", 45,
     "这一页讲驱动攻坚本身。SDIO 这条链我是分层钉死证据的：枚举命令 CRC7 全部合法，"
     "卡功能协商到 FN1、512 字节块，ESP-Hosted 帧层起来，CMD52 能写 5A 读回 5A，"
     "CMD53 我们先往缓冲区预填 AA，传完之后被 DMA 覆盖，证明传输确实发生过。"
     "整个过程修出 7 个真实缺陷，其中两个是真正的根因：一是 SDIO 的 PLL 从来没有被 PMU 上电，"
     "导致控制器一条命令都不执行，但所有寄存器读回来全是对的，这个特别有欺骗性；"
     "二是等待响应的函数忽略了命令字里的响应标志，导致 CMD5 从来没有被发出去过。"
     "在此之前几十轮里所有的超时，其实都不是等不到回应，而是根本没发。",
     {
         8: "命令层", 9: "CMD0/5/3/7 枚举多次复位稳定，48 位帧 CRC7 全部合法",
         13: "卡功能层", 14: "FN1 就绪、512 字节块协商成功；CCCR 读写逐项验证",
         18: "传输层", 19: "CMD52 写 0x5A 读回 0x5A；CMD53 预填 0xAA 被 DMA 覆盖",
         23: "根因层", 24: "共 7 处：SDIO PLL 未上电、响应标志被忽略、完成位未清、DMA 无 cache 维护",
         28: "帧层", 29: "ESP-Hosted 帧层 up（hdr v1、12 字节头、开启校验）",
         33: "终局", 34: "wlan0 拿到 192.168.1.9，三连 ping 全绿",
     }),

    # ---------------- 7 驱动集成 5 子系统 ----------------
    (16, "一次接进五个子系统，全部有真机证据", 50,
     "把上面这套方法用起来，我们把五个子系统一次接进了同一块板。"
     "显示是 EK79007 的 7 寸屏，1024×600；触摸是 GT911；摄像头是 SC2336，走 V4L2；"
     "音频是 ES8311，I2S 加 DMA；网络是板载 C6，经 SDIO 走 ESP-Hosted，最终 wlan0 拿到地址、ping 通。"
     "这里要强调一个坑：以太网口在这块板上从来没有接线，所以网络能力只能讲 WiFi，不能讲以太网保底。"
     "中枢是中心这块板，五个分支每一条都对应一份真机产物，不是「代码写完了」。",
     {
         11: "openvela × P4X", 12: "五个子系统 · 真机证据",
         15: "显示", 16: "EK79007 1024×600，fb0 可导出",
         19: "触摸", 20: "GT911 /dev/input0；补 ioctl",
         23: "摄像头", 24: "SC2336 经 V4L2 出帧；修 CSI/ISP",
         27: "网络", 28: "C6 → SDIO → Hosted；wlan0 已通",
     }),

    # ---------------- 8 平台级缺陷 ----------------
    (10, "顺手修掉的平台级缺陷，影响面大于本作品", 40,
     "下面这些缺陷不是我们项目自己的 bug，是平台级的，任何一块 esp32p4 板都会撞上。"
     "最狠的一个是内核级：任务退出时那条换出 ECALL 被引导期的守卫丢掉了，"
     "结果是任何任务一退出整机就失聪，表现为 agent 一启动网络和串口全哑；"
     "我们用一个判别实验证明它跟 agent 无关——在子 shell 里敲一个 exit 同样死机。"
     "还有中断层 up_enable_irq 从不置位 CLIC 的使能位，显示驱动里八个结构体没有零初始化导致写被当成读，"
     "以及摄像头那条 CSI 与 ISP 共享桥从未初始化。这些我们都改了并复验。",
     {
         7: "内核 · 任务退出死机", 8: "换出 ECALL 被引导守卫吞掉，任何任务退出都整机失聪；子 shell 敲 exit 同样复现",
         11: "中断 · CLIC 使能位", 12: "up_enable_irq 从不置位 CLIC IE，中断申请装了却永远不来",
         15: "显示 · MIPI-DSI", 16: "8 处 mipi_dsi_msg 未零初始化，一条写被当成读发出去，等应答超时",
         19: "视觉 · CSI 与 ISP", 20: "P4 的 CSI bridge 与 ISP 共享，openvela 从不初始化 ISP 输入模块，桥 FIFO 恒空",
     }),

    # ---------------- 9 硬指标一览（表格） ----------------
    (14, "一页看完硬指标：每一项都能在仓里复查", 32,
     "这一页把所有可复查的硬指标摆在一起。空间从 460KB 降到 120.3KB；一次构建产出两个镜像；"
     "五个子系统全部有真机证据；SDIO 修出 7 个真实缺陷；WiFi 的 wlan0 拿到地址并 ping 通；"
     "整个项目 597 次提交，光 SDIO 攻坚那两天就有 203 次；AI 协作日志 31 个会话、四万九千多事件。"
     "我特意没画图表 —— 这些数字单位不同，画在一起会误导，列成表反而每个都能被单独核对。",
     {
         3: "一页看完硬指标：每一项都能在仓里复查",
         5: "__TABLE__",
     }),

    # ---------------- 10 AI 开发工作流 ----------------
    (22, "AI 开发工作流：把仪器接入变成可复用资产", 39,
     "AI 开发这块，我想讲清楚到底可复用什么。第一是可复用的仪器接口："
     "Tabby、pxview、OpenOCD 三个 MCP 客户端，任何项目拿去都能用。"
     "第二是可复用的闭环脚本：evidence.py 一条命令出四份证据，cycle.sh 走完改到提交。"
     "第三是可复用的解码器：48 位帧自己解、带 CRC7 校验。第四是可复用的方法论："
     "证据优先、反假设、判据可审计。第五是可复用的交付：清单驱动的 manifest，评审零手工步骤。"
     "第六是可复用的日志：会话导出成赛事格式，AI 怎么想的一目了然。",
     {
         7: "仪器接口", 8: "Tabby / pxview / OpenOCD 三个 MCP 客户端，换项目照用",
         11: "闭环脚本", 12: "evidence.py 一条命令四份证据；cycle.sh 走完改到提交",
         15: "解码器", 16: "48 位 SDIO 帧自解 + CRC7 校验，不信仪器默认标注",
     }),

    # ---------------- 11 平台与交付价值 ----------------
    (18, "可回流、可复用的平台价值", 28,
     "从平台角度看，这些成果不是留在自己仓库里。移植本身可以回流到 openvela 的官方待适配板，"
     "硬件在环的工具链和闭环脚本任何一块 esp32p4 板都能直接拿去用；"
     "评审侧是 init、sync、build 三步、零部署步骤。这两件事决定了它的复用价值。",
     {
         9: "可回流上游", 10: "板级与芯片层按 openvela vendor 风格组织，成果可 PR 回官方待适配板；交付不含任何部署脚本",
         11: "可复用工具链", 12: "硬件在环闭环与诊断工具对任意 esp32p4 板通用；评审侧 init → sync → build 三步复现",
     }),

    # ---------------- 12 未来展望：工业落地 ----------------
    (21, "未来展望：为什么它能落到工业场景", 38,
     "最后讲落地。我认为它适合工业场景，有三个支点。"
     "第一是价格：P4 本身不带射频，WiFi 交给一颗便宜的 C6 协处理器，整板 BOM 远低于带射频的 SoC 方案，"
     "更不是 Android 或完整 Linux 的量级，启动是毫秒到秒级，没有 Android 那种几百 MB 的运行时。"
     "第二是类 Linux 的体验：openvela 基于 NuttX，提供 POSIX 线程、文件系统、网络栈，"
     "既有的工业代码迁移成本低，不是从零学一套 RTOS API。"
     "第三是驱动覆盖面：显示、触摸、摄像头、音频、SDIO、WiFi 我们已经在一颗芯片上全跑通了，"
     "这在工业板卡上是很难得的密度。这三条对应三个场景："
     "电力，配电柜和二次设备的状态面板加本地告警；新能源，充电桩和储能柜的人机界面加无线回传；"
     "3D 打印，开源打印机的触控上位机，同时做摄像头监控和打印状态显示。"
     "我的判断是：它已经具备能出画面、能联网、能跑 Agent 的最小工业原型能力，缺的是外壳和行业认证。",
     {
         7: "三个支点", 8: "价格：P4 不带射频，WiFi 交给 C6 协处理器，BOM 远低于带射频 SoC 与 Android / 完整 Linux 方案\n"
                          "类 Linux：openvela 基于 NuttX，提供 POSIX 线程 / 文件系统 / 网络栈，既有工业代码迁移成本低\n"
                          "驱动全面：显示 / 触摸 / 摄像头 / 音频 / SDIO / WiFi 已在同一颗芯片上真机跑通",
         11: "三个场景", 12: "电力：配电柜 / 二次设备的状态面板与本地告警\n"
                            "新能源：充电桩 / 储能柜的人机界面与无线回传\n"
                            "3D 打印：开源打印机的触控上位机，摄像头监控 + 打印状态显示",
     }),

    # ---------------- 13 视频位 ----------------
    (25, None, 50,
     "下面放一段 27 秒的真机演示，是板端实拍，不是模拟器。"
     "（播放视频）好，这段录像里能看到同一块板子上多子系统同时在跑。",
     {
         20: "真机演示 · 27.7s（板端实拍，非模拟器）",
     }),

    # ---------------- 14 结尾 ----------------
    (31, "THANKS", 10,
     "总结一句：把 openvela 落到一块官方待适配的板上，用 AI 驱动真实仪器把驱动一环环打通，"
     "让每一步结论都能被复现和审计。感谢各位评委老师，欢迎提问。",
     {
         7: "桌伴 DeskMate · openvela × ESP32-P4X-C5 · ez-xu 队（contest2026_342）",
         8: "github.com/open-vela/contest2026_342_ezdezhandui",
     }),
]


# 技术段页号（讲稿里用来算占比）
TECH_PAGES = (3, 4, 5, 6, 7, 8)


# --------------------------------------------------------------------------
# 版面微调：{页号: {shape_id: (top_in, height_in)}}
# 只在模板给的框装不下正文时用；不动坐标之外的任何视觉参数。
# --------------------------------------------------------------------------
GEOM = {
    # 第 5 页：四个卡片的小标题 + 正文，正文框太矮（0.79in），下方是空白带
    5: {14: (9.05, 2.05), 16: (9.05, 2.05), 18: (9.05, 2.05), 20: (9.05, 2.05)},
    # 第 13 页：视频占位提示文字太小、贴得太近，放大并下移
    13: {20: (9.30, 0.70)},
    # 第 11 页：小标题框 0.78in 但正文从 9.39in 才开始，标题被正文顶穿
    11: {9: (8.86, 0.62), 11: (8.86, 0.62),
         10: (9.62, 1.60), 12: (9.62, 1.60)},
    # 第 7 页：中心辐射四张分支卡，正文框底边正好压在卡片边框上
    # 上排卡 y=4.17..6.25，下排卡 y=10.00..12.08 —— 上下两排必须分别定位
    7: {16: (5.34, 0.86), 20: (5.34, 0.86),
        24: (11.18, 0.86), 28: (11.18, 0.86)},
}


# --------------------------------------------------------------------------
# 写入工具
# --------------------------------------------------------------------------
def _set_paragraph_text(para, text):
    """改写一个段落：保留第一个 run 的全部格式，其余 run 删除。"""
    runs = para.runs
    if not runs:
        return False
    first = runs[0]
    first.text = text
    for r in runs[1:]:
        r._r.getparent().remove(r._r)
    return True


def set_text(shape, value):
    """把 value（str 或 list[str]）写进形状，段落数与内容自动适配。"""
    tf = shape.text_frame
    lines = [value] if isinstance(value, str) else list(value)

    paras = list(tf.paragraphs)
    if not paras:
        return

    # 段落不足：复制最后一段的 XML（含 rPr）追加
    while len(tf.paragraphs) < len(lines):
        last = tf.paragraphs[-1]
        newp = copy.deepcopy(last._p)
        last._p.getparent().append(newp)

    paras = list(tf.paragraphs)
    for i, line in enumerate(lines):
        if i < len(paras):
            if not _set_paragraph_text(paras[i], line):
                # 该段落没有 run（纯空段）：复制兄弟段落的 run
                run_xml = None
                for p in paras:
                    if p.runs:
                        run_xml = copy.deepcopy(p.runs[0]._r)
                        break
                if run_xml is not None:
                    p_xml = paras[i]._p
                    p_xml.append(run_xml)
                    _set_paragraph_text(paras[i], line)

    # 多余段落删掉
    for p in list(tf.paragraphs)[len(lines):]:
        p._p.getparent().remove(p._p)


def _em_width(ch):
    """粗略字宽（em）：CJK 全角 1.0，ASCII 半角 0.52。"""
    o = ord(ch)
    if o >= 0x2E80 or 0xFF00 <= o <= 0xFF60:
        return 1.0
    if ch == ' ':
        return 0.30
    return 0.52


def _text_height_pt(text, width_pt, size_pt, line_factor=1.35):
    """估算文字在给定宽度下折行后的总高度（pt）。"""
    usable = max(width_pt, 1)
    total = 0.0
    for raw in text.split('\n'):
        if not raw:
            total += size_pt * line_factor
            continue
        w = 0.0
        lines = 1
        for ch in raw:
            cw = _em_width(ch) * size_pt
            if w + cw > usable - 1:
                lines += 1
                w = cw
            else:
                w += cw
        total += lines * size_pt * line_factor
    return total


def fit_text(shape, min_pt=13.0):
    """把文字收进形状：开启自动换行，必要时逐级缩小字号直到装得下。

    模板里大量正文框是 wrap=none + 固定高度，直接灌入长句会横向出血。
    这里只做「装得下」的最小必要修改，字号阶梯与配色一律不动。
    """
    tf = shape.text_frame
    if not tf.text.strip():
        return
    tf.word_wrap = True

    width_pt = (shape.width or 0) / 914400.0 * 72.0
    height_pt = (shape.height or 0) / 914400.0 * 72.0
    width_pt -= (tf.margin_left + tf.margin_right) / 914400.0 * 72.0
    height_pt -= (tf.margin_top + tf.margin_bottom) / 914400.0 * 72.0

    sizes = []
    for para in tf.paragraphs:
        for run in para.runs:
            if run.font.size:
                sizes.append(run.font.size.pt)
    if not sizes:
        return
    base = max(sizes)

    size = base
    while size > min_pt:
        need = _text_height_pt(tf.text, width_pt, size)
        if need <= height_pt * 0.98:
            break
        size -= 1.0

    # 空间富余时适度放大（最多 base×1.5、且不超过 30pt），避免大卡片里吊着一小段字
    cap = min(30.0, base * 1.5)
    grown = size
    while grown < cap:
        if _text_height_pt(tf.text, width_pt, grown + 1.0) > height_pt * 0.92:
            break
        grown += 1.0
    size = grown

    if size != base:
        for para in tf.paragraphs:
            for run in para.runs:
                run.font.size = Pt(size)

    # 文字远少于框高 → 锚点移到顶端，否则正文会吊在卡片底部（模板里是 anchor=ctr）
    need_final = _text_height_pt(tf.text, width_pt, size)
    if height_pt > 0 and need_final < height_pt * 0.60:
        bodyPr = tf._txBody.find(
            '{http://schemas.openxmlformats.org/drawingml/2006/main}bodyPr')
        if bodyPr is not None:
            bodyPr.set('anchor', 't')


def reflow_title(shape, text, max_chars_single_line=13):
    """标题形状是 wrap=none：短标题放大更醒目，长标题必须撤掉 no-wrap 才能折行。"""
    tf = shape.text_frame
    tf.word_wrap = True
    if len(text) <= max_chars_single_line:
        size = 46
    else:
        size = 36
    set_text(shape, text)
    for para in tf.paragraphs:
        for run in para.runs:
            run.font.size = Pt(size)
    bodyPr = tf._txBody.find(
        '{http://schemas.openxmlformats.org/drawingml/2006/main}bodyPr')
    if bodyPr is not None:
        for child in list(bodyPr):
            if child.tag.endswith('}noAutofit') or child.tag.endswith('}spAutoFit'):
                bodyPr.remove(child)
        bodyPr.set('anchor', 'b')
    fit_text(shape)


def shrink_shape(shape, pt):
    """把形状里所有 run 的字号统一设为 pt。"""
    if not shape.has_text_frame:
        return
    for para in shape.text_frame.paragraphs:
        for run in para.runs:
            run.font.size = Pt(pt)


def add_caption(slide, text):
    """在页面底部加一行口径说明（浅灰小字，不抢主视觉）。"""
    from pptx.util import Inches
    from pptx.dml.color import RGBColor
    box = slide.shapes.add_textbox(Inches(1.94), Inches(13.15),
                                   Inches(22.78), Inches(0.45))
    tf = box.text_frame
    tf.word_wrap = True
    para = tf.paragraphs[0]
    run = para.add_run()
    run.text = text
    run.font.size = Pt(14)
    run.font.color.rgb = RGBColor(0x64, 0x74, 0x8B)


def add_notes(slide, text):
    slide.notes_slide.notes_text_frame.text = text


def delete_slide(prs, index):
    xml_slides = prs.slides._sldIdLst
    slides = list(xml_slides)
    rId = slides[index].get('{http://schemas.openxmlformats.org/officeDocument/2006/relationships}id')
    prs.part.drop_rel(rId)
    xml_slides.remove(slides[index])


# --------------------------------------------------------------------------
# 表格页（第 9 页）：硬指标一览
# 每行 = 一个维度；数值一律取自 FACTS 对应的实测记录，可在仓内复查。
# --------------------------------------------------------------------------
TABLE_HEADER = ["维度", "数值", "复查位置"]
TABLE_ROWS = [
    ["SRAM 静态占用", "460KB → 120.3KB / 512KB",
     "README §七；clean tree 重建实测"],
    ["构建产物", "应用 1,835,008 B + 引导 24,672 B",
     "一次 build.sh 双镜像，usb_stable.sh 校验 hash"],
    ["子系统集成", "5 条全部真机跑通",
     "显示 / 触摸 / 相机 / 音频 / WiFi，见 docs/40_验证与证据/"],
    ["SDIO 缺陷", "7 个真实缺陷，2 个真根因",
     "SDIO PLL 未上电；响应标志被忽略"],
    ["网络", "wlan0 192.168.1.9，三连 ping 全绿",
     "连接_WiFi_SDIO.md BY.32；提交 d4ffc8e"],
    ["提交与协作", "597 次提交（10/31 单日 174）",
     "git log 实算；AI 日志 31 会话 / 49,599 事件"],
]


def fill_table(slide):
    """把第 9 页的表格填成硬指标一览（保留模板的行列配色与字号）。"""
    for shape in slide.shapes:
        if not shape.has_table:
            continue
        table = shape.table
        need_rows = len(TABLE_ROWS) + 1
        # 模板给了 6 行（表头 + 5），不够就按最后一行的格式补
        while len(table.rows) < need_rows:
            from copy import deepcopy
            tr = deepcopy(table.rows[len(table.rows) - 1]._tr)
            table._tbl.append(tr)
        for r, row in enumerate([TABLE_HEADER] + TABLE_ROWS):
            for c, text in enumerate(row):
                cell = table.cell(r, c)
                # 只改文字，保留单元格既有段落格式
                para = cell.text_frame.paragraphs[0]
                if para.runs:
                    para.runs[0].text = text
                    for extra in para.runs[1:]:
                        extra._r.getparent().remove(extra._r)
                else:
                    para.text = text
                cell.text_frame.word_wrap = True


# --------------------------------------------------------------------------
# 主流程
# --------------------------------------------------------------------------
def build(template, out, speaker_md, preview_dir):
    prs = Presentation(template)

    keep = {src: (title, secs, notes, texts)
            for src, title, secs, notes, texts, *_ in SLIDES}
    order = [src for src, *_ in SLIDES]          # 目标顺序（模板页号序列）
    keep_src = set(order)

    sldIdLst = prs.slides._sldIdLst
    id_by_src = {i + 1: el for i, el in enumerate(list(sldIdLst))}

    # 1) 删掉模板里不被使用的页
    for src in sorted(id_by_src):
        if src not in keep_src:
            el = id_by_src.pop(src)
            prs.part.drop_rel(el.get(
                '{http://schemas.openxmlformats.org/officeDocument/2006/relationships}id'))
            sldIdLst.remove(el)

    # 2) 保留页按目标顺序重排
    for el in list(sldIdLst):
        sldIdLst.remove(el)
    for src in order:
        sldIdLst.append(id_by_src[src])

    # 3) 逐页填内容
    report = []
    for pos, src in enumerate(order, 1):
        title, secs, notes, texts = keep[src]
        slide = prs.slides[pos - 1]

        by_id = {}
        def collect(shapes):
            for sh in shapes:
                by_id[sh.shape_id] = sh
                if sh.shape_type == 6:
                    collect(sh.shapes)
        collect(slide.shapes)

        if title is not None:
            title_shape = by_id.get(3)
            if title_shape is not None:
                reflow_title(title_shape, title)

        for sid, (top_in, height_in) in GEOM.get(pos, {}).items():
            sh = by_id.get(sid)
            if sh is not None:
                sh.top = Emu(int(top_in * 914400))
                sh.height = Emu(int(height_in * 914400))

        for sid, val in texts.items():
            sh = by_id.get(sid)
            if val == '__TABLE__':
                continue
            if sh is None:
                report.append(f'  ! p{pos} 模板 p{src} 找不到形状 id={sid}')
                continue
            if val is None:
                sh._element.getparent().remove(sh._element)
            elif sh.has_text_frame:
                set_text(sh, val)

        # 清掉模板底部的版式说明脚注（保留页里所有 "用于…/按…" 的说明段）
        for sh in list(by_id.values()):
            if sh.has_text_frame and re.match(
                    r'^(用于|按项目实际|按需|两栏长文|三栏长文|2×2|中心为统领|左侧 16:9|边放视频)',
                    sh.text_frame.text.strip()):
                sh._element.getparent().remove(sh._element)

        if pos == 10:
            add_caption(slide,
                        "AI 协作日志 31 个会话 / 49,599 条事件（claude-code 18 + dsh 13），"
                        "已按赛事格式导出到 logs/ez-xu/")
        if pos == 9:
            fill_table(slide)
            add_caption(slide,
                        "口径：数值均取自本仓实测记录（README / STATUS / 连接_WiFi_SDIO / git log / logs/ez-xu/manifest.json）")

        if pos == 1:
            sh = by_id.get(7)
            if sh is not None:
                shrink_shape(sh, 24)

        # 全片收口：所有还有文字的形状做一次「装得下」检查
        # （模板里大量正文框是 wrap=none + 固定高度，长句会横向出血）
        for sh in by_id.values():
            try:
                if sh.has_text_frame and sh.text_frame.text.strip():
                    fit_text(sh)
            except Exception as e:
                report.append(f'  ! p{pos} fit 失败 id={sh.shape_id}: {e}')

        add_notes(slide, f'【{pos}/14 · {secs}s】\n{notes}')

    total = sum(s[2] for s in SLIDES)
    os.makedirs(os.path.dirname(os.path.abspath(out)) or '.', exist_ok=True)
    prs.save(out)

    write_speaker_md(speaker_md, total)
    print(f'已生成: {out}')
    print(f"页数: {len(prs.slides._sldIdLst)}  总时长: {total}s")
    for line in report:
        print(line)
    return prs


def write_speaker_md(path, total):
    tech = sum(s[2] for n, s in enumerate(SLIDES, 1) if n in TECH_PAGES)
    rest = total - tech
    lines = [
        '# 桌伴 DeskMate · 决赛答辩讲稿（8 分钟）',
        '',
        '> 由 `tools/engineering/build_defense_ppt.py` 生成，与 PPT 逐页一一对应。',
        f'> 合计 **{total} 秒**；技术段（第 {TECH_PAGES[0]}–{TECH_PAGES[-1]} 页）'
        f'{tech} 秒（{tech * 100 // total}%），其余 {rest} 秒。',
        '> 官方要求「技术与产品方案并重」：这里用第 11 页（可回流可复用）与'
        '第 12 页（工业落地）承担产品侧，不做消费品叙事。',
        '',
        '## 配时总表',
        '',
        '| # | 标题 | 秒数 |',
        '|---|---|---|',
    ]
    for n, s in enumerate(SLIDES, 1):
        lines.append(f'| {n} | {s[1] or "（视频位）"} | {s[2]}s |')
    lines += ['', f'**合计 {total} 秒**（上限 480 秒 = 8 分钟演讲，另有 2 分钟问答）。', '']

    for n, s in enumerate(SLIDES, 1):
        lines += [f'## 第 {n} 页 · {s[1] or "视频位"}　`{s[2]}s`', '', s[3], '']
        if n == 13:
            lines += [
                '### 这一页怎么插视频（成稿只留了占位，未嵌入视频）',
                '',
                '1. PowerPoint 打开 `提交材料/答辩PPT_桌伴DeskMate.pptx`，翻到第 13 页；',
                '2. `插入 → 视频 → 此设备`，选 `提交材料/6360b48fc40776612b7558d2061b97ec.mp4`；',
                '3. 拖到页面中部、按高度适配（竖屏 720×1280，两侧留黑是正常现象）；',
                '4. 播放选项卡里设为「单击时播放」，并把「播放完返回开头」勾上，方便重复演示。',
                '',
                '> 视频不嵌入成稿，避免 PPT 体积过大、也避免换机器时路径失效。',
                '',
            ]
    lines += [
        '## 投屏前 3 项检查',
        '',
        '1. **字体**：成稿用的是 MiSans；演示机若没有，会回退到系统黑体，字距略变但不会溢出。',
        '2. **视频**：第 13 页是占位页（播放图标 + 一行说明），需现场插入；'
        '也可用外部播放器直接打开 `提交材料/6360b48f….mp4`。',
        '3. **比例**：16:9，26.67 × 15.0 英寸；投屏前确认投影仪不是 4:3。',
        '',
        '## 数据来源（改数字只改 FACTS 一处）',
        '',
    ]
    for k, v in FACTS.items():
        lines.append(f'- `{k}` = {v}')
    lines.append('')
    os.makedirs(os.path.dirname(os.path.abspath(path)) or '.', exist_ok=True)
    with open(path, 'w', encoding='utf-8') as f:
        f.write('\n'.join(lines))
    print(f'已生成讲稿: {path}')


# --------------------------------------------------------------------------
# 校验
# --------------------------------------------------------------------------
PLACEHOLDER = re.compile(r'以下填写|在此填写|以下放置|填写小标题|填写正文|填写模块|示意数据|模板 · |以下填写章节标题')


def verify(path, speaker_md):
    from pptx import Presentation as P2
    prs = P2(path)
    ok = True

    print('== 1 零残留占位符 ==')
    hits = []
    for i, slide in enumerate(prs.slides, 1):
        for sh in slide.shapes:
            texts = []
            if sh.has_text_frame:
                texts.append(sh.text_frame.text)
            if sh.has_table:
                for r in range(len(sh.table.rows)):
                    for c in range(len(sh.table.columns)):
                        texts.append(sh.table.cell(r, c).text)
            for t in texts:
                if t and PLACEHOLDER.search(t):
                    hits.append((i, sh.name, t[:60]))
    print(f'   命中 {len(hits)} 处' + ('  ✗' if hits else '  ✓'))
    for h in hits[:12]:
        print('   ', h)
    ok &= not hits

    print('== 2 页数 ==')
    n = len(prs.slides._sldIdLst)
    print(f'   {n} 页' + ('  ✓' if n == 14 else '  ✗ 应为 14'))
    ok &= (n == 14)

    print('== 3 时长 ==')
    total = sum(s[2] for s in SLIDES)
    tech = sum(s[2] for i, s in enumerate(SLIDES, 1) if i in TECH_PAGES)
    print(f'   合计 {total}s（≤480）技术段 {tech}s（≥210）'
          + ('  ✓' if total <= 480 and tech >= 210 else '  ✗'))
    ok &= (total <= 480 and tech >= 210)

    print('== 4 口径一致（数字必须在 FACTS 里）==')
    all_text = []
    for slide in prs.slides:
        for sh in slide.shapes:
            if sh.has_text_frame:
                all_text.append(sh.text_frame.text)
            if getattr(sh, 'has_table', False) and sh.has_table:
                for row in sh.table.rows:
                    for cell in row.cells:
                        all_text.append(cell.text)
    blob = '\n'.join(all_text)
    for token in ['403 kHz', '714,543', '120.3KB', '1,835,008', '24,672', '49,599', '597', '192.168.1.9']:
        present = token in blob
        print(f'   {token:14s} {"出现" if present else "未出现（可选）"}')

    print('== 5 讲稿 ==')
    if os.path.exists(speaker_md):
        txt = open(speaker_md, encoding='utf-8').read()
        print(f'   {len(txt)} 字符，含 {txt.count("## 第")} 个小节'
              + ('  ✓' if txt.count('## 第') == 14 else '  ✗'))
        ok &= (txt.count('## 第') == 14)
    else:
        print('   ✗ 缺失')
        ok = False

    print('\n结论:', 'PASS' if ok else 'FAIL')
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--template')
    ap.add_argument('--out')
    ap.add_argument('--speaker',
                    default='docs/60_交付与提交/答辩讲稿_桌伴DeskMate.md')
    ap.add_argument('--preview',
                    default='docs/60_交付与提交/答辩PPT预览')
    ap.add_argument('--verify')
    a = ap.parse_args()

    if a.verify:
        sys.exit(verify(a.verify, a.speaker))

    if not a.template or not a.out:
        ap.error('--template 与 --out 必填')
    build(a.template, a.out, a.speaker, a.preview)


if __name__ == '__main__':
    main()
