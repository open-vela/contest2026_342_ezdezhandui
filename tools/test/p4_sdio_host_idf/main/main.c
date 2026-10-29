/* ESP-IDF SDIO host probe for the on-board ESP32-C6.
 *
 * Same hardware, same companion, but IDF's own sdmmc host stack instead of the
 * NuttX driver. If this reads data, the NuttX side is at fault; if it also
 * times out on the data phase, the companion's slave hardware is.
 */

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"

static const char *TAG = "p4sdio";

/* SD2 = the C6's slot on this board. */
#define PIN_CLK 18
#define PIN_CMD 19
#define PIN_D0  14
#define PIN_D1  15
#define PIN_D2  16
#define PIN_D3  17
#define C6_EN   54

void app_main(void)
{
    static uint8_t buf[512] __attribute__((aligned(64)));
    static uint8_t t4[4] __attribute__((aligned(64)));
    uint32_t token;
    esp_err_t err;

    /* Release the companion from reset first. */
    gpio_config_t en = { .pin_bit_mask = 1ULL << C6_EN, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&en);
    gpio_set_level(C6_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(C6_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(1500));

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    host.max_freq_khz = 400;
    host.flags |= SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = PIN_CLK; slot.cmd = PIN_CMD;
    slot.d0 = PIN_D0; slot.d1 = PIN_D1;
    slot.d2 = PIN_D2; slot.d3 = PIN_D3;
    slot.width = 1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "host init ...");
    ESP_ERROR_CHECK(sdmmc_host_init());
    ESP_ERROR_CHECK(sdmmc_host_init_slot(host.slot, &slot));

    static sdmmc_card_t card_s;
    sdmmc_card_t *card = &card_s;
    memset(card, 0, sizeof(card_s));
    err = sdmmc_card_init(&host, card);
    ESP_LOGI(TAG, "sdmmc_card_init -> %s (%d)", esp_err_to_name(err), err);
    if (err != ESP_OK) return;

    ESP_LOGI(TAG, "card: name=%.8s", card->cid.name);

    /* Force the identification clock back after card init.
     *
     * sdmmc_card_init leaves the host at the card's advertised speed; at that
     * rate this companion stops answering registers altogether.
     */

    esp_err_t clkerr = sdmmc_host_set_card_clk(host.slot, 400);
    ESP_LOGI(TAG, "set_card_clk(400 kHz) -> %s", esp_err_to_name(clkerr));

    /* Open the data path, the same handshake the NuttX driver performs. */
    static uint8_t one[4] __attribute__((aligned(64))) = { 0, 0, 0, 0 };
    err = sdmmc_io_write_bytes(card, 1, 0x8C, one, 4);
    ESP_LOGI(TAG, "OPEN_DATA_PATH -> %s", esp_err_to_name(err));

    for (int i = 0; i < 20; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));

        uint32_t plen = 0;
        err = sdmmc_io_read_bytes(card, 1, 0x60, t4, 4);
        if (err != ESP_OK) ESP_LOGI(TAG, "    LEN read err %s", esp_err_to_name(err));
        plen = (uint32_t)t4[0] | ((uint32_t)t4[1] << 8) |
               ((uint32_t)t4[2] << 16) | ((uint32_t)t4[3] << 24);

        err = sdmmc_io_read_bytes(card, 1, 0x44, t4, 4);
        if (err != ESP_OK) ESP_LOGI(TAG, "    TOKEN read err %s", esp_err_to_name(err));
        token = (uint32_t)t4[0] | ((uint32_t)t4[1] << 8) |
                ((uint32_t)t4[2] << 16) | ((uint32_t)t4[3] << 24);

        ESP_LOGI(TAG, "[%d] PACKET_LEN=%08x TOKEN=%08x", i, plen, token);

        if ((plen & 0xFFFFF) == 0) continue;

        memset(buf, 0xcc, sizeof(buf));
        err = sdmmc_io_read_bytes(card, 1, 0x1F800 - 60, buf, 60);
        ESP_LOGI(TAG, "    read60 -> %s", esp_err_to_name(err));
        ESP_LOGI(TAG, "    buf: %02x %02x %02x %02x %02x %02x %02x %02x",
                 buf[0], buf[1], buf[2], buf[3],
                 buf[4], buf[5], buf[6], buf[7]);
        if (buf[0] != 0xcc) {
            ESP_LOGI(TAG, "    *** DATA ARRIVED ***");
        }
        break;
    }

    ESP_LOGI(TAG, "probe done");
}
