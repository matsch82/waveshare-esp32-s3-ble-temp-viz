#include "board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define TAG "board"

void *board_epd_spi_handle = NULL;

bool board_init(void)
{
    esp_err_t err;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOARD_EPD_PWR_EN_GPIO) | (1ULL << BOARD_PWR_LATCH_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(err));
        return false;
    }

    /* EPD power enable is ACTIVE LOW on the V2 board; VBAT latch is active high. */
    gpio_set_level(BOARD_EPD_PWR_EN_GPIO, 0);
    gpio_set_level(BOARD_PWR_LATCH_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    /* E-paper control pins */
    io_conf.pin_bit_mask = (1ULL << BOARD_EPD_RST_GPIO)
                         | (1ULL << BOARD_EPD_DC_GPIO)
                         | (1ULL << BOARD_EPD_CS_GPIO);
    io_conf.mode = GPIO_MODE_OUTPUT;
    err = gpio_config(&io_conf);
    if (err != ESP_OK) return false;

    gpio_set_level(BOARD_EPD_CS_GPIO, 1);
    gpio_set_level(BOARD_EPD_DC_GPIO, 0);
    gpio_set_level(BOARD_EPD_RST_GPIO, 1);

    /* Busy input */
    io_conf.pin_bit_mask = (1ULL << BOARD_EPD_BUSY_GPIO);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    err = gpio_config(&io_conf);
    if (err != ESP_OK) return false;

    spi_bus_config_t buscfg = {
        .mosi_io_num = BOARD_EPD_MOSI_GPIO,
        .miso_io_num = -1,
        .sclk_io_num = BOARD_EPD_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        /* The whole 200x200 1bpp frame must go out in one transaction while CS
           is held low, so the bus must allow a 5000 byte transfer. */
        .max_transfer_sz = BOARD_EPD_WIDTH * BOARD_EPD_HEIGHT,
    };

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 10 * 1000 * 1000,
        .mode = 0,
        /* CS is driven manually so it stays asserted for a full RAM write. */
        .spics_io_num = -1,
        .queue_size = 7,
        /* SSD1680 expects MSB-first data. */
    };

    /* ESP32-S3 SPI2 (a.k.a. HSPI) is used by the on-board e-paper. */
    err = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return false;
    }
    err = spi_bus_add_device(SPI2_HOST, &devcfg, (spi_device_handle_t *)&board_epd_spi_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

void board_epd_power_on(void)
{
    /* Active low enable. */
    gpio_set_level(BOARD_EPD_PWR_EN_GPIO, 0);
    gpio_set_level(BOARD_PWR_LATCH_GPIO, 1);
}

void board_epd_power_off(void)
{
    gpio_set_level(BOARD_EPD_PWR_EN_GPIO, 1);
}

int board_epd_busy_level(void)
{
    return gpio_get_level(BOARD_EPD_BUSY_GPIO);
}
