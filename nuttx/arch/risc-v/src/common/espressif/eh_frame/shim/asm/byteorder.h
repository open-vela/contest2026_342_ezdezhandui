/* Shim for <asm/byteorder.h>, the Linux-kernel header eh_frame.c reaches for
 * when __KERNEL__ is defined -- and the NuttX build does define it.
 *
 * Rather than fight the macro (a source-level -U__KERNEL__ did not survive into
 * the generated compile line), satisfy the include. The two helpers the frame
 * layer uses are byte-order conversions to/from little-endian, and this target
 * (RISC-V, ESP32-P4) is little-endian, so they are the identity -- exactly what
 * the upstream little-endian branch would have produced.
 */
#pragma once
#include <stdint.h>

#define cpu_to_le16(v) ((uint16_t)(v))
#define le16_to_cpu(v) ((uint16_t)(v))
#define cpu_to_le32(v) ((uint32_t)(v))
#define le32_to_cpu(v) ((uint32_t)(v))
