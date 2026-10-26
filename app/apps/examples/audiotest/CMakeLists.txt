# ##############################################################################
# apps/examples/audiotest/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# ##############################################################################

if(CONFIG_EXAMPLES_AUDIOTEST)
  nuttx_add_application(
    NAME
    ${CONFIG_EXAMPLES_AUDIOTEST_PROGNAME}
    PRIORITY
    ${CONFIG_EXAMPLES_AUDIOTEST_PRIORITY}
    STACKSIZE
    ${CONFIG_EXAMPLES_AUDIOTEST_STACKSIZE}
    MODULE
    ${CONFIG_EXAMPLES_AUDIOTEST}
    SRCS
    audiotest_main.c)
endif()
