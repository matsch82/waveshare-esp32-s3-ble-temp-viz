#include "epd_ssd1680.h"
#include "board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define TAG "epd"

/*
 * GDEY0154D67 1.54" 200x200 B/W panel (Waveshare ESP32-S3-ePaper-1.54 V2).
 *
 * The waveform lookup tables and register sequence below are derived from the
 * tuanpmt/esp_epaper component (MIT license), which is specifically written for
 * this board/panel combination. The older GDEH0154D67 / SSD16XX generic flow
 * (0x22 0xF7 without external LUT) does not drive the GDEY panel visibly.
 */

static const uint8_t LUT_FULL[159] = {
    0x80, 0x48, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x48, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x48, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x48, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x08, 0x01, 0x00, 0x08, 0x01, 0x00, 0x02,
    0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x00,
    0x22, 0x17, 0x41, 0x00, 0x32, 0x20
};

static const uint8_t LUT_PARTIAL[159] = {
    0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x00,
    0x02, 0x17, 0x41, 0xB0, 0x32, 0x28
};

/* CS is driven manually so it can stay asserted for a whole frame transfer. */
static void spi_txn(const uint8_t *data, size_t len)
{
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = data,
    };
    spi_device_polling_transmit((spi_device_handle_t)board_epd_spi_handle, &t);
}

static void spi_cmd(uint8_t cmd)
{
    gpio_set_level(BOARD_EPD_DC_GPIO, 0);
    gpio_set_level(BOARD_EPD_CS_GPIO, 0);
    spi_txn(&cmd, 1);
    gpio_set_level(BOARD_EPD_CS_GPIO, 1);
}

static void spi_data(const uint8_t *data, size_t len)
{
    gpio_set_level(BOARD_EPD_DC_GPIO, 1);
    gpio_set_level(BOARD_EPD_CS_GPIO, 0);
    spi_txn(data, len);
    gpio_set_level(BOARD_EPD_CS_GPIO, 1);
}

static void spi_data_byte(uint8_t b)
{
    spi_data(&b, 1);
}

static void epd_wait_idle(void)
{
    while (board_epd_busy_level() == 1) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static void reset_controller(void)
{
    gpio_set_level(BOARD_EPD_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(BOARD_EPD_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(BOARD_EPD_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static void load_lut(const uint8_t *lut)
{
    spi_cmd(0x32);
    spi_data(lut, 153);
    epd_wait_idle();

    spi_cmd(0x3F);
    spi_data_byte(lut[153]);

    spi_cmd(0x03);
    spi_data_byte(lut[154]);

    spi_cmd(0x04);
    spi_data(&lut[155], 3);

    spi_cmd(0x2C);
    spi_data_byte(lut[158]);
}

/* RAM window: X in bytes, Y counts down from the last row. */
static void set_windows(uint16_t x_start, uint16_t y_start, uint16_t x_end, uint16_t y_end)
{
    spi_cmd(0x44);
    spi_data_byte((x_start >> 3) & 0xFF);
    spi_data_byte((x_end >> 3) & 0xFF);

    spi_cmd(0x45);
    spi_data_byte(y_start & 0xFF);
    spi_data_byte((y_start >> 8) & 0xFF);
    spi_data_byte(y_end & 0xFF);
    spi_data_byte((y_end >> 8) & 0xFF);
}

static void set_cursor(uint16_t x_start, uint16_t y_start)
{
    spi_cmd(0x4E);
    spi_data_byte(x_start & 0xFF);

    spi_cmd(0x4F);
    spi_data_byte(y_start & 0xFF);
    spi_data_byte((y_start >> 8) & 0xFF);
}

static void turn_on_display_full(void)
{
    spi_cmd(0x22);
    spi_data_byte(0xC7);
    spi_cmd(0x20);
    epd_wait_idle();
}

static void turn_on_display_partial(void)
{
    spi_cmd(0x22);
    spi_data_byte(0xCF);
    spi_cmd(0x20);
    epd_wait_idle();
}

/* Full-refresh initialization (Waveshare V2 EPD_Init). */
static void epd_init_full(void)
{
    reset_controller();

    epd_wait_idle();
    spi_cmd(0x12); /* Software reset */
    epd_wait_idle();

    spi_cmd(0x01); /* Driver output control */
    spi_data_byte(0xC7);
    spi_data_byte(0x00);
    spi_data_byte(0x01);

    spi_cmd(0x11); /* Data entry mode: Y decrement, X increment */
    spi_data_byte(0x01);

    set_windows(0, EPD_WIDTH - 1, EPD_HEIGHT - 1, 0);

    spi_cmd(0x3C); /* Border waveform */
    spi_data_byte(0x01);

    spi_cmd(0x18); /* Use internal temperature sensor */
    spi_data_byte(0x80);

    spi_cmd(0x22); /* Load temperature and waveform setting */
    spi_data_byte(0xB1);
    spi_cmd(0x20);

    set_cursor(0, EPD_HEIGHT - 1);
    epd_wait_idle();

    load_lut(LUT_FULL);
}

/* Partial-refresh initialization (Waveshare V2 EPD_Init_Partial). */
static void epd_init_partial(void)
{
    reset_controller();
    epd_wait_idle();

    set_windows(0, EPD_WIDTH - 1, EPD_HEIGHT - 1, 0);
    set_cursor(0, EPD_HEIGHT - 1);

    load_lut(LUT_PARTIAL);

    spi_cmd(0x37);
    spi_data((const uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00,
                               0x40, 0x00, 0x00, 0x00, 0x00}, 10);

    spi_cmd(0x3C); /* Border waveform for partial updates */
    spi_data_byte(0x80);

    spi_cmd(0x22);
    spi_data_byte(0xC0);
    spi_cmd(0x20);
    epd_wait_idle();
}

bool epd_init(void)
{
    board_epd_power_on();
    vTaskDelay(pdMS_TO_TICKS(20));
    epd_init_full();
    return true;
}

void epd_clear_white_full(void)
{
    static uint8_t white[EPD_BUF_SIZE];
    memset(white, 0xFF, sizeof(white));

    /* Write white to current RAM (0x24) and base RAM (0x26), then refresh. */
    spi_cmd(0x24);
    spi_data(white, sizeof(white));
    spi_cmd(0x26);
    spi_data(white, sizeof(white));
    turn_on_display_full();

    /* Switch the controller into partial-refresh mode for subsequent updates. */
    epd_init_partial();
}

void epd_refresh_fb(const uint8_t *fb, const uint8_t *prev_fb, bool partial)
{
    epd_wait_idle();

    if (partial) {
        /* The controller was powered off during sleep, so we must restore the
           previous/base image into RAM 0x26 before the differential update. */
        epd_init_partial();
        spi_cmd(0x26);
        spi_data(prev_fb, EPD_BUF_SIZE);
        spi_cmd(0x24);
        spi_data(fb, EPD_BUF_SIZE);
        turn_on_display_partial();
        return;
    }

    /* Full refresh: re-init with the full LUT, write both RAM buffers so the
       base image matches, refresh, then go back to partial-refresh mode. */
    epd_init_full();
    spi_cmd(0x24);
    spi_data(fb, EPD_BUF_SIZE);
    spi_cmd(0x26);
    spi_data(fb, EPD_BUF_SIZE);
    turn_on_display_full();
    epd_init_partial();
}

void epd_power_off(void)
{
    epd_wait_idle();
    spi_cmd(0x10); /* Deep sleep mode */
    spi_data_byte(0x01);
    vTaskDelay(pdMS_TO_TICKS(100));
    board_epd_power_off();
}
