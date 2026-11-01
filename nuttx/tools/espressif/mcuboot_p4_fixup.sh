#!/usr/bin/env bash
############################################################################
# tools/espressif/mcuboot_p4_fixup.sh
#
# Make MCUboot's ESP32-P4 port build with the ESP32-P4 toolchain used by
# openvela/NuttX (riscv-none-elf GCC 13.x) and with the esp-hal-3rdparty
# revision that ships with this tree.
#
# Runs as the ExternalProject PATCH_COMMAND, so it must be idempotent:
# the MCUboot checkout is reset on every update step.
#
# 1. esp32p4 HAL: esp-hal in this tree reports ESP-IDF 6.1.0, while MCUboot
#    expects 6.0.0 -> relax the check.
# 2. Toolchain compatibility:
#      -specs=picolibc.specs  -> provided by the NuttX MCUboot toolchain file
#                                (--specs=nosys.specs), so drop it here
#      -std=gnu23             -> -std=gnu2x for GCC 13
# 3. mbedtls submodule: newer MCUboot renamed ext/mbedtls ->
#    ext/mbedtls-3.6.0.
############################################################################
set -e

MCUBOOT_DIR="${1:?usage: mcuboot_p4_fixup.sh <mcuboot-dir>}"

ESP_CMAKE="${MCUBOOT_DIR}/boot/espressif/CMakeLists.txt"
HAL_CMAKE="${MCUBOOT_DIR}/boot/espressif/hal/CMakeLists.txt"

[ -f "${ESP_CMAKE}" ] || {
  echo "mcuboot_p4_fixup.sh: ${ESP_CMAKE} not found" >&2
  exit 1
}

# 1. Relax the HAL version gate (6.0.0 -> any 6.x available in this tree)
sed -i -E 's/set\(EXPECTED_IDF_HAL_VERSION "[0-9]+\.[0-9]+\.[0-9]+"\)/set(EXPECTED_IDF_HAL_VERSION "6.1.0")/' \
  "${ESP_CMAKE}"

# 2. Toolchain compatibility (both the bootloader and its HAL library)
for f in "${ESP_CMAKE}" "${HAL_CMAKE}"; do
  [ -f "$f" ] || continue
  sed -i 's/-specs=picolibc.specs//g' "$f"
  sed -i 's/-std=gnu23/-std=gnu2x/g' "$f"
done

# 4. esptool 4.12 (shipped with this tree) spells these options with
#    underscores; MCUboot master uses the esptool v5 hyphenated names.
sed -i 's/--flash-mode \${ESP_FLASH_MODE} --flash-freq \${ESP_FLASH_FREQ} --flash-size \${CONFIG_ESP_FLASH_SIZE}/--flash_mode ${ESP_FLASH_MODE} --flash_freq ${ESP_FLASH_FREQ} --flash_size ${CONFIG_ESP_FLASH_SIZE}/' \
  "${ESP_CMAKE}"

# 5. Map the flash-mapped (XIP) region before jumping to the application:
#    MCUboot only memcpy()s IRAM/DRAM, so an app linked for flash execution
#    (openvela/NuttX default) would fault on its first flash access.
python3 "$(dirname "$0")/mcuboot_p4_loader_patch.py" "${MCUBOOT_DIR}"

echo "mcuboot_p4_fixup.sh: applied ESP32-P4 compatibility fixes"
