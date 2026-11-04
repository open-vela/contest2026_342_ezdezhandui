############################################################################
# apps/examples/deskmate/Makefile
#
# SPDX-License-Identifier: Apache-2.0
#
############################################################################

include $(APPDIR)/Make.defs

# DeskMate control-panel UI

PROGNAME  = $(CONFIG_EXAMPLES_DESKMATE_PROGNAME)
PRIORITY  = $(CONFIG_EXAMPLES_DESKMATE_PRIORITY)
STACKSIZE = $(CONFIG_EXAMPLES_DESKMATE_STACKSIZE)
MODULE    = $(CONFIG_EXAMPLES_DESKMATE)

CFLAGS += ${INCDIR_PREFIX}$(CURDIR)/src

MAINSRC = deskmate_main.c
CSRCS  += fonts/dm_font_14.c
CSRCS  += fonts/dm_font_32.c
CSRCS   = src/dm_selftest.c
CSRCS  += src/agent/dm_agent.c
CSRCS  += src/hal/dm_camera.c
CSRCS  += src/dm_log.c src/hal/dm_display.c
CSRCS  += src/hal/dm_net.c
CSRCS  += src/hal/dm_touch.c
CSRCS  += src/ui/dm_page_camera.c
CSRCS  += src/ui/dm_page_chat.c
CSRCS  += src/ui/dm_page_home.c
CSRCS  += src/ui/dm_page_net.c
CSRCS  += src/ui/dm_ui.c
CSRCS  += src/ui/dm_widgets.c

include $(APPDIR)/Application.mk

