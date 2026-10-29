/* Minimal SDIO slave test: does this C6's SDIO slave drive data at all?
 *
 * No esp-hosted, no WiFi, no protocol. It configures the slave on the same pins
 * the board uses and queues one known 64-byte pattern, then keeps re-queueing it
 * whenever the previous one is consumed. If a host cannot read this, the fault
 * is in the SDIO slave hardware or the board wiring, not in esp-hosted.
 */

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/sdio_slave.h"
#include "esp_log.h"

static const char *TAG = "c6sdio";

/* Same pins the board uses for the C6 side. */
#define PIN_CLK 19
#define PIN_CMD 18
#define PIN_D0  20
#define PIN_D1  21
#define PIN_D2  22
#define PIN_D3  23

static uint8_t pattern[64] __attribute__((aligned(4)));

void app_main(void)
{
    sdio_slave_config_t cfg = {
        .sending_mode     = SDIO_SLAVE_SEND_PACKET,
        .send_queue_size  = 4,
        .recv_buffer_size = 512,
        .event_cb         = NULL,
        .flags            = SDIO_SLAVE_FLAG_DEFAULT_SPEED,
        .timing           = SDIO_SLAVE_TIMING_PSEND_PSAMPLE,
    };

    for (int i = 0; i < (int)sizeof(pattern); i++)
      {
        pattern[i] = (uint8_t)(0xC6 ^ i);
      }

    ESP_LOGI(TAG, "sdio_slave_initialize ...");
    ESP_ERROR_CHECK(sdio_slave_initialize(&cfg));
    ESP_ERROR_CHECK(sdio_slave_start());
    ESP_LOGI(TAG, "sdio_slave_start done, pins CLK=%d CMD=%d D0..D3=%d,%d,%d,%d",
             PIN_CLK, PIN_CMD, PIN_D0, PIN_D1, PIN_D2, PIN_D3);

    for (;;)
      {
        esp_err_t err = sdio_slave_transmit(pattern, sizeof(pattern));
        ESP_LOGI(TAG, "transmit -> %d (host %s reading)",
                 err, err == ESP_OK ? "IS" : "is NOT");
      }
}
