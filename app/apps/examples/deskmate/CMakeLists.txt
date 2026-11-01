# ##############################################################################
# apps/examples/deskmate/CMakeLists.txt
#
# SPDX-License-Identifier: Apache-2.0
#
# ##############################################################################

if(CONFIG_EXAMPLES_DESKMATE)

  nuttx_add_application(
    NAME
    ${CONFIG_EXAMPLES_DESKMATE_PROGNAME}
    PRIORITY
    ${CONFIG_EXAMPLES_DESKMATE_PRIORITY}
    STACKSIZE
    ${CONFIG_EXAMPLES_DESKMATE_STACKSIZE}
    MODULE
    ${CONFIG_EXAMPLES_DESKMATE}
    INCLUDE_DIRECTORIES
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    SRCS
    deskmate_main.c
    src/dm_selftest.c
    src/hal/dm_display.c
    src/hal/dm_touch.c
    src/ui/dm_ui.c)

endif()
