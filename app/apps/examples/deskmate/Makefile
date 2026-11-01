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
CSRCS   = src/dm_selftest.c
CSRCS  += src/hal/dm_display.c
CSRCS  += src/hal/dm_touch.c
CSRCS  += src/ui/dm_ui.c

include $(APPDIR)/Application.mk
