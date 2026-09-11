/****************************************************************************
 * arch/risc-v/src/common/espressif/freertos_compat/freertos/idf_additions.h
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

#ifndef __ARCH_RISCV_SRC_COMMON_ESPRESSIF_FREERTOS_COMPAT_FREERTOS_IDF_ADDITIONS_H
#define __ARCH_RISCV_SRC_COMMON_ESPRESSIF_FREERTOS_COMPAT_FREERTOS_IDF_ADDITIONS_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stddef.h>
#include <stdint.h>

#include "platform/os.h"
#include "freertos/queue.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* ESP-IDF "…WithCaps" allocation variants.  NuttX has no capability based
 * heap regions, so the caps argument is accepted and ignored by the shim
 * (see esp-hal-3rdparty/nuttx/src/heap_caps.c for the same reasoning).
 */

#define xQueueCreateWithCaps(items, item_size, caps) \
  esp_os_queue_create_with_caps((items), (item_size), (caps))

#define vQueueDeleteWithCaps(queue) esp_os_queue_delete_with_caps(queue)

/* Static variants simply alias the dynamic ones here: the NuttX shim always
 * allocates the underlying message queue itself.
 */

#define xQueueCreateStaticWithCaps(items, item_size, storage, caps) \
  esp_os_queue_create_with_caps((items), (item_size), (caps))

#define vQueueUnregisterQueue(queue) \
  esp_os_queue_delete_with_caps(queue)

#endif /* __ARCH_RISCV_SRC_COMMON_ESPRESSIF_FREERTOS_COMPAT_FREERTOS_IDF_ADDITIONS_H */
