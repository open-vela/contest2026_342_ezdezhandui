# ESP-IDF SDIO 主机对照探针（P4 侧）

**目的**：用 **ESP-IDF 自己的 `sdmmc` 主机栈**读同一颗 C6，与 NuttX 驱动对照。

## 用法

```sh
idf.py set-target esp32p4 && idf.py build
python3 -m esptool --chip esp32p4 -p /dev/ttyACM0 -b 460800 write_flash \
  --flash_size 16MB 0x2000 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin 0x10000 build/p4sdiohost.bin
```

（会覆盖 NuttX 固件；用 `tools/build_flash/usb_stable.sh` 可以恢复。）

## 实测结果（2026-10-29）

```
sdmmc_card_init -> ESP_OK (0)                 <- 枚举成功
set_card_clk(400 kHz) -> ESP_OK
OPEN_DATA_PATH -> ESP_ERR_TIMEOUT             <- CMD52 写失败
LEN read err ESP_ERR_TIMEOUT                  <- CMD52 读失败
TOKEN read err ESP_ERR_TIMEOUT
read60 -> ESP_ERR_TIMEOUT                     <- CMD53 失败
```

## 与 NuttX 驱动的对照

| 环节 | NuttX 驱动 | **ESP-IDF 主机栈** |
|---|---|---|
| 枚举 | ✅ 成功 | ✅ 成功 |
| **CMD52 寄存器读** | ✅ **读到合法值** | 🔴 **全部超时** |
| **CMD52 写（握手）** | ✅ **CP 收到并响应** | 🔴 **超时** |
| CMD53 数据阶段 | 🔴 超时 | 🔴 超时 |

**同一块板、同一颗 C6、同一组引脚，IDF 的栈连寄存器都读不到，
而 NuttX 驱动读到了合法值并完成了 `OPEN_DATA_PATH` 握手。**

**结论：问题不在"NuttX 驱动写得不对"—— 一个独立实现的官方主机栈在这条链路上表现更差。**
