# NuttX port of the official esp-hosted host stack.
#
# Only built when CONFIG_ESPRESSIF_HOSTED_OFFICIAL is selected, so the default
# product is bit-for-bit what it was before this directory existed (acceptance
# criterion of the porting plan, docs appendix AS.4).

set(EH_HOSTED_NUTTX_DIR ${CMAKE_CURRENT_LIST_DIR})

target_sources(arch PRIVATE
  ${EH_HOSTED_NUTTX_DIR}/src/eh_host_port_sdio.c
  ${EH_HOSTED_NUTTX_DIR}/src/port_stubs.c
)

# BEFORE: the stack includes <freertos/FreeRTOS.h> and friends, and this port's
# self-contained shims must win over the tree's freertos_compat layer (whose
# implementation lives in the untracked MCUboot subtree; see appendix AT).
target_include_directories(arch BEFORE PRIVATE
  ${EH_HOSTED_NUTTX_DIR}/include
  ${EH_HOSTED_NUTTX_DIR}/../include
  ${EH_HOSTED_NUTTX_DIR}/../../../common/eh_common/include
)

# Configuration of the official stack, in the symbols its own
# eh_host_port_master_config.h reads.  Route B does not port esp_netif/lwip, so
# only the SDIO bus is selected here; the values are the board wiring measured
# in appendices AK/AN (slot 1 through the GPIO matrix, 4-bit, 40 MHz).
target_compile_definitions(arch PRIVATE
  CONFIG_ESP_HOSTED_HOST_TRANSPORT_MCU_READY=1
  CONFIG_ESP_HOSTED_HOST_TRANSPORT_BUS_SDIO_READY=1
  CONFIG_ESP_HOSTED_HOST_TRANSPORT_BUS_SDIO=1
  CONFIG_ESP_HOSTED_HOST_SDIO_SLOT=1
  CONFIG_ESP_HOSTED_HOST_SDIO_BUS_WIDTH=4
  CONFIG_ESP_HOSTED_HOST_SDIO_CLK_KHZ=40000
  CONFIG_ESP_HOSTED_HOST_SDIO_PIN_CLK=18
  CONFIG_ESP_HOSTED_HOST_SDIO_PIN_CMD=19
  CONFIG_ESP_HOSTED_HOST_SDIO_PIN_D0=14
  CONFIG_ESP_HOSTED_HOST_SDIO_PIN_D1=15
  CONFIG_ESP_HOSTED_HOST_SDIO_PIN_D2=16
  CONFIG_ESP_HOSTED_HOST_SDIO_PIN_D3=17
  CONFIG_ESP_HOSTED_HOST_SDIO_EN_GPIO=54
  EH_HOST_HOST_USES_STATIC_NETIF=0
  CONFIG_ESP_HOSTED_CP_TARGET_ESP32C6=1
  CONFIG_ESP_HOSTED_USE_MEMPOOL=1
)
