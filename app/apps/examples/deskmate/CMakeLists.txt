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
    fonts/dm_font_20.c
    fonts/dm_font_32.c
    src/dm_selftest.c
    src/dm_log.c
    src/agent/dm_agent.c
    src/hal/dm_camera.c
    src/hal/dm_display.c
    src/hal/dm_net.c
    src/hal/dm_touch.c
    src/ui/dm_page_camera.c
    src/ui/dm_page_chat.c
    src/ui/dm_page_home.c
    src/ui/dm_page_net.c
    src/ui/dm_page_settings.c
    src/ui/dm_page_agent.c
    src/ui/dm_ui.c
    src/ui/dm_widgets.c)

endif()
