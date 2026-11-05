#include "epd_ssd1680.h"
#include "board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "epd"

/*
 * GDEY0154D67 1.54" 200x200 B/W panel with SSD1681 controller (Waveshare
 * ESP32-S3-ePaper-1.54 V2).
 *
 * The waveform lookup tables and register sequence below are derived from the
 * tuanpmt/esp_epaper component (MIT license), which is specifically written for
 * this board/panel combination. The older GDEH0154D67 / SSD16XX generic flow
 * (0x22 0xF7 without external LUT) does not drive the GDEY panel visibly.
 *
 * The panel rail may have been off since the last refresh (deep sleep), which
 * wipes every register and both RAM banks, so each refresh runs the complete
 * init and rewrites both RAMs.
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

/* A full refresh takes ~2-3 s; give up well after that instead of hanging
   forever on a stuck BUSY line. */
#define EPD_BUSY_TIMEOUT_MS 10000
/* RES# low and recovery time, and the wait after SW reset 0x12. */
#define EPD_RESET_MS 10
/* BUSY stays high in deep sleep, so entering it is a fixed wait. */
#define EPD_SLEEP_ENTRY_MS 20

/* Controller is in its own deep sleep (0x10); only a RES# pulse wakes it. */
static bool s_asleep = false;

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
    TickType_t start = xTaskGetTickCount();
    while (board_epd_busy_level() == 1) {
        if (xTaskGetTickCount() - start > pdMS_TO_TICKS(EPD_BUSY_TIMEOUT_MS)) {
            ESP_LOGE(TAG, "BUSY still high after %d ms, continuing", EPD_BUSY_TIMEOUT_MS);
            return;
        }
        /* Long enough for automatic light sleep between polls (a refresh
           takes 0.75-1.85 s, so 10 ms of latency does not matter). */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void reset_controller(void)
{
    gpio_set_level(BOARD_EPD_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(EPD_RESET_MS));
    gpio_set_level(BOARD_EPD_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(EPD_RESET_MS));
    s_asleep = false;
}

static void enter_deep_sleep(void)
{
    spi_cmd(0x10);
    spi_data_byte(0x01); /* Mode 1: RAM retained */
    vTaskDelay(pdMS_TO_TICKS(EPD_SLEEP_ENTRY_MS));
    s_asleep = true;
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

/* Complete init (Waveshare V2 EPD_Init, then EPD_Init_Partial for partial
   mode). The hardware reset comes first: it is the only way out of deep
   sleep, where BUSY stays high, so BUSY is not polled before it. */
static void init_controller(bool partial)
{
    reset_controller();

    spi_cmd(0x12); /* Software reset */
    vTaskDelay(pdMS_TO_TICKS(EPD_RESET_MS));
    epd_wait_idle();

    spi_cmd(0x01); /* Driver output control */
    spi_data_byte(0xC7);
    spi_data_byte(0x00);
    spi_data_byte(0x01);

    spi_cmd(0x11); /* Data entry mode: Y decrement, X increment */
    spi_data_byte(0x01);

    set_windows(0, EPD_HEIGHT - 1, EPD_WIDTH - 1, 0);

    spi_cmd(0x3C); /* Border waveform */
    spi_data_byte(0x01);

    spi_cmd(0x18); /* Use internal temperature sensor */
    spi_data_byte(0x80);

    spi_cmd(0x22); /* Load temperature and waveform setting */
    spi_data_byte(0xB1);
    spi_cmd(0x20);
    epd_wait_idle();

    if (!partial) {
        load_lut(LUT_FULL);
        return;
    }

    load_lut(LUT_PARTIAL);

    spi_cmd(0x37);
    spi_data((const uint8_t[]){0x00, 0x00, 0x00, 0x00, 0x00,
                               0x40, 0x00, 0x00, 0x00, 0x00}, 10);

    spi_cmd(0x3C); /* Border waveform for partial updates */
    spi_data_byte(0x80);

    spi_cmd(0x22); /* Clock and analog on ahead of the update */
    spi_data_byte(0xC0);
    spi_cmd(0x20);
    epd_wait_idle();
}

static void write_ram(uint8_t cmd, const uint8_t *buf)
{
    set_cursor(0, EPD_HEIGHT - 1);
    spi_cmd(cmd);
    spi_data(buf, EPD_BUF_SIZE);
}

void epd_refresh_fb(const uint8_t *fb, const uint8_t *prev_fb, bool partial)
{
    int64_t start = esp_timer_get_time();
    if (!prev_fb) partial = false;

    board_epd_power_on();
    if (!board_epd_is_powered()) {
        ESP_LOGE(TAG, "panel power-up failed, refresh skipped");
        return;
    }

    init_controller(partial);

    /* 0x26 holds the image on the glass, 0x24 the new one. */
    write_ram(0x26, partial ? prev_fb : fb);
    write_ram(0x24, fb);

    /* Both sequences end with analog and oscillator off. */
    spi_cmd(0x22);
    spi_data_byte(partial ? 0xCF : 0xC7);
    spi_cmd(0x20);
    epd_wait_idle();

    enter_deep_sleep();
    ESP_LOGI(TAG, "%s refresh: %d ms", partial ? "partial" : "full",
             (int)((esp_timer_get_time() - start) / 1000));
}

void epd_power_off(void)
{
    if (board_epd_is_powered() && !s_asleep) {
        epd_wait_idle();
        enter_deep_sleep();
    }
    board_epd_power_off();
    /* Unpowered now; the next refresh starts with a reset anyway. */
    s_asleep = false;
}
