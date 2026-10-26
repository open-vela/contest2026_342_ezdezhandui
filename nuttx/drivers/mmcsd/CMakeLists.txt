# ##############################################################################
# drivers/mmcsd/CMakeLists.txt
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

if(CONFIG_MMCSD)
  set(SRCS)

  # Include MMC/SD drivers

  if(CONFIG_MMCSD_SDIO)
    # sdio.c carries the SDIO *function* API (sdio_probe, sdio_io_rw_direct,
    # sdio_set_wide_bus, ...) that an SDIO lower half is driven through, while
    # mmcsd_sdio.c is the memory-card client on top of it.  The Makefile path
    # builds both (see Make.defs: "CSRCS += mmcsd_sdio.c sdio.c"); this list
    # only ever named the first, so any SDIO (rather than SD memory) user
    # linked against sdio_probe and failed.
    list(APPEND SRCS mmcsd_sdio.c sdio.c)
  endif()

  if(CONFIG_MMCSD_SPI)
    list(APPEND SRCS mmcsd_spi.c mmcsd_debug.c)
  endif()

  if(CONFIG_MMCSD_PROCFS)
    list(APPEND SRCS mmcsd_procfs.c)
  endif()

  target_sources(drivers PRIVATE ${SRCS})
endif()
