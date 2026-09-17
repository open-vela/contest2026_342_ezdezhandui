/****************************************************************************
 * arch/risc-v/src/esp32p4/esp32p4_atomic.c
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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <nuttx/irq.h>

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* RV32IMC has no 64-bit atomic instructions.  The toolchain (riscv-none-elf)
 * ships no libatomic, so provide the 64-bit atomic fetch-or used by
 * esp_gpio_reserve().  Single-core protection via critical section is
 * sufficient; upgrade to spinlock if SMP is enabled.
 *
 * The signatures must match GCC's built-in declarations exactly
 * (volatile void *), otherwise -Werror=builtin-declaration-mismatch.
 */

uint64_t __atomic_fetch_or_8(volatile void *ptr, uint64_t val, int memorder)
{
  irqstate_t flags = up_irq_save();
  uint64_t old = *(volatile uint64_t *)ptr;

  *(volatile uint64_t *)ptr = old | val;
  up_irq_restore(flags);

  return old;
}

uint64_t __atomic_load_8(const volatile void *ptr, int memorder)
{
  irqstate_t flags = up_irq_save();
  uint64_t old = *(volatile const uint64_t *)ptr;

  up_irq_restore(flags);

  return old;
}

uint64_t __atomic_fetch_and_8(volatile void *ptr, uint64_t val, int memorder)
{
  irqstate_t flags = up_irq_save();
  uint64_t old = *(volatile uint64_t *)ptr;

  *(volatile uint64_t *)ptr = old & val;
  up_irq_restore(flags);

  return old;
}
