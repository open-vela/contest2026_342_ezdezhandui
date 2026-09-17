#!/usr/bin/env python3
############################################################################
# tools/espressif/mcuboot_mkloadhdr.py
#
# Build the payload that MCUboot's ESP32-P4 port expects inside the
# application slot.
#
# The MCUboot espressif port does not execute the ESP image format directly:
# `esp_app_image_load()` reads an `esp_image_load_header_t` (magic
# 0xace637d3) which tells it which SRAM regions to memcpy out of flash, where
# the entry point lives, and (after the openvela loader patch) which flash
# region to map for execute-in-place.
#
# Slot layout produced (offsets relative to the slot start):
#     +0x0000  MCUboot image header        (added by `imgtool sign -H 32`)
#     +0x0020  load header (0x60 bytes)    <- hdr_offset
#     +0x0080  SRAM (IRAM/DRAM) region     <- copied to RAM by MCUboot
#     +0x10000 flash-mapped region         <- 64KB aligned for the MMU, XIP
#
# Usage:
#   mcuboot_mkloadhdr.py <esp-image.bin> <out.bin>
############################################################################

import struct
import sys

LOAD_HEADER_MAGIC = 0xACE637D3
MCUBOOT_HEADER_SIZE = 0x20
LOAD_HEADER_SIZE = 0x60  # 24 u32 words
MMU_PAGE = 0x10000  # flash MMU page size / alignment requirement

# The linker script (esp32p4_sections*.ld) hardcodes this slot offset for the
# XIP region when CONFIG_ESPRESSIF_BOOTLOADER_MCUBOOT is enabled, because it
# has to report an LMA that NuttX can turn into the correct flash paddr.
# Fail loudly here rather than producing an image the linker disagrees with.
EXPECTED_XIP_SLOT_OFFSET = 0x10000

# ESP32-P4: IRAM and DRAM share the 0x4FF00000..0x4FFC0000 window.
SOC_IRAM_LOW, SOC_IRAM_HIGH = 0x4FF00000, 0x4FFC0000
SOC_DRAM_LOW = 0x4FF00000
SOC_IROM_LOW, SOC_IROM_HIGH = 0x40000000, 0x44000000

# TCM (ITCM/DTCM), i.e. SOC_TCM_LOW..SOC_TCM_HIGH on this SoC.  Functions
# marked TCM_IRAM_ATTR / TCM_DRAM_ATTR -- rtc_clk_mpll_enable()/disable(), the
# pmu_sleep helpers -- are linked into .tcm.text/.tcm.data at 0x301000xx and
# the application calls straight into them during early clock bring-up.
#
# This window is disjoint from both the IRAM and the IROM window above, so a
# loader that only knows those two silently drops the segment.  The result is
# not a build error but a board that never boots: the app jumps into TCM
# holding power-on garbage and takes an illegal-instruction exception at
# 0x30100064 that returns to itself forever (mcause=0x30000002,
# mepc=0x30100064, seen with a debugger while the panel/console stayed dead).
SOC_TCM_LOW, SOC_TCM_HIGH = 0x30100000, 0x30102000


def parse_esp_image(path):
    """Return (entry_addr, [(load_addr, data_offset, data_len), ...], blob)."""
    with open(path, "rb") as f:
        blob = f.read()

    if len(blob) < 0x18 or blob[0] != 0xE9:
        raise SystemExit(f"{path}: not an ESP image (bad magic)")

    seg_count = blob[1]
    entry = struct.unpack_from("<I", blob, 4)[0]

    segments = []
    off = 0x18
    for _ in range(seg_count):
        load_addr, data_len = struct.unpack_from("<II", blob, off)
        off += 8
        if load_addr != 0:  # padding segments carry load_addr 0
            segments.append((load_addr, off, data_len))
        off += data_len
    return entry, segments, blob


def region_of(segments, low, high, what):
    """Collect segments living inside [low, high)."""
    sel = [s for s in segments if low <= s[0] < high]
    for load_addr, _, data_len in sel:
        if load_addr + data_len > high:
            raise SystemExit(
                f"{what}: segment 0x{load_addr:08x}+0x{data_len:x} exceeds window")
    return sel


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)

    esp_path, out_path = sys.argv[1], sys.argv[2]
    entry, segments, blob = parse_esp_image(esp_path)

    sram_segs = region_of(segments, SOC_IRAM_LOW, SOC_IRAM_HIGH, "SRAM")
    xip_segs = region_of(segments, SOC_IROM_LOW, SOC_IROM_HIGH, "XIP")
    tcm_segs = region_of(segments, SOC_TCM_LOW, SOC_TCM_HIGH, "TCM")

    if not sram_segs:
        raise SystemExit("no SRAM segment found; nothing for MCUboot to load")

    # ---- SRAM region: one contiguous image so a single memcpy covers it ----
    sram_start = min(s[0] for s in sram_segs)
    sram_end = max(s[0] + s[2] for s in sram_segs)
    sram_size = sram_end - sram_start
    sram_buf = bytearray(sram_size)
    for load_addr, data_off, data_len in sram_segs:
        rel = load_addr - sram_start
        sram_buf[rel:rel + data_len] = blob[data_off:data_off + data_len]

    # ---- TCM region: contiguous image, loaded by MCUboot into TCM ----------
    tcm_start = tcm_size = 0
    tcm_buf = bytearray()
    if tcm_segs:
        tcm_start = min(s[0] for s in tcm_segs)
        tcm_end = max(s[0] + s[2] for s in tcm_segs)
        tcm_size = tcm_end - tcm_start
        tcm_buf = bytearray(tcm_size)
        for load_addr, data_off, data_len in tcm_segs:
            rel = load_addr - tcm_start
            tcm_buf[rel:rel + data_len] = blob[data_off:data_off + data_len]

    # ---- XIP region: keep runtime layout so vaddr -> paddr stays linear ----
    xip_base = xip_size = 0
    xip_buf = b""
    if xip_segs:
        xip_base = min(s[0] for s in xip_segs)
        xip_end = max(s[0] + s[2] for s in xip_segs)
        xip_size = xip_end - xip_base
        xip_buf = bytearray(xip_size)
        for load_addr, data_off, data_len in xip_segs:
            rel = load_addr - xip_base
            xip_buf[rel:rel + data_len] = blob[data_off:data_off + data_len]

    # ---- Assemble payload ---------------------------------------------------
    payload = bytearray()
    payload += b"\x00" * LOAD_HEADER_SIZE
    payload += sram_buf

    # TCM goes right after SRAM so the existing XIP padding below still pushes
    # the flash-mapped region to EXPECTED_XIP_SLOT_OFFSET.
    tcm_slot_off = 0
    if tcm_buf:
        tcm_slot_off = MCUBOOT_HEADER_SIZE + len(payload)
        payload += tcm_buf

    xip_slot_off = 0
    if xip_segs:
        # The flash MMU requires (paddr % 64KB) == (vaddr % 64KB); the XIP
        # region starts at a 64KB-aligned virtual address, so its slot offset
        # has to be a multiple of 64KB as well.
        block = 1
        while block * MMU_PAGE - MCUBOOT_HEADER_SIZE < len(payload):
            block += 1
        want = block * MMU_PAGE - MCUBOOT_HEADER_SIZE
        payload += b"\x00" * (want - len(payload))
        xip_slot_off = MCUBOOT_HEADER_SIZE + want
        if xip_slot_off != EXPECTED_XIP_SLOT_OFFSET:
            raise SystemExit(
                f"XIP region would land at slot offset 0x{xip_slot_off:x} but the "
                f"linker script expects 0x{EXPECTED_XIP_SLOT_OFFSET:x}; the SRAM "
                f"region grew past the reserved area - update "
                f"MCUBOOT_XIP_SLOT_OFFSET in esp32p4_sections*.ld accordingly")
        payload += xip_buf

    iram_slot_off = MCUBOOT_HEADER_SIZE + LOAD_HEADER_SIZE

    # ---- Load header --------------------------------------------------------
    h = [0] * 24
    h[0] = LOAD_HEADER_MAGIC
    h[1] = entry
    # IRAM (DRAM aliases IRAM on ESP32-P4, so a single copy covers both)
    h[2] = sram_start
    h[3] = iram_slot_off
    h[4] = sram_size
    h[5] = SOC_DRAM_LOW
    h[6] = 0
    h[7] = 0
    h[8] = tcm_start
    h[9] = tcm_slot_off
    h[10] = tcm_size
    h[11] = 0
    h[12] = 0
    h[13] = 0
    if xip_segs:
        h[14] = xip_base
        h[15] = xip_slot_off
        h[16] = xip_size
        h[17] = xip_base
        h[18] = xip_slot_off
        h[19] = xip_size
    struct.pack_into("<24I", payload, 0, *h)

    with open(out_path, "wb") as f:
        f.write(payload)

    print(f"load header: entry=0x{entry:08x}")
    print(f"  SRAM  vaddr=0x{sram_start:08x} size=0x{sram_size:x} -> slot_off=0x{iram_slot_off:x}")
    if tcm_buf:
        print(f"  TCM   vaddr=0x{tcm_start:08x} size=0x{tcm_size:x} -> slot_off=0x{tcm_slot_off:x}")
    if xip_segs:
        print(f"  XIP   vaddr=0x{xip_base:08x} size=0x{xip_size:x} -> slot_off=0x{xip_slot_off:x}")
    print(f"wrote {out_path}: {len(payload)} bytes")


if __name__ == "__main__":
    main()
