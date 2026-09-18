# ESP32-P4 USB 直通稳定性工具

## 背景:USB 直通"不稳定"的根因(已调查确认)

调查结论(2026-09-01):

1. **设备无自发断开**:4.5 分钟监控 devnum 保持稳定,零自发断开。
2. **断开 = esptool `--before default_reset` 触发**:每次 esptool 连接都硬件复位→设备重新枚举(正常现象),复位瞬间 USB 短暂消失。
3. **复位后立即稳定**:esptool 复位后 3 秒 console 正常(实测 47B NSH)。
4. **CP210x(同总线)零断开**:排除 VMware USB 直通/总线问题。
5. **OpenOCD esp_usb_jtag 打不开 ≠ USB 问题**:固件运行时 USB-JTAG 的 JTAG 功能被占用,仅 ROM bootloader 模式可用(可用 esptool 串口代替)。
6. **供电隐患**:设备请求 500mA,USB 1.1 端口供电可能不足(运行时电流峰值),极端时可能掉电(未见,但建议独立供电)。

**"不稳定"真实体验来源**:每次 esptool 复位→设备短暂消失→此时访问失败;复位后需 2-3 秒稳定。高频 esptool+console 操作放大观感。

## 工具

### usb_stable.sh
```bash
bash tools/usb_stable.sh [firmware.bin] flash   # 烧录(带重试+复位后等待)
bash tools/usb_stable.sh <firmware.bin> chip_id # 连接测试
bash tools/usb_stable.sh <firmware.bin> reset   # 复位+等待稳定
```
核心:每次 esptool 操作后 `wait_stable`(等 ttyACM0 出现+可打开+3 秒缓冲),杜绝"复位后立即访问失败"。

### verify_aiagent.sh
```bash
bash tools/verify_aiagent.sh [firmware.bin]      # 复位→启动 ai_agent→多次读取
```
检测 ai_agent READY(vela>)标志。

## 经验
- 任何 USB 操作后 **sleep 3-5 秒**再访问(设备重枚举需要时间)
- console 用**单连接持续读写**,避免反复 open/close(ttyACM0 反复开关会触发设备状态变化)
- esptool 避免高频重复 chip_id(每次触发复位枚举)
- 如需 JTAG/内存诊断(OpenOCD),需在 ROM bootloader 模式(esptool 保持下载),固件运行时不可用
