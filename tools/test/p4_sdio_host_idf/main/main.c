/* ESP-IDF SDIO host probe for the on-board ESP32-C6 -- reference baseline.
 *
 * Goal (acceptance stage 1): show, on this exact board and with ESP-IDF's own
 * sdmmc host stack, that function 1 of the companion answers -- using the SAME
 * sequence esp-hosted's own IDF port performs.  This is the "known good"
 * baseline the NuttX port is then compared against.
 *
 * ---------------------------------------------------------------------------
 * Why the previous revision of this probe proved nothing (appendix BX)
 * ---------------------------------------------------------------------------
 * It called sdmmc_card_init() and then poked function 1 directly.  But IDF's
 * sdmmc_card_init() never programs the SDIO function-enable registers:
 *
 *   - SD_IO_CCCR_FN_ENABLE (0x02), SD_IO_CCCR_FN_READY (0x03) and
 *     SD_IO_CCCR_INT_ENABLE (0x04) are defined in sd_protocol_defs.h and
 *     referenced NOWHERE in components/sdmmc (verified by grep),
 *   - sdmmc_io_reset() additionally writes CCCR 0x06 = RES, which resets the
 *     card's I/O functions.
 *
 * The ESP SDIO slave does not answer function-1 accesses until the host has
 * enabled the function (CCCR 0x02 bit 1) and seen it come ready (CCCR 0x03
 * bit 1).  So "sdmmc_card_init -> OK, then every FN1 access -> 0x107" was the
 * expected outcome of an incomplete host sequence -- not evidence of a
 * hardware fault.  esp-hosted's port has a mandatory card_fn_init() for
 * exactly this reason (port/os/idf/src/eh_host_port_sdio.c:155, called from
 * eh_host_port_sdio_card_init() at :460).
 *
 * This revision therefore performs the complete official sequence:
 *
 *   1. release the companion from reset (GPIO54 = C6_EN on this board),
 *   2. sdmmc_card_init() at 400 kHz,
 *   3. dump the card's CCCR/FBR state before touching anything,
 *   4. the full card_fn_init(): enable FN1, wait for IO_RDY, program
 *      INT_ENABLE (IENM + FN1), set and verify 512-byte FN0/FN1 block sizes,
 *      read CCCR 0x07,
 *   5. raise the card clock the way the official port does, then prove FN1
 *      still answers at that rate (the old revision blamed the clock),
 *   6. write HOST_TO_SLAVE_INTR = BIT(0) = ESP_OPEN_DATA_PATH, exactly as
 *      sdio_generate_slave_intr() does (one byte, CMD52),
 *   7. wait for the card interrupt with sdmmc_io_wait_int() -- the official
 *      eh_host_port_sdio_wait_slave_intr() -- and read INT_RAW, PACKET_LEN and
 *      TOKEN_RDATA,
 *   8. drain the payload from the packet window (ESP_SLAVE_CMD53_END_ADDR -
 *      len) with BOTH transfer shapes the official transport can use
 *      (block-only 512-byte, and exact byte length), and clear the interrupt
 *      through INT_CLR = the new-packet bit.
 *
 * The address is deliberately NOT masked: read_reg()/write_reg() mask to the
 * 10-bit register window, but the payload window is addressed as
 * ESP_SLAVE_CMD53_END_ADDR - len (e.g. 0x1F7C4 for a 60-byte packet).
 */

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "p4sdio";

/* SD2 = the C6's slot on this board (official Kconfig.host.sdio defaults for
 * ESP_HOSTED_P4_DEV_BOARD_FUNC_BOARD + ESP_HOSTED_CP_TARGET_ESP32C6).
 */
#define PIN_CLK 18
#define PIN_CMD 19
#define PIN_D0  14
#define PIN_D1  15
#define PIN_D2  16
#define PIN_D3  17
#define C6_EN   54

/* Slave register window (function 1, relative addresses -- the same values the
 * official transport passes to read_reg/write_reg, which mask them again). */
#define REG_INT_RAW        0x50  /* interrupt raw status, bit23 = new packet */
#define REG_INT_ST         0x58  /* masked interrupt status                  */
#define REG_PACKET_LEN     0x60  /* packet length, low 20 bits               */
#define REG_TOKEN_RDATA    0x44  /* token of the pending packet              */
#define REG_INT_CLR        0xD4  /* write 1s here to clear slave->host ints  */
#define REG_HOST_TO_SLAVE  0x8C  /* event bits host -> slave (SCRATCH_REG_7) */

#define ESP_SLAVE_CMD53_END_ADDR 0x1F800u  /* packet window ends here */
#define ESP_SLAVE_LEN_MASK       0xFFFFFu
#define ESP_BLOCK_SIZE           512u
#define SDIO_INT_NEW_PACKET      23
#define ESP_OPEN_DATA_PATH       0        /* enum value in the official header */

#define FUNC1_EN_MASK  (1u << 1)
#define IENM_MASK      (1u << 0)

/* CCCR / FBR addresses (function 0 address space). */
#define CCCR_FN_ENABLE   0x02
#define CCCR_FN_READY    0x03
#define CCCR_INT_ENABLE  0x04
#define CCCR_BUS_WIDTH   0x07
#define CCCR_FN0_BLKSIZEL 0x10
#define CCCR_FN0_BLKSIZEH 0x11
#define FBR_START        0x100
#define CCCR_FN1_BLKSIZEL (FBR_START + 0x10)
#define CCCR_FN1_BLKSIZEH (FBR_START + 0x11)

#define SDIO_INIT_MAX_RETRY 10

static uint8_t payload[1024] __attribute__((aligned(64)));
static uint8_t t4[4] __attribute__((aligned(64)));

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Read a 4-byte function-1 register.  Returns the value read, and sets *err. */
static uint32_t read_reg1(sdmmc_card_t *card, uint32_t reg, esp_err_t *err)
{
    memset(t4, 0, sizeof(t4));
    *err = sdmmc_io_read_bytes(card, 1, reg, t4, 4);
    return rd32(t4);
}

/* CCCR byte read (function 0), logged on failure. */
static esp_err_t cccr_read(sdmmc_card_t *card, uint32_t reg, const char *what,
                           uint8_t *out)
{
    esp_err_t err = sdmmc_io_read_byte(card, 0, reg, out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CCCR 0x%02x (%s) read -> %s", (unsigned)reg, what,
                 esp_err_to_name(err));
    }
    return err;
}

/* Set and verify FN block size -- the official eh_host_port_sdio_set_blocksize(). */
static esp_err_t set_blocksize(sdmmc_card_t *card, uint8_t fn, uint16_t value)
{
    uint8_t lo = 0, hi = 0, rb_lo = 0, rb_hi = 0;
    size_t offset = FBR_START * fn;
    esp_err_t err;

    lo = (uint8_t)(value & 0xff);
    hi = (uint8_t)((value >> 8) & 0xff);

    err  = sdmmc_io_write_byte(card, 0, offset + CCCR_FN0_BLKSIZEL, lo, NULL);
    err |= sdmmc_io_write_byte(card, 0, offset + CCCR_FN0_BLKSIZEH, hi, NULL);
    err |= sdmmc_io_read_byte(card, 0, offset + CCCR_FN0_BLKSIZEL, &rb_lo);
    err |= sdmmc_io_read_byte(card, 0, offset + CCCR_FN0_BLKSIZEH, &rb_hi);

    ESP_LOGI(TAG, "FN%u block size: wrote %u, read back %u (err %s)", fn,
             (unsigned)value, (unsigned)(rb_lo | ((uint16_t)rb_hi << 8)),
             esp_err_to_name(err));

    if (err != ESP_OK) {
        return err;
    }
    return ((rb_lo | ((uint16_t)rb_hi << 8)) == value) ? ESP_OK : ESP_FAIL;
}

/* ------------------------------------------------------------------------- */
/* The official card-function bring-up (port/os/idf/src/eh_host_port_sdio.c)  */
/* ------------------------------------------------------------------------- */
static esp_err_t card_fn_init(sdmmc_card_t *card)
{
    uint8_t ioe = 0, ior = 0, ie = 0, bus_width = 0;
    esp_err_t err;
    int i;

    /* 1. Function 1 enable (CCCR 0x02, bit 1). */
    if (cccr_read(card, CCCR_FN_ENABLE, "IOEx", &ioe) != ESP_OK) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "CCCR 0x02 IOEx before: 0x%02x", ioe);

    ioe |= FUNC1_EN_MASK;
    err = sdmmc_io_write_byte(card, 0, CCCR_FN_ENABLE, ioe, &ioe);
    ESP_LOGI(TAG, "CCCR 0x02 IOEx write 0x%02x -> %s (card echoed 0x%02x)",
             ioe, esp_err_to_name(err), ioe);
    if (err != ESP_OK) {
        return err;
    }

    if (cccr_read(card, CCCR_FN_ENABLE, "IOEx rb", &ioe) != ESP_OK) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "CCCR 0x02 IOEx after: 0x%02x", ioe);

    /* 2. Wait for function 1 to report ready (CCCR 0x03 bit 1).
     *    This is the step the previous probe revision never performed, and the
     *    step that decides whether FN1 answers at all. */
    for (i = 0; i < SDIO_INIT_MAX_RETRY; i++) {
        if (cccr_read(card, CCCR_FN_READY, "IO_RDY", &ior) != ESP_OK) {
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "CCCR 0x03 IO_RDY try %d: 0x%02x", i, ior);
        if (ior & FUNC1_EN_MASK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (i >= SDIO_INIT_MAX_RETRY) {
        ESP_LOGE(TAG, "function 1 never reported ready -- slave side is not "
                      "serving FN1; nothing else can be tested");
        return ESP_FAIL;
    }

    /* 3. Interrupt enable: IENM (master, bit 0) + function 1 (bit 1).
     *    Without the master bit the card is not allowed to drive DAT1. */
    if (cccr_read(card, CCCR_INT_ENABLE, "INT_ENA", &ie) != ESP_OK) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "CCCR 0x04 INT_ENABLE before: 0x%02x", ie);
    ie |= IENM_MASK | FUNC1_EN_MASK;
    err = sdmmc_io_write_byte(card, 0, CCCR_INT_ENABLE, ie, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CCCR 0x04 write -> %s", esp_err_to_name(err));
        return err;
    }
    if (cccr_read(card, CCCR_INT_ENABLE, "INT_ENA rb", &ie) != ESP_OK) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "CCCR 0x04 INT_ENABLE after: 0x%02x (want 0x03)", ie);

    /* 4. 512-byte blocks on both functions (FN0 at 0x10, FN1 at 0x110). */
    if (set_blocksize(card, 0, ESP_BLOCK_SIZE) != ESP_OK) {
        return ESP_FAIL;
    }
    if (set_blocksize(card, 1, ESP_BLOCK_SIZE) != ESP_OK) {
        return ESP_FAIL;
    }

    /* 5. Bus width actually negotiated. */
    if (cccr_read(card, CCCR_BUS_WIDTH, "BUS_WIDTH", &bus_width) == ESP_OK) {
        ESP_LOGI(TAG, "CCCR 0x07 BUS_WIDTH: 0x%02x (bit1 = 4-bit)", bus_width);
    }

    return ESP_OK;
}

/* Payload read: exactly what the official transport does.
 *
 * eh_host_bus_sdio.c calls read_block(ESP_SLAVE_CMD53_END_ADDR - data_left,
 * pos, len) and the port either rounds up to a 512-byte block
 * (EH_HOST_PORT_SDIO_RX_BLOCK_ONLY_XFER = 1, "slave pads with 0") or uses the
 * exact length.  Both shapes are exercised here so the slave's tolerance for
 * each is a measured fact rather than an assumption.
 */
static void read_payload(sdmmc_card_t *card, uint32_t len, bool block_only)
{
    esp_err_t err;
    uint32_t addr = ESP_SLAVE_CMD53_END_ADDR - len;

    memset(payload, 0xcc, sizeof(payload));

    if (block_only) {
        uint32_t bytes = ((len + ESP_BLOCK_SIZE - 1) / ESP_BLOCK_SIZE) * ESP_BLOCK_SIZE;
        err = sdmmc_io_read_blocks(card, 1, addr, payload, bytes);
        ESP_LOGI(TAG, "    payload: block mode %u B @0x%05x (masked would be "
                      "0x%03x) -> %s", (unsigned)bytes, (unsigned)addr,
                 (unsigned)(addr & 0x3FF), esp_err_to_name(err));
    } else {
        uint32_t bytes = (len + 3u) & ~3u;   /* official RX_LEN_TO_TRANSFER */
        err = sdmmc_io_read_bytes(card, 1, addr, payload, bytes);
        ESP_LOGI(TAG, "    payload: byte mode %u B @0x%05x (masked would be "
                      "0x%03x) -> %s", (unsigned)bytes, (unsigned)addr,
                 (unsigned)(addr & 0x3FF), esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "    payload: %02x %02x %02x %02x %02x %02x %02x %02x "
                  "%02x %02x %02x %02x %02x %02x %02x %02x",
             payload[0], payload[1], payload[2], payload[3],
             payload[4], payload[5], payload[6], payload[7],
             payload[8], payload[9], payload[10], payload[11],
             payload[12], payload[13], payload[14], payload[15]);

    if (payload[0] != 0xcc) {
        ESP_LOGI(TAG, "    *** DATA ARRIVED ***");
    }
}

void app_main(void)
{
    esp_err_t err;
    uint32_t intr = 0, plen = 0, token = 0;
    uint8_t  one = (uint8_t)(1u << ESP_OPEN_DATA_PATH);   /* BIT(0) */

    /* 1. Release the companion from reset first (GPIO54 = C6_EN). */
    gpio_config_t en = { .pin_bit_mask = 1ULL << C6_EN, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&en);
    gpio_set_level(C6_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(C6_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGI(TAG, "companion released (GPIO%d), settling done", C6_EN);

    /* 2. Host + slot: SD2, 4-bit, the board's pins. */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    host.max_freq_khz = 400;          /* enumerate at the identification clock */
    host.flags |= SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = PIN_CLK; slot.cmd = PIN_CMD;
    slot.d0 = PIN_D0; slot.d1 = PIN_D1;
    slot.d2 = PIN_D2; slot.d3 = PIN_D3;
    slot.width = 4;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "host init ...");
    ESP_ERROR_CHECK(sdmmc_host_init());
    ESP_ERROR_CHECK(sdmmc_host_init_slot(host.slot, &slot));

    static sdmmc_card_t card_s;
    sdmmc_card_t *card = &card_s;
    memset(card, 0, sizeof(card_s));

    err = sdmmc_card_init(&host, card);
    ESP_LOGI(TAG, "sdmmc_card_init -> %s (%d)", esp_err_to_name(err), err);
    if (err != ESP_OK) {
        return;
    }
    ESP_LOGI(TAG, "card: name=%.8s real_freq=%d kHz", card->cid.name,
             card->real_freq_khz);

    /* 3. What the card looked like on arrival (no writes yet). */
    {
        uint8_t rev = 0, ioe = 0, ior = 0, ie = 0, bw = 0;
        esp_err_t e1 = cccr_read(card, 0x00, "CCCR_REV", &rev);
        esp_err_t e2 = cccr_read(card, CCCR_FN_ENABLE, "IOEx", &ioe);
        esp_err_t e3 = cccr_read(card, CCCR_FN_READY, "IO_RDY", &ior);
        esp_err_t e4 = cccr_read(card, CCCR_INT_ENABLE, "INT_ENA", &ie);
        esp_err_t e5 = cccr_read(card, CCCR_BUS_WIDTH, "BUS_WIDTH", &bw);
        ESP_LOGI(TAG, "before fn_init: rev=0x%02x(r%d) IOEx=0x%02x(r%d) "
                      "IO_RDY=0x%02x(r%d) INT_ENA=0x%02x(r%d) BUS_WIDTH=0x%02x(r%d)",
                 rev, e1, ioe, e2, ior, e3, ie, e4, bw, e5);
    }

    /* 4. The official card-function bring-up. */
    err = card_fn_init(card);
    ESP_LOGI(TAG, "card_fn_init -> %s", esp_err_to_name(err));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stopping: without FN1 the rest of the sequence is "
                      "meaningless");
        return;
    }

    /* 5. Raise the card clock the way the official port does, then prove that
     *    FN1 still answers at that rate. */
    {
        esp_err_t clkerr = sdmmc_host_set_card_clk(host.slot, 40000);
        ESP_LOGI(TAG, "set_card_clk(40000 kHz) -> %s", esp_err_to_name(clkerr));
        vTaskDelay(pdMS_TO_TICKS(50));

        esp_err_t e;
        uint32_t v = read_reg1(card, REG_INT_RAW, &e);
        ESP_LOGI(TAG, "FN1 INT_RAW at 40 MHz: 0x%08x (%s) -- FN1 %s",
                 (unsigned)v, esp_err_to_name(e),
                 (e == ESP_OK) ? "answers at 40 MHz" : "does NOT answer at 40 MHz");
    }

    /* 5b. Does the companion serve the transport's twenty-byte window read?
     *
     * The official transport reads INT_RAW..PACKET_LEN -- 0x50..0x63, twenty
     * bytes (REG_BUF_LEN in eh_host_bus_sdio.c) -- in ONE CMD53 and takes the
     * packet length out of the second half.  The NuttX driver returns success
     * for that read but was measured to deliver only its first four bytes, and
     * the two possible explanations need different fixes: either the driver
     * drops the rest, or the slave only ever sends four bytes for a
     * register-window byte-mode read and the read has to be split.  Filling the
     * buffer with 0x5a first makes the delivered length visible here, on the
     * host stack that is known to move data.
     */

    {
        static uint8_t win[20] __attribute__((aligned(64)));
        memset(win, 0x5a, sizeof(win));

        esp_err_t e = sdmmc_io_read_bytes(card, 1, REG_INT_RAW, win, sizeof(win));
        ESP_LOGI(TAG, "window 0x50 x20 -> %s bytes="
                      "%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x "
                      "%02x%02x%02x%02x %02x%02x%02x%02x",
                 esp_err_to_name(e),
                 win[0], win[1], win[2], win[3], win[4], win[5], win[6], win[7],
                 win[8], win[9], win[10], win[11], win[12], win[13], win[14],
                 win[15], win[16], win[17], win[18], win[19]);

        /* And the same window as five four-byte reads, to compare. */
        for (int k = 0; k < 5; k++) {
            uint8_t q[4] = { 0x5a, 0x5a, 0x5a, 0x5a };
            esp_err_t e2 = sdmmc_io_read_bytes(card, 1, REG_INT_RAW + k * 4, q, 4);
            ESP_LOGI(TAG, "window 0x%02x x4 -> %s bytes=%02x%02x%02x%02x",
                     (unsigned)(REG_INT_RAW + k * 4), esp_err_to_name(e2),
                     q[0], q[1], q[2], q[3]);
        }
    }

    /* 6. Card interrupt on the host side, then OPEN_DATA_PATH. */
    err = sdmmc_io_enable_int(card);
    ESP_LOGI(TAG, "sdmmc_io_enable_int -> %s", esp_err_to_name(err));

    err = sdmmc_io_write_byte(card, 1, REG_HOST_TO_SLAVE, one, NULL);
    ESP_LOGI(TAG, "HOST_TO_SLAVE_INTR <- 0x%02x (ESP_OPEN_DATA_PATH) -> %s",
             one, esp_err_to_name(err));

    /* 7. Follow the transport: wait for the card interrupt, read the register
     *    window, drain the payload, clear the interrupt. */
    for (int i = 0; i < 20; i++) {
        TickType_t t0 = xTaskGetTickCount();
        esp_err_t werr = sdmmc_io_wait_int(card, pdMS_TO_TICKS(2000));
        uint32_t waited = (uint32_t)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS);

        esp_err_t e1, e2, e3;
        intr  = read_reg1(card, REG_INT_RAW, &e1);
        plen  = read_reg1(card, REG_PACKET_LEN, &e2);
        token = read_reg1(card, REG_TOKEN_RDATA, &e3);

        ESP_LOGI(TAG, "[%d] wait_int=%s after %u ms | INT_RAW=0x%08x(%s) "
                      "new_packet=%u | LEN=0x%08x(%s) len=%u | TOKEN=0x%08x(%s)",
                 i, esp_err_to_name(werr), (unsigned)waited,
                 (unsigned)intr, esp_err_to_name(e1),
                 (unsigned)((intr >> SDIO_INT_NEW_PACKET) & 1u),
                 (unsigned)plen, esp_err_to_name(e2),
                 (unsigned)(plen & ESP_SLAVE_LEN_MASK),
                 (unsigned)token, esp_err_to_name(e3));

        {
            esp_err_t e4;
            uint32_t st = read_reg1(card, REG_INT_ST, &e4);
            ESP_LOGI(TAG, "    INT_ST=0x%08x (%s)", (unsigned)st,
                     esp_err_to_name(e4));
        }

        if ((plen & ESP_SLAVE_LEN_MASK) != 0) {
            uint32_t len = plen & ESP_SLAVE_LEN_MASK;
            read_payload(card, len, (i & 1) == 0);

            /* Clear the new-packet interrupt, exactly like the transport. */
            {
                uint8_t clr[4];
                uint32_t bit = 1u << SDIO_INT_NEW_PACKET;
                clr[0] = (uint8_t)(bit & 0xff);
                clr[1] = (uint8_t)((bit >> 8) & 0xff);
                clr[2] = (uint8_t)((bit >> 16) & 0xff);
                clr[3] = (uint8_t)((bit >> 24) & 0xff);
                esp_err_t e5 = sdmmc_io_write_bytes(card, 1, REG_INT_CLR, clr, 4);
                ESP_LOGI(TAG, "    INT_CLR <- 0x%08x (%s)", (unsigned)bit,
                         esp_err_to_name(e5));
            }

            {
                esp_err_t e6;
                uint32_t after = read_reg1(card, REG_PACKET_LEN, &e6);
                ESP_LOGI(TAG, "    LEN after drain: 0x%08x len=%u (%s)",
                         (unsigned)after, (unsigned)(after & ESP_SLAVE_LEN_MASK),
                         esp_err_to_name(e6));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(300));
    }

    ESP_LOGI(TAG, "probe done");
}
