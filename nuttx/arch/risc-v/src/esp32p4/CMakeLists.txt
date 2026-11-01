# ##############################################################################
# arch/risc-v/src/esp32p4/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# Licensed to the Apache Software Foundation (ASF) under one or more contributor
# license agreements.  See the NOTICE file distributed with this work for
# additional information regarding copyright ownership.  The ASF licenses this
# file to you under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License.  You may obtain a copy of
# the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
# License for the specific language governing permissions and limitations under
# the License.
#
# ##############################################################################

# Include common espressif CMake configuration
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common/espressif espressif)

set(SRCS esp_chip_rev.c esp32p4_atomic.c)

target_compile_definitions(arch PRIVATE _RETARGETABLE_LOCKING)
target_compile_options(arch PRIVATE -Wno-array-bounds -Wno-stringop-overflow)

# ── P4-only SDIO / ESP-Hosted cluster ────────────────────────────────────────
# Moved out of the shared espressif layer (plan 2026-11-01): the shared files are
# compiled for every ESP32 family, these are selected only by ESP32-P4 symbols.

if(CONFIG_ESPRESSIF_SDMMC)
  target_sources(arch PRIVATE esp_hosted/sdmmc/esp_sdmmc.c)
endif()

if(CONFIG_ESPRESSIF_HOSTED_SDIO)
  target_sources(arch PRIVATE esp_hosted/transport/esp_hosted_sdio.c)
endif()

# Headers of the cluster: the driver, the transport, the vendored frame layer and
# the port's compatibility shims.
# FreeRTOS compatibility headers: the vendored ESP-IDF HAL sources compiled for
# P4 (MIPI-CSI capture path) include <platform/os.h> shims; the port's copies of
# them live with the cluster.
target_include_directories(arch PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/esp_hosted/sdmmc
  ${CMAKE_CURRENT_LIST_DIR}/esp_hosted/transport
  ${CMAKE_CURRENT_LIST_DIR}/esp_hosted/compat/platform_include
  ${CMAKE_CURRENT_LIST_DIR}/esp_hosted/compat/freertos_compat)

# ESP-Hosted frame layer (vendored byte-for-byte from espressif/esp-hosted-mcu;
# see esp_hosted/frame/README.md). Only built for the hosted-SDIO configuration.
if(CONFIG_ESPRESSIF_HOSTED_SDIO)
  set(_eh_frame_src ${CMAKE_CURRENT_LIST_DIR}/esp_hosted/frame/src/eh_frame.c)
  target_sources(arch PRIVATE ${_eh_frame_src})

  # eh_frame.c picks its endian helpers with `#ifdef __KERNEL__` first, and the
  # NuttX build defines __KERNEL__ -- so it would try to pull <asm/byteorder.h>
  # (a Linux-only header) instead of taking the plain little-endian branch.
  # Undefining it, for this one source, selects the correct branch: NuttX on
  # RISC-V here is little-endian, which is exactly what that branch implements.
  set_source_files_properties(${_eh_frame_src} PROPERTIES COMPILE_OPTIONS "-U__KERNEL__")
  target_include_directories(arch PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/esp_hosted/frame/include
    ${CMAKE_CURRENT_LIST_DIR}/esp_hosted/frame/shim)
endif()

# ── P4-only drivers that belong to this port ────────────────────────────────
# Moved out of the shared espressif layer (plan 2026-11-01): they exist only in
# this project (upstream openvela has no such files), so they are scoped here.

if(CONFIG_ESP_RMT AND CONFIG_DRIVERS_RC)
  target_sources(arch PRIVATE drivers/esp_lirc.c)
endif()

if(CONFIG_SYSTEM_NXDIAG_ESPRESSIF_CHIP_WO_TOOL)
  target_sources(arch PRIVATE drivers/esp_nxdiag.c)
endif()

if(CONFIG_ESPRESSIF_ADC)
  target_sources(arch PRIVATE drivers/esp_adc.c)
endif()

if(CONFIG_ESPRESSIF_TOUCH)
  target_sources(arch PRIVATE drivers/esp_touch.c)
endif()

if(CONFIG_ESPRESSIF_MIPI_CSI)
  target_sources(arch PRIVATE drivers/esp_csi.c)
endif()

if(CONFIG_ESPRESSIF_DEDICATED_GPIO)
  target_sources(arch PRIVATE drivers/esp_dedic_gpio.c)
endif()

if(CONFIG_ESPRESSIF_MIPI_DSI)
  target_sources(arch PRIVATE drivers/esp_mipi_dsi.c)
endif()

if(CONFIG_ESPRESSIF_EMAC)
  target_sources(arch PRIVATE drivers/esp_emac.c)
endif()

target_include_directories(arch PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/drivers)

# NuttX port layer of the official esp-hosted host stack (appendix AS).
# Selected only by CONFIG_ESPRESSIF_HOSTED_OFFICIAL; the port's own sources and
# include paths live in its CMakeLists.
if(CONFIG_ESPRESSIF_HOSTED_OFFICIAL)
  add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/esp_hosted/official/port/os/nuttx)
endif()

if(SRCS)
  target_sources(arch PRIVATE ${SRCS})
endif()
