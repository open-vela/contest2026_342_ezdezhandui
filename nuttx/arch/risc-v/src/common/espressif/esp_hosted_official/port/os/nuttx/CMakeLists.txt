# NuttX port of the official esp-hosted host stack.
#
# Built as its own static library and linked into arch, rather than by adding
# its sources to arch directly.  That distinction matters: this port carries
# shim headers (esp_log.h, esp_err.h, freertos/*) that must be found *before*
# the tree's own copies for the stack's translation units, and must not be seen
# by the rest of arch -- putting the include directory on arch globally made
# every IDF component pick up the shims instead of its own headers, which broke
# unrelated files (measured: esp_efuse_fields.c, flash_encrypt.c).
#
# Only built when CONFIG_ESPRESSIF_HOSTED_OFFICIAL is selected, so the default
# product stays bit-for-bit identical (docs appendix AS.4).

set(EH_HOSTED_NUTTX_DIR ${CMAKE_CURRENT_LIST_DIR})
get_filename_component(EH_HOSTED_DIR ${EH_HOSTED_NUTTX_DIR}/../../.. ABSOLUTE)

# The vendored sources this port compiles.  Route B keeps esp_netif/lwip out,
# so eh_host_bus_sdio.c is the only bus driver selected.
file(GLOB EH_HOSTED_STACK_SRCS
  ${EH_HOSTED_DIR}/common/eh_common/src/*.c
  ${EH_HOSTED_DIR}/common/eh_frame/src/*.c
  ${EH_HOSTED_DIR}/common/eh_mempool/src/*.c
  ${EH_HOSTED_DIR}/common/eh_tlv/src/*.c
  ${EH_HOSTED_DIR}/host/eh_host_config/src/*.c
  ${EH_HOSTED_DIR}/host/mcu/eh_host_mcu_transport/src/*.c
  ${EH_HOSTED_DIR}/host/eh_host_core/src/*.c
  ${EH_HOSTED_DIR}/host/eh_host_core/src/mcu/*.c
  ${EH_HOSTED_DIR}/host/eh_host_autoinit/src/*.c
  ${EH_HOSTED_DIR}/host/features/eh_host_feat_rpc/src/*.c
  ${EH_HOSTED_DIR}/host/features/eh_host_feat_rpc_ext_v2/src/*.c
  ${EH_HOSTED_DIR}/host/features/eh_host_feat_system/src/*.c
  ${EH_HOSTED_DIR}/host/mcu/eh_host_mcu_vserial/src/*.c
  ${EH_HOSTED_DIR}/common/serializers/third_party/msg_codec/protobuf-c/protobuf-c/*.c
  ${EH_HOSTED_DIR}/port/os/posix/src/*.c
)

set(EH_HOSTED_SRCS
  ${EH_HOSTED_NUTTX_DIR}/src/eh_host_port_sdio.c
  ${EH_HOSTED_NUTTX_DIR}/src/esp_event_nuttx.c
  ${EH_HOSTED_NUTTX_DIR}/src/esp_mac_nuttx.c
  ${EH_HOSTED_NUTTX_DIR}/src/esp_timer_nuttx.c
  ${EH_HOSTED_NUTTX_DIR}/src/eh_hosted_nuttx_entry.c
  ${EH_HOSTED_NUTTX_DIR}/src/port_stubs.c
  ${EH_HOSTED_STACK_SRCS}
)

# Reuse the include directories the arch target has already been given, so this
# library sees exactly the NuttX/ESP-IDF headers the rest of arch sees -- plus
# its own shims, prepended.
get_target_property(EH_HOSTED_ARCH_INCS arch INCLUDE_DIRECTORIES)

add_library(eh_hosted_nuttx STATIC ${EH_HOSTED_SRCS})

target_include_directories(eh_hosted_nuttx BEFORE PRIVATE
  ${EH_HOSTED_NUTTX_DIR}/include
  ${EH_HOSTED_NUTTX_DIR}/../include
)

target_include_directories(eh_hosted_nuttx PRIVATE
  ${EH_HOSTED_ARCH_INCS}
  ${EH_HOSTED_DIR}/common/eh_common/include
  ${EH_HOSTED_DIR}/common/eh_frame/include
  ${EH_HOSTED_DIR}/common/eh_mempool/include
  ${EH_HOSTED_DIR}/common/eh_tlv/include
  ${EH_HOSTED_DIR}/host/eh_host_config/include
  ${EH_HOSTED_DIR}/host/mcu/eh_host_mcu_transport/include
  ${EH_HOSTED_DIR}/host/mcu/eh_host_mcu_transport/src
  ${EH_HOSTED_DIR}/host/features/eh_host_feat_rpc_ext_v2/include
  ${EH_HOSTED_DIR}/host/mcu/eh_host_mcu_vserial/include
  ${EH_HOSTED_DIR}/host/compat/include
  ${EH_HOSTED_DIR}/host/eh_host_core/include
  ${EH_HOSTED_DIR}/host/eh_host_core/src
  ${EH_HOSTED_DIR}/host/eh_host_autoinit/include
  ${EH_HOSTED_DIR}/host/features/eh_host_feat_rpc/include
  ${EH_HOSTED_DIR}/host/features/eh_host_feat_system/include
  ${EH_HOSTED_DIR}/host/mcu/eh_host_mcu_vserial/include
  ${EH_HOSTED_DIR}/common/serializers/eh_proto_v2/include
  ${EH_HOSTED_DIR}/common/serializers/third_party/msg_codec/protobuf-c
  ${EH_HOSTED_DIR}/port/os/posix/include
)

# Configuration of the official stack, in the symbols its own
# eh_host_port_master_config.h reads (board wiring from appendices AK/AN).
target_compile_definitions(eh_hosted_nuttx PRIVATE
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
  CONFIG_ESP_HOSTED_HOST_VSERIAL_MCU=1
  CONFIG_ESP_HOSTED_HOST_VSERIAL_MCU_READY=1
  CONFIG_ESP_HOSTED_HOST_CP_BRINGUP_ON_TIMEOUT_REATTEMPT=1
)

# The stack expects the master config to be force-included (its own build does
# this through an INTERFACE target).  Scoped to this library only.
set_source_files_properties(${EH_HOSTED_STACK_SRCS} PROPERTIES
  COMPILE_OPTIONS "-include;eh_host_port_master_config.h")

target_link_libraries(arch PRIVATE eh_hosted_nuttx)
