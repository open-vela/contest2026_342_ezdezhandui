# NuttX port of the official esp-hosted host stack.
#
# Only built when CONFIG_ESPRESSIF_HOSTED_OFFICIAL is selected, so the default
# product is bit-for-bit what it was before this directory existed (acceptance
# criterion of the porting plan, docs appendix AS.4).

set(EH_HOSTED_NUTTX_DIR ${CMAKE_CURRENT_LIST_DIR})

target_sources(arch PRIVATE
  ${EH_HOSTED_NUTTX_DIR}/src/eh_host_port_sdio.c
)

# BEFORE: the stack includes <freertos/FreeRTOS.h> and friends, and this port's
# self-contained shims must win over the tree's freertos_compat layer (whose
# implementation lives in the untracked MCUboot subtree; see appendix AT).
target_include_directories(arch BEFORE PRIVATE
  ${EH_HOSTED_NUTTX_DIR}/include
  ${EH_HOSTED_NUTTX_DIR}/../include
  ${EH_HOSTED_NUTTX_DIR}/../../../common/eh_common/include
)
