/****************************************************************************
 * boards/risc-v/esp32p4/esp32p4-function-ev-board/src/esp32p4_bringup.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <fcntl.h>
#include <syslog.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <nuttx/fs/fs.h>

#include "esp_board_ledc.h"
#include "esp_board_spiflash.h"
#include "esp_board_i2c.h"
#include "esp_board_bmp180.h"

#include "espressif/esp_start.h"

#ifdef CONFIG_WATCHDOG
#  include "espressif/esp_wdt.h"
#endif

#ifdef CONFIG_TIMER
#  include "espressif/esp_gptimer.h"
#endif

#ifdef CONFIG_ONESHOT
#  include "espressif/esp_oneshot.h"
#endif

#ifdef CONFIG_RTC_DRIVER
#  include "espressif/esp_rtc.h"
#endif

#ifdef CONFIG_DEV_GPIO
#  include "espressif/esp_gpio.h"
#endif

#ifdef CONFIG_INPUT_BUTTONS
#  include <nuttx/input/buttons.h>
#endif

#ifdef CONFIG_ESPRESSIF_EFUSE
#  include "espressif/esp_efuse.h"
#endif

#ifdef CONFIG_ESP_RMT
#  include "esp_board_rmt.h"
#endif

#ifdef CONFIG_ESPRESSIF_I2S
#  include "esp_board_i2s.h"
#endif

#ifdef CONFIG_ESPRESSIF_SPI
#  include "espressif/esp_spi.h"
#  include "esp_board_spidev.h"
#  ifdef CONFIG_ESPRESSIF_SPI_BITBANG
#    include "espressif/esp_spi_bitbang.h"
#  endif
#endif

#ifdef CONFIG_SPI_SLAVE_DRIVER
#  include "espressif/esp_spi.h"
#  include "esp_board_spislavedev.h"
#endif

#ifdef CONFIG_ESPRESSIF_TEMP
#  include "espressif/esp_temperature_sensor.h"
#endif

#ifdef CONFIG_ESP_MCPWM
#  include "esp_board_mcpwm.h"
#endif

#ifdef CONFIG_ESP_PCNT
#  include "espressif/esp_pcnt.h"
#  include "esp_board_pcnt.h"
#endif

#ifdef CONFIG_ESPRESSIF_ADC
#  include "esp_board_adc.h"
#endif

#ifdef CONFIG_PM
#  include "espressif/esp_pm.h"
#endif

#ifdef CONFIG_SYSTEM_NXDIAG_ESPRESSIF_CHIP_WO_TOOL
#  include "espressif/esp_nxdiag.h"
#endif

#ifdef CONFIG_ESP_SDM
#  include "espressif/esp_sdm.h"
#endif

#ifdef CONFIG_COMP
#  include "espressif/esp_ana_cmpr.h"
#endif

/* Temporary bring-up experiment: skip the MIPI-DSI host and the EK79007
 * framebuffer so the camera can be exercised with the display completely out
 * of the picture.
 *
 * The camera never receives a byte even though the SC2336 is verified
 * configured and streaming (PLL locked, HTS/VTS correct, 0x0100 reads back 1)
 * and the whole receive chain reports configured and enabled (CSI controller
 * start rc=0, PHY LDO on, hs_freq range 0x25, host active lanes = 2, bridge
 * enabled with the RAW10 data type, DW-GDMA armed for exactly one frame).
 * That leaves the MIPI D-PHY link itself, and the DSI is the obvious
 * candidate for interfering with it: it is initialised right after the camera
 * and it is the one that works.
 *
 * Set to 0 (or delete) once the question is answered.
 */

#define ESP32P4_CAMERA_WITHOUT_DSI_TEST 0

#if defined(CONFIG_ESP32P4_FUNCTION_EV_BOARD_MIPI_DSI) && \
    !ESP32P4_CAMERA_WITHOUT_DSI_TEST
#  include "espressif/esp_mipi_dsi.h"
#endif

#if defined(CONFIG_ESP32P4_FUNCTION_EV_BOARD_LCD) && \
    !ESP32P4_CAMERA_WITHOUT_DSI_TEST
#  include <nuttx/video/fb.h>
#endif

#ifdef CONFIG_ESPRESSIF_USE_LP_CORE
#  include "espressif/esp_ulp.h"
#  ifdef CONFIG_ESPRESSIF_ULP_USE_TEST_BIN
#    include "ulp/ulp_code.h"
#  endif
#  ifdef CONFIG_ESPRESSIF_LP_MAILBOX
#    include "espressif/esp_lp_mailbox.h"
#  endif
#endif

#include "esp32p4-function-ev-board.h"
#include <arch/board/board.h>

#ifdef CONFIG_NETDB_DNSCLIENT
#  include <nuttx/net/dns.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: esp_bringup
 *
 * Description:
 *   Perform architecture-specific initialization.
 *
 * Input Parameters:
 *   None.
 *
 * Returned Value:
 *   Zero (OK) is returned on success; A negated errno value is returned on
 *   any failure.
 *
 ****************************************************************************/

int esp_bringup(void)
{
  int ret = OK;

#ifdef CONFIG_FS_PROCFS
  /* Mount the procfs file system */

  ret = nx_mount(NULL, "/proc", "procfs", 0, NULL);
  if (ret < 0)
    {
      _err("Failed to mount procfs at /proc: %d\n", ret);
    }
#endif

#ifdef CONFIG_FS_TMPFS
  /* Mount the tmpfs file system */

  ret = nx_mount(NULL, CONFIG_LIBC_TMPDIR, "tmpfs", 0, NULL);
  if (ret < 0)
    {
      _err("Failed to mount tmpfs at %s: %d\n", CONFIG_LIBC_TMPDIR, ret);
    }
#endif

#if defined(CONFIG_ESPRESSIF_EFUSE)
  ret = esp_efuse_initialize("/dev/efuse");
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to init EFUSE: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_MWDT0
  ret = esp_wdt_initialize("/dev/watchdog0", ESP_WDT_MWDT0);
  if (ret < 0)
    {
      _err("Failed to initialize WDT: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_MWDT1
  ret = esp_wdt_initialize("/dev/watchdog1", ESP_WDT_MWDT1);
  if (ret < 0)
    {
      _err("Failed to initialize WDT: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_RWDT
  ret = esp_wdt_initialize("/dev/watchdog2", ESP_WDT_RWDT);
  if (ret < 0)
    {
      _err("Failed to initialize WDT: %d\n", ret);
    }
#endif

#ifdef CONFIG_TIMER
  ret = esp_timer_initialize(0);
  if (ret < 0)
    {
      _err("Failed to initialize Timer 0: %d\n", ret);
    }

#ifndef CONFIG_ONESHOT
  ret = esp_timer_initialize(1);
  if (ret < 0)
    {
      _err("Failed to initialize Timer 1: %d\n", ret);
    }
#endif
#endif

#ifdef CONFIG_ONESHOT
  ret = esp_oneshot_initialize();
  if (ret < 0)
    {
      _err("Failed to initialize Oneshot Timer: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESP_RMT
  ret = board_rmt_txinitialize(RMT_OUTPUT_PIN);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: board_rmt_txinitialize() failed: %d\n", ret);
    }

  ret = board_rmt_rxinitialize(RMT_INPUT_PIN);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: board_rmt_txinitialize() failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_RTC_DRIVER
  /* Initialize the RTC driver */

  ret = esp_rtc_driverinit();
  if (ret < 0)
    {
      _err("Failed to initialize the RTC driver: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_SPI
#  if defined(CONFIG_ESPRESSIF_SPI2_SLAVE) && defined(CONFIG_ESPRESSIF_SPI2)
  ret = board_spislavedev_initialize(ESPRESSIF_SPI2);
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize SPI%d Slave driver: %d\n",
             ESPRESSIF_SPI2, ret);
    }
#  elif defined(CONFIG_ESPRESSIF_SPI2)
  ret = board_spidev_initialize(ESPRESSIF_SPI2);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to init spidev 2: %d\n", ret);
    }
#  endif

#  if defined(CONFIG_ESPRESSIF_SPI3_SLAVE) && defined(CONFIG_ESPRESSIF_SPI3)
  ret = board_spislavedev_initialize(ESPRESSIF_SPI3);
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize SPI%d Slave driver: %d\n",
             ESPRESSIF_SPI3, ret);
    }
#  elif defined(CONFIG_ESPRESSIF_SPI3)
  ret = board_spidev_initialize(ESPRESSIF_SPI3);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to init spidev 3: %d\n", ret);
    }
#  endif

#  ifdef CONFIG_ESPRESSIF_SPI_BITBANG
  ret = board_spidev_initialize(ESPRESSIF_SPI_BITBANG);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to init spidev 3: %d\n", ret);
    }
#  endif /* CONFIG_ESPRESSIF_SPI_BITBANG */

#  ifdef CONFIG_ESPRESSIF_LPSPI0
  ret = board_spidev_initialize(ESPRESSIF_LPSPI0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: Failed to init lpspi: %d\n", ret);
    }
#  endif
#endif /* CONFIG_ESPRESSIF_SPI */

#ifdef CONFIG_ESPRESSIF_SPIFLASH
  ret = board_spiflash_init();
  if (ret)
    {
      syslog(LOG_ERR, "ERROR: Failed to initialize SPI Flash\n");
    }
#endif

#if defined(CONFIG_ESPRESSIF_I2S)
  /* Configure I2S peripheral interfaces */

  ret = board_i2s_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize I2S driver: %d\n", ret);
    }
#endif

#if defined(CONFIG_I2C_DRIVER)
  /* Configure I2C peripheral interfaces */

  ret = board_i2c_init();

  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize I2C driver: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_LCD_POWER
  /* MIPI PHY LDO (ch3 @ 2500 mV) + panel reset + backlight off.
   * Must run before the DSI host is initialized, because that starts the
   * DSI PHY PLL which is powered from VDD_MIPI_DPHY.
   */

  ret = board_lcd_power_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: failed to init LCD power path: %d\n", ret);
      return ret;
    }
#endif

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_CAMERA
  /* MIPI-CSI camera.
   *
   * MUST run BEFORE the MIPI-DSI/LCD bring-up below.  Both drivers use the
   * same DW-GDMA controller, and the CSI camera brings it up through
   * dw_gdma_acquire_group_handle(), which asserts a *global* controller reset
   * -- note that dw_gdma_ll_reset_register() ignores its group id:
   *
   *   static inline void _dw_gdma_ll_reset_register(int group_id)
   *   {
   *       (void)group_id;                       <- group id ignored
   *       HP_SYS_CLKRST.hp_rst_en0.reg_rst_en_gdma = 1;
   *       HP_SYS_CLKRST.hp_rst_en0.reg_rst_en_gdma = 0;
   *   }
   *
   * Running that AFTER the DSI has programmed its channel wipes the DSI's
   * channel-0 configuration, so the framebuffer transfer -- a one-shot list
   * (LLP terminates) that is re-armed only from esp_mipi_dsi_dma_isr() --
   * never gets re-armed and the panel freezes on its very first frame.
   *
   * Observed on HW before this reorder: DSI LLI correct (sar = fb,
   * dar = DSI bridge) but chen0 == 0, the channel registers back to zero, and
   * a breakpoint on esp_mipi_dsi_dma_isr never hit.
   *
   * esp_i2cbus_initialize() is reference counted, so this is safe whether or
   * not the touchscreen brought I2C0 up first.
   */

  ret = esp32p4_camera_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: failed to initialize the camera: %d\n", ret);
    }
#endif

#if defined(CONFIG_ESP32P4_FUNCTION_EV_BOARD_MIPI_DSI) && \
    !ESP32P4_CAMERA_WITHOUT_DSI_TEST
  /* 2 data lanes at the EK79007 reference bit rate. */

  struct esp_mipi_dsi_bus_config_s bus_cfg =
    {
      .num_data_lanes = BOARD_MIPI_DSI_LANES,
      .lane_bit_rate_mbps = BOARD_MIPI_DSI_LANE_BITRATE_MBPS,
    };

  ret = esp_mipi_dsi_initialize(&bus_cfg);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: failed to init MIPI-DSI host: %d\n", ret);
      return ret;
    }

  syslog(LOG_INFO, "MIPI-DSI host registered (%d lanes @ %d Mbps)\n",
         BOARD_MIPI_DSI_LANES, BOARD_MIPI_DSI_LANE_BITRATE_MBPS);
#endif

#if defined(CONFIG_ESP32P4_FUNCTION_EV_BOARD_LCD) && \
    !ESP32P4_CAMERA_WITHOUT_DSI_TEST
  /* EK79007 DCS init + DPI + DW-GDMA framebuffer -> /dev/fb0 */

  ret = fb_register(0, 0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: failed to register /dev/fb0: %d\n", ret);
    }
  else
    {
      /* fb_register() clears the plane; restore the test fill for DMA. */

      ret = board_lcd_reload_test_pattern();
      if (ret < 0)
        {
          syslog(LOG_ERR, "ERROR: failed to reload FB test pattern: %d\n",
                 ret);
        }

      syslog(LOG_INFO, "/dev/fb0 registered (%s)\n", BOARD_LCD_PANEL_NAME);
    }
#endif

#ifdef CONFIG_ESP32P4_FUNCTION_EV_BOARD_TOUCHSCREEN
  /* GT911 on the shared I2C0 bus.  Registered after the display so that the
   * panel is already up when the first touch sample arrives.
   */

  ret = board_touchscreen_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: failed to initialize touchscreen: %d\n", ret);
    }
#endif

#ifdef CONFIG_SENSORS_BMP180
  /* Try to register BMP180 device in I2C0 */

  ret = board_bmp180_initialize(0);

  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize BMP180 "
             "Driver for I2C0: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESP_SDM
  struct esp_sdm_chan_config_s config =
  {
    .gpio_num = 5,
    .sample_rate_hz = 1000 * 1000,
    .flags = 0,
  };

  struct dac_dev_s *dev = esp_sdminitialize(config);
  ret = dac_register("/dev/dac0", dev);
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize DAC driver: %d\n",
             ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_TEMP
  struct esp_temp_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG(10, 50);
  ret = esp_temperature_sensor_initialize(cfg);
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to initialize temperature sensor driver: %d\n",
             ret);
    }
#endif
#ifdef CONFIG_ESPRESSIF_TWAI0

  /* Initialize TWAI and register the TWAI driver. */

  ret = board_twai_setup(0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: TWAI0 board_twai_setup failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_TWAI1

  /* Initialize TWAI and register the TWAI driver. */

  ret = board_twai_setup(1);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: TWAI1 board_twai_setup failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_TWAI2

  /* Initialize TWAI and register the TWAI driver. */

  ret = board_twai_setup(2);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: TWAI2 board_twai_setup failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_DEV_GPIO
  ret = esp_gpio_init();
  if (ret < 0)
    {
      ierr("Failed to initialize GPIO Driver: %d\n", ret);
    }
#endif

#if defined(CONFIG_INPUT_BUTTONS) && defined(CONFIG_INPUT_BUTTONS_LOWER)
  /* Register the BUTTON driver */

  ret = btn_lower_initialize("/dev/buttons");
  if (ret < 0)
    {
      ierr("ERROR: btn_lower_initialize() failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_LEDC
  ret = board_ledc_setup();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: board_ledc_setup() failed: %d\n", ret);
    }
#endif /* CONFIG_ESPRESSIF_LEDC */

#ifdef CONFIG_ESP_MCPWM_CAPTURE
  ret = board_capture_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: board_capture_initialize failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESP_MCPWM_MOTOR
  ret = board_motor_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: board_motor_initialize failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESP_PCNT
  ret = board_pcnt_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: board_pcnt_initialize failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_PM
  /* Configure PM */

  ret = esp_pmconfigure();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: esp_pmconfigure failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_SYSTEM_NXDIAG_ESPRESSIF_CHIP_WO_TOOL
  ret = esp_nxdiag_initialize();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: esp_nxdiag_initialize failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_ADC
  ret = board_adc_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: board_adc_init failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_ANA_COMPR0
  ret = esp_cmprinitialize(ESPRESSIF_COMP0);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: esp_cmprinitialize(%d) failed: %d\n",
             ESPRESSIF_COMP0, ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_ANA_COMPR1
  ret = esp_cmprinitialize(ESPRESSIF_COMP1);
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: esp_cmprinitialize(%d) failed: %d\n",
             ESPRESSIF_COMP1, ret);
    }
#endif

#ifdef CONFIG_ESPRESSIF_EMAC
  ret = board_emac_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: board_emac_init failed: %d\n", ret);
    }
#endif

#ifdef CONFIG_NETDB_DNSCLIENT
  /* 把 DNS 服务器真正塞进解析器。
   *
   * 只写 /tmp/resolv.conf 是不够的：resolv.conf 是"每次解析时读一次"的
   * 后备来源，而 gethostbyname()/getaddrinfo() 走的是解析器内部的
   * nameserver 列表。不调用 dns_add_nameserver() 的话，即使 DNS 客户端
   * 编进来了、文件也写了，仍然是"查不到"。这里先用配置的静态 DNS 兜底，
   * 之后 DHCP 拿到租约时 NuttX 会自动覆盖为租约下发的服务器。
   */

  {
    struct sockaddr_in dns;

    memset(&dns, 0, sizeof(dns));
    dns.sin_family      = AF_INET;
    dns.sin_port        = htons(53);
    dns.sin_addr.s_addr = inet_addr(CONFIG_ESP32P4_FUNCTION_EV_BOARD_DNS_PRIMARY);

    ret = dns_add_nameserver((FAR const struct sockaddr *)&dns, sizeof(dns));
    if (ret < 0)
      {
        syslog(LOG_WARNING, "WARN: dns_add_nameserver(%s) failed: %d\n",
               CONFIG_ESP32P4_FUNCTION_EV_BOARD_DNS_PRIMARY, ret);
      }
    else
      {
        syslog(LOG_INFO, "DNS nameserver set to %s\n",
               CONFIG_ESP32P4_FUNCTION_EV_BOARD_DNS_PRIMARY);
      }
  }
#endif

#ifdef CONFIG_ESPRESSIF_USE_LP_CORE
#  ifdef CONFIG_ESPRESSIF_LP_MAILBOX
  esp_lp_mailbox_init();
#  endif

  /* ULP initialization should be the handled later than
   * peripherals to use supported peripherals properly on ULP core
   */

  ret = esp_ulp_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "ERROR: esp_ulp_init failed: %d\n", ret);
    }
  else
    {
#  ifdef CONFIG_ESPRESSIF_ULP_USE_TEST_BIN
      esp_ulp_load_bin((char *)esp_ulp_bin, esp_ulp_bin_len);
#  endif
    }
#endif

  /* If we got here then perhaps not all initialization was successful, but
   * at least enough succeeded to bring-up NSH with perhaps reduced
   * capabilities.
   */

  return ret;
}
