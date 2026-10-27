/****************************************************************************
 * chips/esp32p4/common/espressif/esp_hosted_sdio.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2026 The NuttX Contributors
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#ifdef CONFIG_ESPRESSIF_HOSTED_SDIO

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/arch.h>
#include <nuttx/mutex.h>

#include "hosted_ref_sdio.h"

#include "esp_cache.h"
#include "esp_clk_tree.h"
#include "esp_err.h"
#include "esp_gpio.h"
#include "esp_private/esp_clk_tree_common.h"
#include "esp_private/periph_ctrl.h"
#include "hal/sdmmc_ll.h"
#include "soc/gpio_sig_map.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define REFSDIO_SOURCE_CLOCK_KHZ 160000
#define REFSDIO_INITIAL_CLOCK_DIV       2
#define REFSDIO_CLOCK_SETTLE_US        10
#define REFSDIO_RESET_TIMEOUT_US   10000
#define REFSDIO_RESET_POLL_US         10
#define REFSDIO_COMMAND_TIMEOUT_US 100000
#define REFSDIO_DATA_TIMEOUT_US    200000
#define REFSDIO_DATA_ERRORS (SDMMC_LL_EVENT_RESP_ERR | \
                                    SDMMC_LL_EVENT_RCRC | \
                                    SDMMC_LL_EVENT_RTO | \
                                    SDMMC_LL_EVENT_DCRC | \
                                    SDMMC_LL_EVENT_DTO | \
                                    SDMMC_LL_EVENT_HTO | \
                                    SDMMC_LL_EVENT_HLE | \
                                    SDMMC_LL_EVENT_FRUN | \
                                    SDMMC_LL_EVENT_SBE | \
                                    SDMMC_LL_EVENT_EBE)
#define REFSDIO_DATA_EVENTS (REFSDIO_DATA_ERRORS | \
                                    SDMMC_LL_EVENT_CMD_DONE | \
                                    SDMMC_LL_EVENT_DATA_OVER | \
                                    SDMMC_LL_EVENT_RXDR | \
                                    SDMMC_LL_EVENT_TXDR)
#define REFSDIO_IDMAC_ERRORS (UINT32_C(1) << 2 | \
                                      UINT32_C(1) << 4 | \
                                      UINT32_C(1) << 5)

#define REFSDIO_COMMAND_EVENTS (SDMMC_LL_EVENT_CMD_DONE | \
                                       SDMMC_LL_EVENT_RESP_ERR | \
                                       SDMMC_LL_EVENT_RCRC | \
                                       SDMMC_LL_EVENT_RTO | \
                                       SDMMC_LL_EVENT_HLE)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct refsdio_s
{
  FAR sdmmc_dev_t *hw;
  uint32_t         clock_khz;
  uint32_t         revision;
  uint32_t         hardware_config;
  uint8_t          slot;
  uint8_t          bus_width;
  bool             initialized;
  bool             faulted;
  bool             dma_validated;
  sdmmc_desc_t     dma_descriptor __attribute__((aligned(64)));
  uint8_t          dma_buffer[4096] __attribute__((aligned(64)));
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct refsdio_s g_host;
static mutex_t g_host_lock = NXMUTEX_INITIALIZER;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int refsdio_validate_config(
  FAR const struct refsdio_config_s *config)
{
  if (config == NULL || config->slot > REFSDIO_SLOT1 ||
      (config->bus_width != REFSDIO_WIDTH1 &&
       config->bus_width != REFSDIO_WIDTH4) ||
      config->clock_khz < REFSDIO_PROBING_CLOCK_KHZ ||
      config->clock_khz > REFSDIO_MAX_CLOCK_KHZ)
    {
      return -EINVAL;
    }

  return OK;
}

static int refsdio_configure_pin(int pin, gpio_pinattr_t attr)
{
  if (esp_configgpio(pin, attr) < 0)
    {
      return -EIO;
    }

  return OK;
}

static int refsdio_wait_reset(FAR struct refsdio_s *host)
{
  uint32_t elapsed;

  for (elapsed = 0; elapsed < REFSDIO_RESET_TIMEOUT_US;
       elapsed += REFSDIO_RESET_POLL_US)
    {
      if (sdmmc_ll_is_controller_reset_done(host->hw) &&
          sdmmc_ll_is_dma_reset_done(host->hw) &&
          sdmmc_ll_is_fifo_reset_done(host->hw))
        {
          return OK;
        }

      up_udelay(REFSDIO_RESET_POLL_US);
    }

  return -ETIMEDOUT;
}

static int refsdio_apply_clock_update(
  FAR struct refsdio_s *host)
{
  sdmmc_hw_cmd_t command;
  uint32_t elapsed;

  memset(&command, 0, sizeof(command));
  command.card_num = host->slot;
  command.update_clk_reg = 1;
  command.wait_complete = 1;
  command.start_command = 1;
  sdmmc_ll_set_command_arg(host->hw, 0);
  sdmmc_ll_set_command(host->hw, command);

  for (elapsed = 0; elapsed < REFSDIO_RESET_TIMEOUT_US;
       elapsed += REFSDIO_RESET_POLL_US)
    {
      if (sdmmc_ll_is_command_taken(host->hw))
        {
          break;
        }

      up_udelay(REFSDIO_RESET_POLL_US);
    }

  if (!sdmmc_ll_is_command_taken(host->hw))
    {
      return -ETIMEDOUT;
    }

  return OK;
}

static int refsdio_update_clock(FAR struct refsdio_s *host,
                                         uint32_t clock_khz)
{
  uint32_t host_div;
  uint32_t card_div;
  int ret;

  if (clock_khz < REFSDIO_PROBING_CLOCK_KHZ ||
      clock_khz > REFSDIO_MAX_CLOCK_KHZ)
    {
      return -EINVAL;
    }

  /* The P4 SDMMC controller has two divider stages.  Keep the controller
   * clock within the low-speed divider range and select the closest clock
   * that does not exceed the caller's request.
   */

  host_div = (REFSDIO_SOURCE_CLOCK_KHZ + clock_khz - 1) /
             clock_khz;
  card_div = 0;

  if (host_div > 16)
    {
      host_div = 16;
      card_div = (REFSDIO_SOURCE_CLOCK_KHZ +
                  (host_div * 2 * clock_khz) - 1) /
                 (host_div * 2 * clock_khz);
    }

  if (host_div == 0 || host_div > 16 || card_div > 255)
    {
      return -ERANGE;
    }

  /* CLKENA, CLKDIV and CLKSRC are shadowed in the CIU.  Synchronize after
   * every change, including after the card clock is enabled.
   */

  sdmmc_ll_enable_card_clock(host->hw, host->slot, false);
  ret = refsdio_apply_clock_update(host);
  if (ret < 0)
    {
      return ret;
    }

  PERIPH_RCC_ATOMIC()
    {
      sdmmc_ll_set_clock_div(host->hw, host_div);
    }

  sdmmc_ll_set_card_clock_div(host->hw, host->slot, card_div);
  ret = refsdio_apply_clock_update(host);
  if (ret < 0)
    {
      return ret;
    }

  sdmmc_ll_enable_card_clock(host->hw, host->slot, true);
  sdmmc_ll_enable_card_clock_low_power(host->hw, host->slot, true);
  ret = refsdio_apply_clock_update(host);
  if (ret < 0)
    {
      return ret;
    }

  host->clock_khz = REFSDIO_SOURCE_CLOCK_KHZ / host_div /
                    (card_div == 0 ? 1 : card_div * 2);
  return OK;
}

static void refsdio_command_error(
  FAR struct refsdio_s *host, uint8_t command, uint32_t argument,
  FAR const char *reason, uint32_t raw, uint32_t elapsed)
{
  /* Capture the command engine before clearing its events or resetting it.
   * RESP0 can belong to the previous command when the current one fails.
   */

  uint32_t cmd = host->hw->cmd.val;
  uint32_t status = host->hw->status.val;
  uint32_t timeout = host->hw->tmout.val;
  uint32_t clkena = host->hw->clkena.val;
  uint32_t idsts = sdmmc_ll_get_idsts_interrupt_raw(host->hw);
  uint32_t last_response = host->hw->resp[0];

  syslog(LOG_ERR,
         "ERROR: ESP-Hosted SDIO: cmd=%u arg=0x%08" PRIx32
         " reason=%s elapsed_us=%" PRIu32 " raw=0x%08" PRIx32 "\n",
         (unsigned int)command, argument, reason, elapsed, raw);

  if (command == 53)
    {
      uint32_t function = (argument >> 28) & 0x7;
      uint32_t address = (argument >> 9) & 0x1ffff;
      uint32_t count = argument & 0x1ff;

      /* CMD53 count zero denotes 512 bytes in byte mode. */

      if (count == 0 && (argument & (UINT32_C(1) << 27)) == 0)
        {
          count = 512;
        }

      syslog(LOG_ERR,
             "ERROR: ESP-Hosted SDIO CMD53: %s fn=%" PRIu32
             " addr=0x%05" PRIx32 " mode=%s count=%" PRIu32
             " rto=%u dto=%u hto=%u\n",
             (argument & (UINT32_C(1) << 31)) != 0 ? "write" : "read",
             function, address,
             (argument & (UINT32_C(1) << 27)) != 0 ? "block" : "byte",
             count, (unsigned int)((raw & SDMMC_LL_EVENT_RTO) != 0),
             (unsigned int)((raw & SDMMC_LL_EVENT_DTO) != 0),
             (unsigned int)((raw & SDMMC_LL_EVENT_HTO) != 0));
    }

  syslog(LOG_ERR,
         "ERROR: ESP-Hosted SDIO snapshot: cmd=0x%08" PRIx32
         " status=0x%08" PRIx32 " tmout=0x%08" PRIx32
         " clkena=0x%08" PRIx32 " idsts=0x%08" PRIx32
         " last_resp=0x%08" PRIx32
         " clock_khz=%" PRIu32 "\n",
         cmd, status, timeout, clkena, idsts, last_response,
         host->clock_khz);
}

static void refsdio_dma_snapshot(
  FAR struct refsdio_s *host, FAR const char *stage)
{
  syslog(LOG_INFO,
         "INFO: ESP-Hosted SDIO DMA: stage=%s ctrl=0x%08" PRIx32
         " bmod=0x%08" PRIx32 " status=0x%08" PRIx32
         " raw=0x%08" PRIx32 " idsts=0x%08" PRIx32
         " bytcnt=%" PRIu32 " tcbcnt=%" PRIu32 " tbbcnt=%" PRIu32
         " fifoth=0x%08" PRIx32 " own=%u error=%u size=%u\n",
         stage, host->hw->ctrl.val, host->hw->bmod.val,
         host->hw->status.val, sdmmc_ll_get_interrupt_raw(host->hw),
         sdmmc_ll_get_idsts_interrupt_raw(host->hw),
         host->hw->bytcnt.byte_count, host->hw->tcbcnt.tcbcnt_reg,
         host->hw->tbbcnt.tbbcnt_reg, host->hw->fifoth.val,
         (unsigned int)host->dma_descriptor.owned_by_idmac,
         (unsigned int)host->dma_descriptor.card_error_summary,
         (unsigned int)host->dma_descriptor.buffer1_size);
}

static int refsdio_do_command(FAR struct refsdio_s *host,
                                      uint8_t command, uint32_t argument,
                                      bool response, bool check_crc,
                                      bool send_init,
                                      FAR uint32_t *response_value)
{
  sdmmc_hw_cmd_t command_register;
  uint32_t interrupt_status;
  uint32_t elapsed;

  if (!sdmmc_ll_is_command_taken(host->hw))
    {
      refsdio_command_error(host, command, argument, "not-taken",
                                   sdmmc_ll_get_interrupt_raw(host->hw), 0);
      return -EBUSY;
    }

  sdmmc_ll_clear_interrupt(host->hw, REFSDIO_COMMAND_EVENTS);

  memset(&command_register, 0, sizeof(command_register));
  command_register.cmd_index = command;
  command_register.card_num = host->slot;
  command_register.response_expect = response;
  command_register.check_response_crc = check_crc;
  command_register.send_init = send_init;
  command_register.wait_complete = !send_init;
  command_register.use_hold_reg = true;
  command_register.start_command = true;
  sdmmc_ll_set_command_arg(host->hw, argument);
  sdmmc_ll_set_command(host->hw, command_register);

  for (elapsed = 0; elapsed < REFSDIO_COMMAND_TIMEOUT_US;
       elapsed += REFSDIO_RESET_POLL_US)
    {
      interrupt_status = sdmmc_ll_get_interrupt_raw(host->hw);
      if ((interrupt_status & REFSDIO_COMMAND_EVENTS) != 0)
        {
          if ((interrupt_status & SDMMC_LL_EVENT_HLE) != 0)
            {
              refsdio_command_error(host, command, argument,
                                           "hardware-lock", interrupt_status,
                                           elapsed);
              sdmmc_ll_clear_interrupt(host->hw,
                interrupt_status & REFSDIO_COMMAND_EVENTS);
              return -EIO;
            }

          if ((interrupt_status & SDMMC_LL_EVENT_RTO) != 0)
            {
              refsdio_command_error(host, command, argument,
                                           "response-timeout",
                                           interrupt_status, elapsed);
              sdmmc_ll_clear_interrupt(host->hw,
                interrupt_status & REFSDIO_COMMAND_EVENTS);
              return -ETIMEDOUT;
            }

          if ((interrupt_status & (SDMMC_LL_EVENT_RESP_ERR |
                                   SDMMC_LL_EVENT_RCRC)) != 0)
            {
              refsdio_command_error(host, command, argument,
                                           "response-error",
                                           interrupt_status, elapsed);
              sdmmc_ll_clear_interrupt(host->hw,
                interrupt_status & REFSDIO_COMMAND_EVENTS);
              return -EIO;
            }

          sdmmc_ll_clear_interrupt(host->hw,
            interrupt_status & REFSDIO_COMMAND_EVENTS);

          if (response && response_value != NULL)
            {
              *response_value = host->hw->resp[0];
            }

          syslog(LOG_INFO,
                 "INFO: ESP-Hosted SDIO: cmd=%u arg=0x%08" PRIx32
                 " raw=0x%08" PRIx32 " elapsed_us=%" PRIu32 "\n",
                 (unsigned int)command, argument, interrupt_status, elapsed);
          return OK;
        }

      up_udelay(REFSDIO_RESET_POLL_US);
    }

  refsdio_command_error(host, command, argument, "software-timeout",
                               sdmmc_ll_get_interrupt_raw(host->hw),
                               elapsed);
  return -ETIMEDOUT;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int refsdio_route_pins(
  FAR const struct refsdio_pins_s *pins)
{
  int ret;

  if (pins == NULL)
    {
      return -EINVAL;
    }

  ret = refsdio_configure_pin(pins->clk_gpio, OUTPUT);
  if (ret < 0)
    {
      return ret;
    }

  esp_gpio_matrix_out(pins->clk_gpio, SD_CARD_CCLK_2_PAD_OUT_IDX, false,
                      false);

  ret = refsdio_configure_pin(pins->cmd_gpio,
                                      INPUT | OUTPUT | PULLUP);
  if (ret < 0)
    {
      return ret;
    }

  esp_gpio_matrix_in(pins->cmd_gpio, SD_CARD_CCMD_2_PAD_IN_IDX, false);
  esp_gpio_matrix_out(pins->cmd_gpio, SD_CARD_CCMD_2_PAD_OUT_IDX, false,
                      false);

  ret = refsdio_configure_pin(pins->d0_gpio,
                                      INPUT | OUTPUT | PULLUP);
  if (ret < 0)
    {
      return ret;
    }

  esp_gpio_matrix_in(pins->d0_gpio, SD_CARD_CDATA0_2_PAD_IN_IDX, false);
  esp_gpio_matrix_out(pins->d0_gpio, SD_CARD_CDATA0_2_PAD_OUT_IDX, false,
                      false);

  ret = refsdio_configure_pin(pins->d1_gpio, INPUT | PULLUP);
  if (ret < 0)
    {
      return ret;
    }

  ret = refsdio_configure_pin(pins->d2_gpio, INPUT | PULLUP);
  if (ret < 0)
    {
      return ret;
    }

  ret = refsdio_configure_pin(pins->d3_gpio, OUTPUT | PULLUP);
  if (ret < 0)
    {
      return ret;
    }

  esp_gpiowrite(pins->d3_gpio, true);
  return OK;
}

int refsdio_initialize(
  FAR const struct refsdio_config_s *config,
  FAR struct refsdio_s **host)
{
  int ret;

  if (host == NULL)
    {
      return -EINVAL;
    }

  *host = NULL;
  ret = refsdio_validate_config(config);
  if (ret < 0)
    {
      return ret;
    }

  ret = nxmutex_lock(&g_host_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (g_host.initialized)
    {
      ret = -EBUSY;
      goto out;
    }

  memset(&g_host, 0, sizeof(g_host));
  g_host.hw = SDMMC_LL_GET_HW(0);
  if (g_host.hw == NULL)
    {
      ret = -ENODEV;
      goto out;
    }

  if (esp_clk_tree_enable_src((soc_module_clk_t)SDMMC_CLK_SRC_DEFAULT,
                              true) != ESP_OK)
    {
      ret = -EIO;
      goto out;
    }

  PERIPH_RCC_ATOMIC()
    {
      sdmmc_ll_enable_bus_clock(0, true);
      sdmmc_ll_reset_register(0);
      sdmmc_ll_set_clock_div(g_host.hw, REFSDIO_INITIAL_CLOCK_DIV);
      sdmmc_ll_select_clk_source(g_host.hw, SDMMC_CLK_SRC_DEFAULT);
      sdmmc_ll_init_phase_delay(g_host.hw);
    }

  /* The internal reset bits clear only after the selected controller clock
   * has propagated.  This is the same ordering used by Espressif's SDMMC
   * host controller before it waits for the reset bits.
   */

  up_udelay(REFSDIO_CLOCK_SETTLE_US);
  sdmmc_ll_reset_controller(g_host.hw);
  sdmmc_ll_reset_dma(g_host.hw);
  sdmmc_ll_reset_fifo(g_host.hw);
  ret = refsdio_wait_reset(&g_host);
  if (ret < 0)
    {
      goto disable_clock;
    }

  sdmmc_ll_clear_interrupt(g_host.hw, UINT32_MAX);
  sdmmc_ll_enable_global_interrupt(g_host.hw, false);
  sdmmc_ll_enable_interrupt(g_host.hw, UINT32_MAX, false);
  sdmmc_ll_init_dma(g_host.hw);
  sdmmc_ll_enable_dma(g_host.hw, false);
  sdmmc_ll_set_data_timeout(g_host.hw, UINT32_MAX);
  sdmmc_ll_set_response_timeout(g_host.hw, 0xff);
  sdmmc_ll_set_card_width(g_host.hw, config->slot,
                          config->bus_width == REFSDIO_WIDTH4 ?
                          SD_BUS_WIDTH_4_BIT : SD_BUS_WIDTH_1_BIT);

  g_host.slot = config->slot;
  g_host.bus_width = config->bus_width;
  ret = refsdio_update_clock(&g_host, config->clock_khz);
  if (ret < 0)
    {
      goto disable_clock;
    }

  g_host.revision = sdmmc_ll_get_version_id(g_host.hw);
  g_host.hardware_config = sdmmc_ll_get_hw_config_info(g_host.hw);
  g_host.initialized = true;
  *host = &g_host;
  ret = OK;
  goto out;

disable_clock:
  PERIPH_RCC_ATOMIC()
    {
      sdmmc_ll_enable_bus_clock(0, false);
    }

  esp_clk_tree_enable_src((soc_module_clk_t)SDMMC_CLK_SRC_DEFAULT, false);
  memset(&g_host, 0, sizeof(g_host));
out:
  nxmutex_unlock(&g_host_lock);
  return ret;
}

int refsdio_set_clock(FAR struct refsdio_s *host,
                               uint32_t clock_khz)
{
  int ret;

  if (host != &g_host)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&g_host_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!g_host.initialized)
    {
      ret = -EPIPE;
    }
  else
    {
      ret = refsdio_update_clock(&g_host, clock_khz);
    }

  nxmutex_unlock(&g_host_lock);
  return ret;
}

int refsdio_command(FAR struct refsdio_s *host,
                             uint8_t command, uint32_t argument,
                             bool response, bool check_crc, bool send_init,
                             FAR uint32_t *response_value)
{
  int ret;

  if (host != &g_host || (!response && response_value != NULL))
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&g_host_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!g_host.initialized)
    {
      ret = -EPIPE;
    }
  else if (g_host.faulted)
    {
      ret = -EIO;
    }
  else
    {
      ret = refsdio_do_command(&g_host, command, argument, response,
                                       check_crc, send_init, response_value);
    }

  nxmutex_unlock(&g_host_lock);
  return ret;
}

int refsdio_transfer(FAR struct refsdio_s *host,
                            uint32_t argument, FAR void *buffer,
                            size_t length, uint16_t block_size)
{
  FAR uint8_t *bytes = buffer;
  sdmmc_hw_cmd_t command;
  uint32_t elapsed = 0;
  uint32_t raw = 0;
  uint32_t events = 0;
  uint32_t idsts = 0;
  uint32_t word;
  size_t count = argument & 0x1ff;
  size_t offset = 0;
  size_t chunk;
  bool write = (argument & (UINT32_C(1) << 31)) != 0;
  bool blocks = (argument & (UINT32_C(1) << 27)) != 0;
  bool dma = length > sizeof(word);
  int ret;

  if (host != &g_host || buffer == NULL || length == 0 || length > 4096 ||
      block_size == 0 || block_size > 512 ||
      (blocks && (count == 0 || count * block_size != length)) ||
      (!blocks && (count == 0 ? 512 : count) != length))
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&g_host_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!host->initialized || host->faulted)
    {
      ret = -EIO;
      goto out;
    }

  if (!sdmmc_ll_is_command_taken(host->hw))
    {
      ret = -EBUSY;
      goto out;
    }

  /* CMD52 and four-byte CMD53 register accesses use PIO, which is the
   * verified control path.  Larger CMD53 payloads use the SDMMC IDMAC.  The
   * static bounce buffer avoids imposing cache-line alignment restrictions
   * on ESP-Hosted callers.
   */

  sdmmc_ll_enable_dma(host->hw, false);
  if (dma)
    {
      sdmmc_ll_reset_dma(host->hw);
    }

  sdmmc_ll_reset_fifo(host->hw);
  ret = refsdio_wait_reset(host);
  if (ret < 0)
    {
      goto fault;
    }

  /* Program the new transfer before enabling IDMAC.  Otherwise it can
   * service a request using the preceding PIO transaction's byte count.
   */

  sdmmc_ll_clear_interrupt(host->hw, REFSDIO_DATA_EVENTS);
  sdmmc_ll_set_block_size(host->hw, blocks ? block_size : length);
  sdmmc_ll_set_data_transfer_len(host->hw, length);

  if (dma)
    {
      /* A DMA reset clears the IDMAC bus mode and interrupt enables. */

      sdmmc_ll_init_dma(host->hw);
      sdmmc_ll_enable_dma(host->hw, false);
      memset(&host->dma_descriptor, 0, sizeof(host->dma_descriptor));
      if (write)
        {
          memcpy(host->dma_buffer, bytes, length);
          ret = esp_cache_msync(host->dma_buffer,
                                sizeof(host->dma_buffer),
                                ESP_CACHE_MSYNC_FLAG_DIR_C2M);
          if (ret != ESP_OK)
            {
              ret = -EIO;
              goto out;
            }
        }
      else
        {
          /* Write back any previous contents before IDMAC overwrites this
           * buffer, then invalidate it after DATA_OVER below.
           */

          ret = esp_cache_msync(host->dma_buffer, sizeof(host->dma_buffer),
                                ESP_CACHE_MSYNC_FLAG_DIR_C2M);
          if (ret != ESP_OK)
            {
              ret = -EIO;
              goto out;
            }
        }

      host->dma_descriptor.first_descriptor = 1;
      host->dma_descriptor.last_descriptor = 1;
      host->dma_descriptor.second_address_chained = 1;
      host->dma_descriptor.owned_by_idmac = 1;
      host->dma_descriptor.buffer1_size = (length + 3) & ~3;
      host->dma_descriptor.buffer1_ptr = host->dma_buffer;
      host->dma_descriptor.next_desc_ptr = NULL;
      ret = esp_cache_msync(&host->dma_descriptor,
                            sizeof(host->dma_descriptor),
                            ESP_CACHE_MSYNC_FLAG_DIR_C2M);
      if (ret != ESP_OK)
        {
          ret = -EIO;
          goto out;
        }

      sdmmc_ll_clear_idsts_interrupt(host->hw, UINT32_MAX);
      sdmmc_ll_set_desc_addr(host->hw,
                             (uint32_t)(uintptr_t)&host->dma_descriptor);
      if (!host->dma_validated)
        {
          refsdio_dma_snapshot(host, "prepared");
        }

      sdmmc_ll_enable_dma(host->hw, true);
      sdmmc_ll_poll_demand(host->hw);
    }

  memset(&command, 0, sizeof(command));
  command.cmd_index = 53;
  command.card_num = host->slot;
  command.response_expect = 1;
  command.check_response_crc = 1;
  command.data_expected = 1;
  command.rw = write;
  command.wait_complete = 1;
  command.use_hold_reg = 1;
  command.start_command = 1;
  sdmmc_ll_set_command_arg(host->hw, argument);
  sdmmc_ll_set_command(host->hw, command);
  if (dma && !host->dma_validated)
    {
      refsdio_dma_snapshot(host, "started");
    }

  for (elapsed = 0; elapsed < REFSDIO_DATA_TIMEOUT_US; elapsed++)
    {
      raw = sdmmc_ll_get_interrupt_raw(host->hw);
      events |= raw & REFSDIO_DATA_EVENTS;
      idsts = sdmmc_ll_get_idsts_interrupt_raw(host->hw);
      if ((events & REFSDIO_DATA_ERRORS) != 0 ||
          (dma && (idsts & REFSDIO_IDMAC_ERRORS) != 0))
        {
          ret = (events & (SDMMC_LL_EVENT_RTO | SDMMC_LL_EVENT_DTO |
                           SDMMC_LL_EVENT_HTO)) != 0 ? -ETIMEDOUT : -EIO;
          goto fault;
        }

      if ((events & SDMMC_LL_EVENT_CMD_DONE) != 0 &&
          (host->hw->resp[0] & UINT32_C(0xcb00)) != 0)
        {
          ret = -EIO;
          goto fault;
        }

      if (!dma && offset < length &&
          (write ? !host->hw->status.fifo_full :
                   host->hw->status.fifo_count != 0))
        {
          chunk = length - offset;
          if (chunk > sizeof(word))
            {
              chunk = sizeof(word);
            }

          if (write)
            {
              word = 0;
              memcpy(&word, bytes + offset, chunk);
              host->hw->buffifo.val = word;
            }
          else
            {
              word = host->hw->buffifo.val;
              memcpy(bytes + offset, &word, chunk);
            }

          offset += chunk;
        }

      sdmmc_ll_clear_interrupt(host->hw,
                               raw & REFSDIO_DATA_EVENTS);

      if (!dma && (events & SDMMC_LL_EVENT_DATA_OVER) != 0 &&
          offset != length)
        {
          ret = -EIO;
          goto fault;
        }

      if ((events & (SDMMC_LL_EVENT_CMD_DONE | SDMMC_LL_EVENT_DATA_OVER)) ==
          (SDMMC_LL_EVENT_CMD_DONE | SDMMC_LL_EVENT_DATA_OVER) &&
          (dma || offset == length))
        {
          if (dma)
            {
              /* IDMAC writes OWN through memory, outside the CPU cache. */

              ret = esp_cache_msync(&host->dma_descriptor,
                                    sizeof(host->dma_descriptor),
                                    ESP_CACHE_MSYNC_FLAG_DIR_M2C);
              if (ret != ESP_OK)
                {
                  ret = -EIO;
                  goto fault;
                }

              if (host->dma_descriptor.owned_by_idmac)
                {
                  up_udelay(1);
                  continue;
                }

              if (host->dma_descriptor.card_error_summary)
                {
                  ret = -EIO;
                  goto fault;
                }

              sdmmc_ll_enable_dma(host->hw, false);
              sdmmc_ll_clear_idsts_interrupt(host->hw, UINT32_MAX);
              if (!write)
                {
                  ret = esp_cache_msync(host->dma_buffer,
                                        sizeof(host->dma_buffer),
                                        ESP_CACHE_MSYNC_FLAG_DIR_M2C);
                  if (ret != ESP_OK)
                    {
                      ret = -EIO;
                      goto fault;
                    }

                  memcpy(bytes, host->dma_buffer, length);
                }

              host->dma_validated = true;
            }

          ret = OK;
          goto out;
        }

      up_udelay(1);
    }

  ret = -ETIMEDOUT;
fault:
  if (dma && !host->dma_validated)
    {
      refsdio_dma_snapshot(host, "failed");
    }

  refsdio_command_error(host, 53, argument, "data-transfer",
                                raw, elapsed);
  host->faulted = true;
  sdmmc_ll_enable_dma(host->hw, false);
  sdmmc_ll_clear_idsts_interrupt(host->hw, UINT32_MAX);
  sdmmc_ll_reset_controller(host->hw);
  sdmmc_ll_reset_dma(host->hw);
  sdmmc_ll_reset_fifo(host->hw);
  refsdio_wait_reset(host);
out:
  nxmutex_unlock(&g_host_lock);
  return ret;
}

int refsdio_get_status(FAR struct refsdio_s *host,
                                FAR struct refsdio_status_s *status)
{
  int ret;

  if (host != &g_host || status == NULL)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&g_host_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!g_host.initialized)
    {
      ret = -EPIPE;
    }
  else
    {
      status->revision = g_host.revision;
      status->hardware_config = g_host.hardware_config;
      status->clock_khz = g_host.clock_khz;
      status->slot = g_host.slot;
      status->bus_width = g_host.bus_width;
      status->initialized = g_host.initialized;
      ret = OK;
    }

  nxmutex_unlock(&g_host_lock);
  return ret;
}

int refsdio_deinitialize(FAR struct refsdio_s *host)
{
  int ret;

  if (host != &g_host)
    {
      return -EINVAL;
    }

  ret = nxmutex_lock(&g_host_lock);
  if (ret < 0)
    {
      return ret;
    }

  if (!g_host.initialized)
    {
      ret = -EPIPE;
      goto out;
    }

  sdmmc_ll_enable_card_clock(g_host.hw, g_host.slot, false);
  sdmmc_ll_enable_dma(g_host.hw, false);
  sdmmc_ll_enable_global_interrupt(g_host.hw, false);
  sdmmc_ll_enable_interrupt(g_host.hw, UINT32_MAX, false);
  sdmmc_ll_clear_interrupt(g_host.hw, UINT32_MAX);
  PERIPH_RCC_ATOMIC()
    {
      sdmmc_ll_enable_bus_clock(0, false);
    }

  esp_clk_tree_enable_src((soc_module_clk_t)SDMMC_CLK_SRC_DEFAULT, false);
  memset(&g_host, 0, sizeof(g_host));
  ret = OK;

out:
  nxmutex_unlock(&g_host_lock);
  return ret;
}

#endif /* CONFIG_ESPRESSIF_HOSTED_SDIO */
