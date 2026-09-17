# ##############################################################################
# tools/espressif/mcuboot_toolchain_espressif.cmake
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

set(CMAKE_SYSTEM_NAME Generic)

set(CMAKE_C_COMPILER riscv-none-elf-gcc)
set(CMAKE_CXX_COMPILER riscv-none-elf-g++)
set(CMAKE_ASM_COMPILER riscv-none-elf-gcc)

# ESP32-P4 is RV32IMAFC; use the same ISA/ABI as the NuttX application
# (the generic rv32imc flags below conflict with the toolchain's default
# ilp32d ABI).  Other chips keep the original rv32imc base.
if(MCUBOOT_TARGET STREQUAL "esp32p4")
  set(_mcuboot_march "-march=rv32imac_zicsr_zifencei -mabi=ilp32")
else()
  set(_mcuboot_march "-march=rv32imc_zicsr_zifencei -mabi=ilp32")
endif()

set(CMAKE_C_FLAGS
    "${_mcuboot_march}"
    CACHE STRING "C Compiler Base Flags")
set(CMAKE_CXX_FLAGS
    "${_mcuboot_march}"
    CACHE STRING "C++ Compiler Base Flags")
set(CMAKE_EXE_LINKER_FLAGS
    "-nostartfiles ${_mcuboot_march} --specs=nosys.specs"
    CACHE STRING "Linker Base Flags")
