# 最小 SDIO 从机测试固件（C6）

**目的**：回答一个一直没能回答的问题 —— **这颗 C6 的 SDIO 从机到底能不能把数据发出去？**

不含 esp-hosted、不含 WiFi、不含任何协议。只做：

```c
sdio_slave_initialize(&cfg);        /* 同样的引脚：CLK=19 CMD=18 D0..D3=20..23 */
sdio_slave_start();
for (;;)
  sdio_slave_transmit(pattern, 64); /* 反复发 64 字节已知数据 0xC6^i */
```

时序用 `SDIO_SLAVE_TIMING_PSEND_PSAMPLE`（与 esp-hosted 的 CP 相同）。

## 怎么用

**1. 让 C6 进入下载模式**：按住板上 **BOOT（GPIO9）** → 按一下 **RESET** → 松开 **BOOT**

**2. 烧录**

```sh
tools/test/c6_sdio_minimal/flash.sh
```

（`prebuilt/` 里已放了编译好的三个镜像；`main/` 是源码，可重新 `idf.py build`）

**3. 在 P4 侧读**

P4 上的驱动会在启动时读一次队列里的包并把内容打出来。C6 每发完一份就接着发下一份，
所以每次 P4 复位都有一次读取机会。

## 结果怎么解读

| 现象 | 结论 |
|---|---|
| **P4 读到 `c6 c7 c4 c5 ...`（`0xC6 ^ i` 的图案）** | ✅ **C6 的 SDIO 从机硬件正常** —— 问题在 esp-hosted 的 CP 配置或协议层 |
| **P4 仍然超时（`-110`）、数据线仍无活动** | 🔴 **C6 的 SDIO 从机硬件或板级连接有问题** —— 与 esp-hosted 无关 |

**这是本次排查里唯一一个能明确定性的实验。**
