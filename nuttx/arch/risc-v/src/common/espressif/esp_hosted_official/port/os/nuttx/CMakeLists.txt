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
  ${EH_HOSTED_DIR}/host/features/eh_host_feat_wifi/src/*.c
  ${EH_HOSTED_DIR}/host/features/eh_host_feat_system/src/*.c
  ${EH_HOSTED_DIR}/host/mcu/eh_host_mcu_vserial/src/*.c
  ${EH_HOSTED_DIR}/common/serializers/third_party/msg_codec/protobuf-c/protobuf-c/*.c
  ${EH_HOSTED_DIR}/common/serializers/eh_proto_v2/src/*.c
  ${EH_HOSTED_DIR}/port/os/posix/src/*.c
  ${EH_HOSTED_NUTTX_DIR}/src/eh_host_port_transport_config.c
  ${EH_HOSTED_NUTTX_DIR}/src/eh_host_port_transport_defaults.c
)

set(EH_HOSTED_SRCS
  ${EH_HOSTED_NUTTX_DIR}/src/eh_host_port_sdio.c
  ${EH_HOSTED_NUTTX_DIR}/src/esp_event_extra_nuttx.c
  ${EH_HOSTED_NUTTX_DIR}/src/esp_wifi_stub.c
  ${EH_HOSTED_NUTTX_DIR}/src/eh_hosted_netdev.c
  ${EH_HOSTED_NUTTX_DIR}/src/eh_hosted_nuttx_entry.c
  ${EH_HOSTED_NUTTX_DIR}/src/eh_host_port_task_nuttx.c
  ${EH_HOSTED_NUTTX_DIR}/src/port_stubs.c
  ${EH_HOSTED_STACK_SRCS}
)

# Reuse the include directories the arch target has already been given, so this
# library sees exactly the NuttX/ESP-IDF headers the rest of arch sees -- plus
# its own shims, prepended.
get_target_property(EH_HOSTED_ARCH_INCS arch INCLUDE_DIRECTORIES)

# OBJECT, not STATIC: the port carries its own include order (shims first), but
# as a separate archive it landed after libc.a on the link line, so its
# references into libc (aligned_alloc, pthread_condattr_*) could not be resolved
# -- a static archive is only searched when the linker walks past it.  Object
# files join the arch link in one pass and keep the same isolation.
add_library(eh_hosted_nuttx OBJECT ${EH_HOSTED_SRCS})

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
  ${EH_HOSTED_DIR}/host/features/eh_host_feat_wifi/include
  ${EH_HOSTED_DIR}/host/features/eh_host_feat_wifi/src
  ${EH_HOSTED_DIR}/host/features/eh_host_feat_system/include
  ${EH_HOSTED_DIR}/host/mcu/eh_host_mcu_vserial/include
  ${EH_HOSTED_DIR}/common/serializers/eh_proto_v2/include
  ${EH_HOSTED_DIR}/common/serializers/third_party/msg_codec/protobuf-c
  ${EH_HOSTED_DIR}/port/os/posix/include
)

# The ESP-IDF header surface comes from the tree's own esp-hal-3rdparty
# checkout, not from shims: the Wi-Fi feature needs the real esp_wifi.h family
# (wifi_config_t, wifi_ps_type_t, esp_wifi_he_types.h, ...), and re-typing that
# in this port would be both wrong and pointless.  ESP_HAL_3RDPARTY_REPO is the
# variable the parent CMakeLists already resolved (esp_hal_3rdparty_SOURCE_DIR,
# the FetchContent name, is not visible here -- measured).
if(DEFINED ESP_HAL_3RDPARTY_REPO AND IS_DIRECTORY "${ESP_HAL_3RDPARTY_REPO}/components")
  file(GLOB EH_HOSTED_IDF_INCS "${ESP_HAL_3RDPARTY_REPO}/components/*/include")
  target_include_directories(eh_hosted_nuttx PRIVATE ${EH_HOSTED_IDF_INCS})
  message(STATUS "esp-hosted: ${ESP_HAL_3RDPARTY_REPO} IDF headers on the port's include path")
endif()

# Configuration of the official stack, in the symbols its own
# eh_host_port_master_config.h reads (board wiring from appendices AK/AN).
target_compile_definitions(eh_hosted_nuttx PRIVATE
  CONFIG_ESP_HOSTED_HOST_CORE=1
  CONFIG_ESP_HOSTED_HOST_FEAT_SYSTEM=1
  CONFIG_ESP_HOSTED_HOST_FEAT_SYSTEM_READY=1
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
  CONFIG_ESP_HOSTED_HOST_SDIO_TX_Q_SIZE=20
  CONFIG_ESP_HOSTED_HOST_SDIO_RX_Q_SIZE=20
  EH_HOST_HOST_USES_STATIC_NETIF=0
  CONFIG_ESP_HOSTED_CP_TARGET_ESP32C6=1
  CONFIG_ESP_HOSTED_USE_MEMPOOL=1
  CONFIG_ESP_HOSTED_HOST_VSERIAL_MCU=1
  CONFIG_ESP_HOSTED_HOST_VSERIAL_MCU_READY=1
  CONFIG_ESP_HOSTED_HOST_CP_BRINGUP_ON_TIMEOUT_REATTEMPT=1
  CONFIG_ESP_HOSTED_HOST_CP_BRINGUP_REATTEMPT_MAX=3
  CONFIG_ESP_HOSTED_HOST_CP_RESET_STRATEGY_ONLY_IF_NECESSARY=1
)

# The stack expects the master config to be force-included (its own build does
# this through an INTERFACE target).  Scoped to this library only.
set_source_files_properties(${EH_HOSTED_STACK_SRCS} PROPERTIES
  COMPILE_OPTIONS "-include;eh_host_port_master_config.h")

# NuttX provides neither TLS nor an emutls runtime on this target, so the one
# _Thread_local variable in the stack (the RPC dispatcher's re-entrancy guard in
# eh_host_feat_rpc_evt.c) cannot be honoured.  Emptying the storage class makes
# it an ordinary static: equivalent while a single context dispatches RPC events,
# which is what this port does.  Revisit when TLS support lands in the tree.
set_source_files_properties(
  ${EH_HOSTED_DIR}/host/features/eh_host_feat_rpc/src/eh_host_feat_rpc_evt.c
  PROPERTIES COMPILE_DEFINITIONS "_Thread_local=")

target_sources(arch PRIVATE $<TARGET_OBJECTS:eh_hosted_nuttx>)
