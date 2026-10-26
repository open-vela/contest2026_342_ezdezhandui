# eh_frame — ESP-Hosted 的帧层（上游原样投放）

这些文件**逐字节来自 `espressif/esp-hosted-mcu`，没有改动**：

| 本目录 | 上游路径 |
|---|---|
| `src/eh_frame.c` | `common/eh_frame/src/eh_frame.c` |
| `include/eh_frame.h` | `common/eh_frame/include/eh_frame.h` |
| `include/eh_common_header.h` | `common/eh_common/include/eh_common_header.h` |
| `include/eh_common_header_v2.h` | `common/eh_common/include/eh_common_header_v2.h` |
| `include/eh_common_interface.h` | `common/eh_common/include/eh_common_interface.h` |
| `include/eh_common_sdio_cfg.h` | `common/eh_common/include/eh_common_sdio_cfg.h` |
| `include/eh_tlv_tags.h` | `common/eh_tlv/include/eh_tlv_tags.h` |

## 为什么是"投放"而不是"改写"

帧层是纯逻辑：它只依赖 `esp_err_t`（本树里
`esp-hal-3rdparty/components/esp_common/include/esp_err.h` 已经提供，
因此在 NuttX 侧**连 shim 都不需要**）。既然一行都不用改，就没有理由改 ——
保持与上游逐字节一致，将来升级直接覆盖即可。

## 耦合方式

帧层不知道任何总线细节。它与 SDIO 传输层的耦合只有两个方向：

- 收：从 DMA 缓冲区里 `eh_frame_decode()`
- 发：把负载先写进缓冲区，再 `eh_frame_encode()`

> ⚠️ 顺序不能反：`eh_frame_encode()` 对 `sizeof(hdr) + payload_len` 计算校验和，
> 所以负载必须**已经在缓冲区里**再调用它，否则每一帧都会在对端校验失败。
> 另外校验和是在"校验和字段自身为 0"的前提下算的。
> 这两条都是 `tools/test/eh_frame/` 的往返测试实际抓出来的，见 docs/08 §E.8。

## 唯一的本地补充：`shim/linux/types.h`

上游 `eh_common_interface.h` 包含 `<linux/types.h>`，而 NuttX 的交叉编译环境
没有这个头。为了**不改上游文件**，这里提供一个最小 shim（只 `#include`
`<stdint.h>` / `<stddef.h>`）。它放在 `shim/` 下并**排在组件自身 include 之后**，
NuttX 也没有别处会找 `linux/types.h`，所以影响范围限于本组件。

### 另一个 shim：`shim/asm/byteorder.h`

`eh_frame.c` 的字节序辅助函数第一分支是
`#ifdef __KERNEL__ -> #include <asm/byteorder.h>`（Linux 内核专用头），
而 **NuttX 构建会定义 `__KERNEL__`**，于是走进了这个分支。

尝试过对源文件单独加 `-U__KERNEL__`，但该选项没有出现在生成的编译命令行里。
改成**满足这个 include**：本目标是 RISC-V（小端），所以
`cpu_to_le16`/`le16_to_cpu` 就是恒等变换 —— 与上游小端分支的语义完全一致。
比跟宏较劲更稳，也不用动源码。

## 独立验证

帧层的正确性不需要板子：

```sh
tools/test/eh_frame/run.sh /tmp/esp-hosted-mcu
```
