#!/bin/bash
# Flash the minimal SDIO slave test to the C6.
# Requires the C6 in download mode: hold BOOT (GPIO9), press RESET, release BOOT.
set -e
source /home/ez/esp/esp-idf/export.sh >/dev/null 2>&1
cd /tmp/c6sdiotest
python3 -m esptool --chip esp32c6 -p /dev/ttyUSB1 -b 460800 \
  --before no_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 4MB \
  0x0     build/bootloader/bootloader.bin \
  0x8000  build/partition_table/partition-table.bin \
  0x10000 build/c6sdiotest.bin
