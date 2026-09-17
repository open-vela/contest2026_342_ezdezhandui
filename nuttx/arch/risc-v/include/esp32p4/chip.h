/****************************************************************************
 * arch/risc-v/include/esp32p4/chip.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#ifndef __ARCH_RISCV_INCLUDE_ESP32P4_CHIP_H
#define __ARCH_RISCV_INCLUDE_ESP32P4_CHIP_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* openvela SIMPLE_BOOT: the 512KB SRAM image cannot afford libgcc's 128-bit
 * soft-fp (__addtf3/__subtf3/__multf3/__divtf3, ~50KB pulled in by the
 * generic strtold implementation behind strtod/strtof).  Force long double
 * == double on this port; nothing here needs 128-bit floats. */

#undef CONFIG_HAVE_LONG_DOUBLE

#endif /* __ARCH_RISCV_INCLUDE_ESP32P4_CHIP_H */
