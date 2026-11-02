# ##############################################################################
# boards/risc-v/esp32p4/esp32p4-function-ev-board/CMakeLists.txt
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

add_subdirectory(src)

# ---------------------------------------------------------------------------
# 导出板级头文件目录
#
# NuttX 的架构层通过 `arch/board/board.h` 找板级头文件，而 `<build>/include/
# arch/board` 指向 `${NUTTX_DIR}/arch/${ARCH}/include/board`；CMake 流程并不会
# 创建这个链接（make 流程由 tools/configure.sh 创建）。缺了它，P4 端口的
# esp_idle.c / esp_i2c.c / esp_timerisr.c 等会因为找不到 arch/board/board.h 而
# 编译失败。这里在 configure 阶段补上（幂等：已存在就跳过）。
# ---------------------------------------------------------------------------
set(_board_inc "${CMAKE_CURRENT_LIST_DIR}/include")
set(_board_link "${NUTTX_DIR}/arch/risc-v/include/board")
if(EXISTS "${_board_inc}" AND NOT EXISTS "${_board_link}")
  file(CREATE_LINK "${_board_inc}" "${_board_link}" COPY_ON_ERROR SYMBOLIC)
  message(STATUS "board include exported: ${_board_link} -> ${_board_inc}")
endif()
