#!/usr/bin/env bash
# =============================================================================
# ensure_psram_guard.sh — 保证 MCUboot 的 PSRAM 时钟守卫存在（幂等）
#
# 背景（附录 AF / AI）
#   HP_SYS_CLKRST_PERI_CLK_CTRL00(0x500E6030) bit15 PSRAM_CORE_CLK_EN 复位默认 1，
#   但在本板曾被清 0。该域不重新初始化，所以状态会跨复位保持；门控关闭时整块
#   MSPI/PSRAM 寄存器窗口不响应，引导器在 mmu_hal_unmap_all() 里写
#   SPI_MEM_S_MMU_ITEM_INDEX_REG 直接 Store/AMO access fault —— 板子"像坏了"。
#
#   守卫 = 在 mmu_hal_unmap_all() 开头把该位写回 1（已置位时无副作用）。
#
# 为什么是脚本而不是 patch
#   nuttx/arch/.../bootloader/esp-hal-3rdparty-mcuboot 这棵子树**未被任何 git 仓库
#   跟踪**，没有可依赖的 pristine 副本，git apply 无从下手。脚本直接在源码树上做
#   幂等插入，评审环境 repo sync 后跑一次即可。
#
# 用法: tools/build_flash/ensure_psram_guard.sh [工作区根]
#       默认从脚本位置上溯三级推导；缺失/已存在都返回 0，只在真正失败时非 0。
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="${1:-$(cd "$SCRIPT_DIR/../../.." && pwd)}"
F="$ROOT/nuttx/bootloader-esp32p4/esp-hal-3rdparty-mcuboot/components/hal/mmu_hal.c"
LINE='    *(volatile uint32_t *)0x500e6030u |= (1u << 15);   /* PSRAM_CORE_CLK_EN */'

[ -f "$F" ] || { echo "skip: $F 不存在（非 repo 工作区？）"; exit 0; }
grep -q '0x500e6030u' "$F" && { echo "ok: PSRAM 守卫已在"; exit 0; }

python3 - "$F" "$LINE" <<'PY' || { echo "fail: 插入失败"; exit 1; }
import sys
path, line = sys.argv[1], sys.argv[2]
src = open(path).read()
anchor = "void mmu_hal_unmap_all(void)\n{\n"
if anchor not in src:
    sys.exit("anchor not found")
note = ("    /* Keep the PSRAM/MSPI core clock gate open before any MMU write.\n"
        "     * bit15 resets to 1 but has been seen cleared after a WDT reset; with the\n"
        "     * gate shut the MSPI window stops answering and the MMU write below faults\n"
        "     * (Store/AMO access fault at 0x5008E380) -- the board looks dead until a\n"
        "     * debugger pokes the register.  Guard lives here because several callers\n"
        "     * unmap the MMU this early.  See docs/02_驱动实现/WiFi_SDIO实现.md 附录 AF/AI. */\n")
open(path, "w").write(src.replace(anchor, anchor + note + line + "\n\n", 1))
print("patched:", path)
PY
