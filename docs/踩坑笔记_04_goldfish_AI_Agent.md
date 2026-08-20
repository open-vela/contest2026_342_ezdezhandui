# 踩坑笔记 #04：goldfish 模拟器跑通 openvela AI Agent（含交互通道血泪史）

> 2026-08-12。目标：官方 AI Agent（packages/ai_agent）在 goldfish-arm64-v8a-ap 跑通。**结论先行：已跑通，ask 命令 Agent 正确应答（工具调用链路工作）。**

## 一次成功的最小路径（直接照抄）

```bash
cd openvela
# 1. 启用 AI Agent（写入板级 defconfig）
echo "CONFIG_EXAMPLES_AI_AGENT_VELA=y" >> vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap/defconfig

# 2. 构建（goldfish arm64 全量约 32 分钟）
PATH=/usr/bin:$PATH ./build.sh vendor/openvela/boards/vela/configs/goldfish-arm64-v8a-ap/ --cmake -j8

# 3. 运行：关键 = 管道喂 stdin + -qemu -nographic（见坑 2/3）
(sleep 40; printf "ai_agent help\n"; sleep 10; printf "ask 现在几点了\n"; sleep 15; printf "quit\n") \
  | ./emulator.sh vela -no-window -no-audio -gpu off -qemu -nographic > /tmp/emu.log 2>&1

# 4. 验证输出：出现 vela> 提示符 + [Agent]: 应答
```

## 坑 1：python2 抢了 CMake 的 Python3（老坑重现，笔记 #01 坑 4）

- `Could NOT find Python3 ... Wrong major version for "/usr/local/bin/python"`（/usr/local/bin/python → python2.7，PATH 里 /usr/local/bin 在前）
- 笔记 #01 的解法：`PATH=/usr/bin:$PATH ./build.sh ...`；本次用等效软链 `ln -sf /usr/bin/python3 ~/.local/bin/python`（~/.local/bin 在 PATH 最前）

## 坑 2：expect/pty 无法输入 guest console，管道可以

- **现象**：expect spawn 模拟器（pty），NSH 提示符出现在 stdout，但 send 命令无回显无响应
- **根因**：Android emulator 从 stdin 读自己的 console 命令，非 tty stdin（管道）时才透传给 guest；pty 时被拦截
- **正解**：`printf "...\n" | ./emulator.sh vela ...`（管道喂 stdin 生效，有回显有响应）

## 坑 3：guest 控制台输入需要 `-qemu -nographic`

- 不加 -nographic：只有 stdout 输出，stdin 不达 guest（即使 -no-window）
- 加 `-qemu -nographic` + stdin 管道 = 完整交互

## 坑 4：adb / telnet 通道均不可用

- `adb shell`：NuttX adbd 收到 "error: closed"（adbd 功能有限，不支持 shell pty）；adb devices 能识别（emulator-5554）
- `telnet 2323`（hostfwd 到 guest telnetd）：连接即被服务器关闭（telnetd shell 启动失败？未深究）
- 模拟器 console 5554：auth 后只有 Android console 命令（event/gsm/network 等），**无 guest 串口转发**
- 结论：**stdin 管道 + -nographic 是唯一可靠交互通道**

## 坑 5：pkill/ps 自误杀（三次！）

- `pkill -f "emulator.*vela"`、`ps | grep emulator | xargs kill`——命令行里含匹配模式会**杀掉当前 shell 自己**（exit 144）
- 正解：grep 用 `[q]emu` 字符类技巧，或先 ps 列出 PID 再单独 kill；或 console `auth + kill`

## 坑 6：AVD 冲突（多模拟器互斥）

- 旧模拟器没死透就启动新的 → `ERROR | Running multiple emulators with the same AVD`（需 -read-only）
- 换配置重跑前：先杀干净（`ps -eo pid,cmd | grep -iE "[q]emu"` 逐个 kill）

## 坑 7：cwd 漂移

- Bash 会话 cwd 会重置；`./emulator.sh`、build.sh 依赖 openvela 根 cwd → 用 `cd /home/ez/share/rk3576-openvela/openvela && ...` 或绝对路径

## 已验证的事实（T1 生死线，2026-08-12）

- ✅ `help` 命令列表含 `ai_agent`（EXAMPLES_AI_AGENT_VELA 编译进镜像）
- ✅ `ai_agent help` → `ifup eth0...OK`（QEMU user 网络 DHCP 自动配置成功）→ `vela>` 提示符
- ✅ `ask 现在几点了` → `[Agent]: 2026-08-12 10:53:39 CST (UTC+8)`（**无 LLM key 也答**：内置 get_current_time 工具，ReAct 链路工作）
- ✅ 完整命令面：set_llm（kimi/qwen/deepseek/glm/openai/mimo…）、set_vision_llm、memory_read/write、session、voice（火山 ASR/TTS）、mqtt、node、router、install_skill、mcp_*、cron
- ⚠️ `list_models` 仅 openrouter 后端可用；set_llm 的完整预设列表待配 key 后实测（README 声明 8 后端含 mimo）
- ⚠️ voice 通道、camera 工具（AI_AGENT_CAMERA 未启用——defconfig 未开 VIDEO）待 T2 开启测试

## 下一步（T2）

1. MiMo/DeepSeek key 配置后 `set_llm` + 真实对话（云端 LLM 路径）
2. 断网降级验证（QEMU 断网 / 拔网）
3. LVGL UI 通道（AI_AGENT_LVGL_UI 默认 n，需 GRAPHICS_LVGL；goldfish 有 virtio-gpu）
4. 自定义工具：GPIO 灯控（看 tools 注册方式，src/tools/）
