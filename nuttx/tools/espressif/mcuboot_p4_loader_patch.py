#!/usr/bin/env python3
############################################################################
# tools/espressif/mcuboot_p4_loader_patch.py
#
# Patch MCUboot's ESP32-P4 loader so that applications executing in place
# from flash (IROM/DROM) keep working.
#
# Background: `esp_app_image_load()` only memcpy()s the IRAM/DRAM regions out
# of flash and then jumps to the entry point.  When the application was
# linked for flash execution (openvela/NuttX with MCUboot, i.e. text and
# rodata live at 0x40000000), the flash pages holding that code are never
# mapped into the CPU's address space, so the first call into flash faults.
#
# The load header already carries the information required to fix this
# (irom_map_addr / irom_flash_offset / irom_size); this patch consumes it and
# programs the MMU + invalidates the cache before transferring control.
#
# Idempotent: running it twice leaves the file unchanged.
#
# Usage: mcuboot_p4_loader_patch.py <mcuboot-dir>
############################################################################

import os
import sys

MARKER = "openvela: map flash-mapped (XIP) regions"

INCLUDES = '''#include "hal/mmu_hal.h"
#include "hal/cache_hal.h"
#include "hal/cache_ll.h"
#include "soc/soc_caps.h"
'''

MAP_CODE = '''    /* %s */
    if (load_header.irom_size > 0) {
        uint32_t mapped_len = 0;
        uint32_t irom_paddr = fap->fa_off + load_header.irom_flash_offset;

        mmu_hal_map_region(0, MMU_TARGET_FLASH0, load_header.irom_map_addr,
                           irom_paddr, load_header.irom_size, &mapped_len);
        cache_hal_invalidate_addr(load_header.irom_map_addr, mapped_len);

        /* Enable the L1 cache bus for the freshly mapped window, otherwise
         * the CPU cannot fetch instructions from it. */
        cache_bus_mask_t bus_mask =
            cache_ll_l1_get_bus(0, load_header.irom_map_addr, mapped_len);
        cache_ll_l1_enable_bus(0, bus_mask);
        cache_ll_l1_enable_bus(1, bus_mask);

        BOOT_LOG_INF("Mapped IROM: vaddr=0x%%x paddr=0x%%x len=0x%%x",
                     load_header.irom_map_addr, irom_paddr, mapped_len);
    }

    BOOT_LOG_INF("start=0x%%x", load_header.entry_addr);''' % MARKER

ANCHOR = '''    BOOT_LOG_INF("start=0x%x", load_header.entry_addr);'''


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)

    root = sys.argv[1]
    path = os.path.join(root, "boot", "espressif", "port", "esp_loader.c")
    with open(path) as f:
        src = f.read()

    if MARKER in src:
        print("loader patch: already applied")
        return

    if ANCHOR not in src:
        raise SystemExit(f"loader patch: anchor not found in {path}")

    # 1. Add the required HAL headers.
    inc_anchor = '#include "esp_mcuboot_image.h"'
    if inc_anchor in src and "hal/mmu_hal.h" not in src:
        src = src.replace(inc_anchor, inc_anchor + "\n" + INCLUDES.rstrip(), 1)
    elif "hal/mmu_hal.h" not in src:
        # Fall back to prepending after the first block of includes.
        lines = src.split("\n")
        for i, line in enumerate(lines):
            if line.startswith('#'):
                continue
            lines.insert(i, INCLUDES.rstrip())
            break
        src = "\n".join(lines)

    # 2. Map the XIP region right before control is handed over.
    src = src.replace(ANCHOR, MAP_CODE, 1)

    # 3. A zero-length region is valid in the load header (ESP32-P4 aliases
    #    IRAM and DRAM, so only one copy is needed) but bootloader_mmap()
    #    rejects a zero length.  Skip such regions instead of logging an
    #    error.
    old_seg = "    const uint32_t *data = (const uint32_t *)bootloader_mmap((fap->fa_off + data_addr), data_len);"
    new_seg = ("    if (data_len == 0) {\n"
               "        return 0;\n"
               "    }\n\n" + old_seg)
    if old_seg in src:
        src = src.replace(old_seg, new_seg, 1)

    with open(path, "w") as f:
        f.write(src)
    print("loader patch: applied (MMU mapping of IROM regions)")


if __name__ == "__main__":
    main()
