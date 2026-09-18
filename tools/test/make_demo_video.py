#!/usr/bin/env python3
"""桌伴 DeskMate 演示视频生成器（证据版 v1）
用真机素材合成 ≤5 分钟 1080p mp4：
  片头 → 移植硬货(MCUboot+XIP) → 系统启动 → LLM 对话 → 显示触摸 → 定时主动
  → 摄像头诚实说明 → 复现路径 → 片尾
依赖: Pillow + imageio-ffmpeg（或系统 ffmpeg）
"""
import os, subprocess, sys, textwrap

def find_ffmpeg():
    for c in ("ffmpeg", "/usr/bin/ffmpeg", "/usr/local/bin/ffmpeg"):
        if os.path.exists(c):
            return c
    try:
        import imageio_ffmpeg
        return imageio_ffmpeg.get_ffmpeg_exe()
    except Exception:
        return "ffmpeg"

FFMPEG = find_ffmpeg()

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "提交材料", "演示视频_桌伴DeskMate_v1.mp4")
TMP = "/tmp/deskmate_video"
os.makedirs(TMP, exist_ok=True)

W, H = 1920, 1080
FPS = 30
BG = (10, 14, 24)
ACCENT = (46, 116, 181)
WHITE = (235, 240, 248)
GRAY = (150, 160, 180)
GREEN = (80, 200, 120)
AMBER = (230, 170, 60)

from PIL import Image, ImageDraw, ImageFont

def _has_cjk(path):
    """判断字体是否含中文字形（决定能否用于中文界面）。"""
    try:
        f = ImageFont.truetype(path, 24)
    except Exception:
        return False
    try:
        return f.getmask("桌").getbbox() is not None
    except Exception:
        return False


def font(sz, bold=False):
    """按「先中文、后西文」的顺序挑字体。

    注意顺序：DejaVu 不含 CJK 字形，若排在前面会让全部中文渲染成豆腐块（□）。
    故先找含中文的字体（Noto Sans CJK / 文泉驿），找不到再退回 DejaVu。
    """
    cjk = [
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc" if bold
        else "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/arphic/uming.ttc",
    ]
    latin = [
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf" if bold
        else "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    ]
    for p in cjk + latin:
        if os.path.exists(p) and (p in latin or _has_cjk(p)):
            try:
                return ImageFont.truetype(p, sz)
            except Exception:
                continue
    return ImageFont.load_default()

F_BIG = font(72, True)
F_TITLE = font(60, True)
F_H = font(42, True)
F_BODY = font(30)
F_SMALL = font(24)
F_MONO = font(26)

def new_frame():
    img = Image.new("RGB", (W, H), BG)
    return img, ImageDraw.Draw(img)

def text_center(d, y, s, f, color=WHITE):
    w = d.textlength(s, font=f)
    d.text(((W - w) / 2, y), s, font=f, fill=color)

def draw_header(d, title, sub=None):
    d.rectangle([0, 0, W, 10], fill=ACCENT)
    d.text((60, 40), title, font=F_H, fill=WHITE)
    if sub:
        d.text((60, 96), sub, font=F_SMALL, fill=GRAY)
    d.line([60, 150, W - 60, 150], fill=(40, 52, 74), width=2)

def draw_logbox(d, lines, y0=190, x0=80, w=W-160, h=640, title=None):
    if title:
        d.text((x0, y0 - 34), title, font=F_SMALL, fill=AMBER)
    d.rectangle([x0, y0, x0 + w, y0 + h], outline=(40, 52, 74), width=2)
    d.rectangle([x0 + 8, y0 + 8, x0 + w - 8, y0 + h - 8], fill=(14, 20, 34))
    yy = y0 + 30
    for ln in lines[: int((h - 60) / 38)]:
        d.text((x0 + 22, yy), ln, font=F_MONO, fill=GREEN if "✓" in ln or "OK" in ln else WHITE)
        yy += 38

def kf(name):
    return os.path.join(TMP, name)

def scene_boot():
    img, d = new_frame()
    draw_header(d, "① openvela → ESP32-P4X 平台移植", "MCUboot 二级引导 + flash XIP（真机启动日志）")
    boot = open("/tmp/boot_check.log", encoding="utf-8", errors="replace").read().splitlines()
    keys = [ln for ln in boot if any(k in ln for k in
            ["Mapped IROM", "Loading image", "EK79007: panel initialized", "/dev/fb0 ready",
             "GT911: product ID", "SC2336 chip ID", "Camera registered", "NuttShell", "DNS nameserver"])][:12]
    draw_logbox(d, keys, title="nsh> reset  ← 真实启动日志（2026-09-18）")
    stats = [
        "SRAM 静态占用: 460KB → 120.3KB / 512KB (23.5%)",
        "text+rodata 迁入 16MB flash XIP 执行",
        "应用镜像 1,835,008 B + MCUboot 引导 24,672 B（一次构建）",
        "交付: 4 文件树 + 325 条 copyfile，repo sync 零部署",
    ]
    yy = 190 + 12 * 38 + 20
    for s in stats:
        d.text((80, yy), "▸ " + s, font=F_BODY, fill=ACCENT if False else WHITE)
        yy += 44
    img.save(kf("s_boot.png"))

def scene_agent():
    img, d = new_frame()
    draw_header(d, "② ai_agent 系统启动", "P0→P6 里程碑 + 12 Skills + 网络 + 定时主动")
    vlog = open(os.path.join(ROOT, "docs/demo/02_verify.log"), encoding="utf-8", errors="replace").read().splitlines()
    keys = [ln for ln in vlog if any(k in ln for k in
            ["P0:", "P1:", "P2:", "P3:", "P4:", "P5:", "P6:", "Skills system ready",
             "Cron started", "Network connected", "WebSocket server started"])][:14]
    draw_logbox(d, keys, title="nsh> ai_agent  ← 真实启动验证（verify_aiagent.sh）")
    yy = 190 + 14 * 38 + 20
    for s in ["✓ 36 tools / 12 skills（含自研 center-assistant、quick-note）",
              "✓ eth0 DHCP 192.168.1.105 / DNS 可用",
              "✓ cron 定时主动服务真实运行",
              "✓ PSRAM heap ≈ 33.9 MB"]:
        d.text((80, yy), s, font=F_BODY, fill=GREEN)
        yy += 44
    img.save(kf("s_agent.png"))

def scene_llm():
    img, d = new_frame()
    draw_header(d, "③ LLM 端到端对话（真机实测）", "WebSocket 28789 → 消息 → 小米 MiMo → 真实回答")
    chat = open(os.path.join(ROOT, "logs/verify-2026-09-17/agent_llm_ws.txt"), encoding="utf-8", errors="replace").read().splitlines()
    keys = [ln for ln in chat if ln.startswith("[ws]")][:6]
    draw_logbox(d, keys, title="tools/test/ws_demo.py 客户端实录（2026-09-17 晚）")
    yy = 190 + 6 * 38 + 20
    for s in ["★ 发出「你好」 → 板端调度 → LLM 返回「你好」",
              "修复链路：① Token Plan 端点 401 → ② NET_TCPBACKLOG RST → ③ TLS 改时钟吞答案",
              "诚实记录：一次问答 261s（推理模型 + 15KB 工具 schema），待提速"]:
        d.text((80, yy), s, font=F_BODY, fill=AMBER if s.startswith("★") else WHITE)
        yy += 44
    img.save(kf("s_llm.png"))

def scene_display():
    img, d = new_frame()
    draw_header(d, "④ 7\" 触控显示 + LVGL", "MIPI-DSI 1024×600（EK79007）+ GT911 触摸，真机渲染已核实")
    lv = os.path.join(ROOT, "docs/验证截图_LVGL界面_1024x600.png")
    if os.path.exists(lv):
        pv = Image.open(lv)
        pv = pv.resize((int(pv.width * 0.9), int(pv.height * 0.9)))
        img.paste(pv, ((W - pv.width) // 2, 170))
        d.text(((W - pv.width) // 2, 150), "真机帧缓冲导出（JTAG → PNG，1024×600）", font=F_SMALL, fill=GRAY)
    yy = 170 + int(600 * 0.9) + 40
    d.text((80, yy), "▸ /dev/fb0 ready 1024x600 RGB565（EK79007）   ▸ /dev/input0（GT911 触摸对位）   ▸ lvgldemo 真机渲染", font=F_BODY, fill=WHITE)
    img.save(kf("s_display.png"))

def scene_camera():
    img, d = new_frame()
    draw_header(d, "⑤ 摄像头：驱动链路全通（诚实说明）", "SC2336 识别 / CSI 2-lane + DMA / 零错误；物理链路待硬件")
    cam = open(os.path.join(ROOT, "logs/verify-2026-09-17/camera_final.log"), encoding="utf-8", errors="replace").read().splitlines()
    keys = [ln for ln in cam if any(k in ln for k in
            ["chip ID", "CSI:", "ctlr start", "start_capture", "DQBUF", "buf"])][:10]
    draw_logbox(d, keys, title="tools/test/camera_diag.sh 实录")
    yy = 190 + 10 * 38 + 20
    for s in ["✓ SCCB 通信 / 传感器表 149/149 / stream-on 成功",
              "✓ CSI 2 lane @405Mbps + DW-GDMA 武装 rc=0",
              "✗ MIPI 数据 lane 物理通路无数据（模组/排线）→ 换硬件后 camera_diag.sh 复测",
              "设计：桌伴感知「有人在」走帧统计，无需出图（轻量降级路径）"]:
        d.text((80, yy), s, font=F_BODY, fill=GREEN if s.startswith("✓") else AMBER if s.startswith("✗") else WHITE)
        yy += 44
    img.save(kf("s_camera.png"))

def scene_repro():
    img, d = new_frame()
    draw_header(d, "⑥ 复现路径（评审零手工步骤）", "GitHub 官方仓 · repo init → sync → build → 烧录 → console")
    cmds = [
        "repo init -u https://github.com/open-vela/contest2026_342_ezdezhandui.git \\",
        "  -b dev-ai-contest-2026 -m contest2026_342_ezdezhandui.xml",
        "repo sync -c -j8            # 325 条 copyfile 自动落位",
        "./build.sh vendor/espressif/boards/esp32p4/esp32p4-function-ev-board/configs/nsh --cmake -j8",
        "tools/build_flash/usb_stable.sh         # 0x2000 引导 + 0x20000 应用（双镜像 hash 校验）",
        "python3 tools/test/board.py reset  # → nsh>",
    ]
    draw_logbox(d, cmds, title="build.sh 一条命令 → 双镜像 → 真机 NSH")
    yy = 190 + 6 * 38 + 20
    for s in ["✓ 干净树实测：编译 2,426 步 / 06:32；依赖就绪后即可构建",
              "✓ 不删除任何上游文件、不放软链 —— 全部靠 copyfile 表达"]:
        d.text((80, yy), s, font=F_BODY, fill=GREEN)
        yy += 44
    img.save(kf("s_repro.png"))

def scene_title():
    img, d = new_frame()
    d.rectangle([0, 0, W, H], fill=(14, 20, 34))
    d.rectangle([0, 0, W, 10], fill=ACCENT)
    text_center(d, 260, "桌伴 DeskMate", F_TITLE, WHITE)
    text_center(d, 360, "openvela × ESP32-P4X 智能视觉中控屏", F_BODY, GRAY)
    text_center(d, 470, "「能看见 · 会主动 · 会执行」", F_BIG, ACCENT)
    text_center(d, 620, "2026 首届 openvela AI 硬件开发者大赛 · contest2026_342 · 队伍 ez-xu", F_SMALL, GRAY)
    text_center(d, 700, "新硬件平台适配（ESP32-P4X 官方待适配板）+ AI 硬件产品创新", F_SMALL, GRAY)
    img.save(kf("s_title.png"))

def scene_end():
    img, d = new_frame()
    d.rectangle([0, 0, W, H], fill=(14, 20, 34))
    d.rectangle([0, 0, W, 10], fill=ACCENT)
    text_center(d, 300, "谢谢观看", F_TITLE, WHITE)
    text_center(d, 440, "代码 / 文档 / AI 日志全量开源", F_BODY, GRAY)
    text_center(d, 560, "https://github.com/open-vela/contest2026_342_ezdezhandui", F_BODY, ACCENT)
    text_center(d, 680, "AI Coding 日志：28 会话 / 35,831 事件（logs/）", F_SMALL, GRAY)
    img.save(kf("s_end.png"))

def render():
    # 每场景生成一段视频（动态：字幕逐行显现用简单淡入+停留）
    scene_funcs = [scene_title, scene_boot, scene_agent, scene_llm,
                   scene_display, scene_camera, scene_repro, scene_end]
    durations = [4, 12, 10, 11, 8, 10, 10, 4]   # 秒
    parts = []
    for fn, dur in zip(scene_funcs, durations):
        fn()
        name = fn.__name__.replace("scene_", "s_")
        clip = os.path.join(TMP, name + ".mp4")
        subprocess.run([FFMPEG, "-y", "-loop", "1", "-i", kf(name + ".png"),
                        "-t", str(dur), "-r", str(FPS), "-vf",
                        f"scale={W}:{H},format=yuv420p,zoompan=z='min(zoom+0.0005,1.06)':d={FPS*dur}:s={W}x{H}:fps={FPS}",
                        "-c:v", "libx264", "-preset", "fast", "-crf", "22", clip],
                       check=True, capture_output=True)
        parts.append(clip)
    # 拼接
    lst = os.path.join(TMP, "list.txt")
    with open(lst, "w") as f:
        for p in parts:
            f.write(f"file '{p}'\n")
    subprocess.run([FFMPEG, "-y", "-f", "concat", "-safe", "0", "-i", lst,
                    "-c:v", "libx264", "-preset", "fast", "-crf", "22", OUT],
                   check=True, capture_output=True)
    print("OK", OUT)

if __name__ == "__main__":
    render()
