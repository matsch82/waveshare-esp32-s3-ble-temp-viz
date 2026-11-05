#include "board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "board"

#define LATCH_MASK  (1ULL << BOARD_PWR_LATCH_GPIO)
#define PWR_EN_MASK (1ULL << BOARD_EPD_PWR_EN_GPIO)
#define BUSY_MASK   (1ULL << BOARD_EPD_BUSY_GPIO)
#define EPD_OUT_MASK ((1ULL << BOARD_EPD_RST_GPIO) | (1ULL << BOARD_EPD_DC_GPIO) | \
                      (1ULL << BOARD_EPD_CS_GPIO) | (1ULL << BOARD_EPD_SCLK_GPIO) | \
                      (1ULL << BOARD_EPD_MOSI_GPIO))

/* SSD1681 power-on flow: supply VCI, wait 10 ms before the reset. */
#define EPD_RAIL_SETTLE_MS 10

void *board_epd_spi_handle = NULL;

static bool s_epd_powered = false;

static void set_pins(uint64_t mask, uint32_t level)
{
    for (int pin = 0; pin < GPIO_NUM_MAX; pin++) {
        if (mask & (1ULL << pin)) gpio_set_level(pin, level);
    }
}

static void hold_pins(uint64_t mask, bool hold)
{
    for (int pin = 0; pin < GPIO_NUM_MAX; pin++) {
        if (!(mask & (1ULL << pin))) continue;
        if (hold) {
            gpio_hold_en(pin);
        } else {
            gpio_hold_dis(pin);
        }
    }
}

/* No pull resistors anywhere: GPIO6 has an external pull-up, and a pull on a
   panel line would either feed the unpowered panel or waste battery current. */
static esp_err_t config_pins(uint64_t mask, gpio_mode_t mode)
{
    const gpio_config_t io_conf = {
        .pin_bit_mask = mask,
        .mode = mode,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&io_conf);
}

/* Configure the pins and only then release a hold that
   board_prepare_deep_sleep() may have left on them. An output's level is
   written before the output is enabled, so pins that are not held (cold
   boot) come up at that level directly as well. */
static esp_err_t take_over_pins(uint64_t mask, gpio_mode_t mode, uint32_t level)
{
    if (mode == GPIO_MODE_OUTPUT) set_pins(mask, level);
    esp_err_t err = config_pins(mask, mode);
    if (err != ESP_OK) return err;
    hold_pins(mask, false);
    return ESP_OK;
}

/* Panel inputs low and BUSY floating, as required while the rail is off.
   gpio_config also detaches SCLK/MOSI from the SPI peripheral. */
static void epd_lines_idle(void)
{
    set_pins(EPD_OUT_MASK, 0);
    config_pins(EPD_OUT_MASK, GPIO_MODE_OUTPUT);
    config_pins(BUSY_MASK, GPIO_MODE_INPUT);
}

static bool spi_up(void)
{
    const spi_bus_config_t buscfg = {
        .mosi_io_num = BOARD_EPD_MOSI_GPIO,
        .miso_io_num = -1,
        .sclk_io_num = BOARD_EPD_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        /* A whole 200x200 1bpp frame goes out in one transaction while CS is
           held low. */
        .max_transfer_sz = BOARD_EPD_WIDTH * BOARD_EPD_HEIGHT / 8,
    };
    const spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 10 * 1000 * 1000,
        .mode = 0,
        /* CS is driven manually so it stays asserted for a full RAM write. */
        .spics_io_num = -1,
        .queue_size = 1,
    };

    esp_err_t err = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return false;
    }
    spi_device_handle_t dev = NULL;
    err = spi_bus_add_device(SPI2_HOST, &devcfg, &dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed: %s", esp_err_to_name(err));
        spi_bus_free(SPI2_HOST);
        return false;
    }
    board_epd_spi_handle = dev;
    return true;
}

/* Freeing the bus resets SCLK/MOSI to inputs with pull-up; the caller drives
   them low again right after. */
static void spi_down(void)
{
    if (!board_epd_spi_handle) return;
    spi_bus_remove_device((spi_device_handle_t)board_epd_spi_handle);
    spi_bus_free(SPI2_HOST);
    board_epd_spi_handle = NULL;
}

bool board_init(void)
{
    /* GPIO17 keeps the board alive on battery, so it goes first and is never
       allowed to drop. */
    esp_err_t err = take_over_pins(LATCH_MASK, GPIO_MODE_OUTPUT, 1);
    if (err == ESP_OK) err = take_over_pins(PWR_EN_MASK, GPIO_MODE_OUTPUT, 1);
    if (err == ESP_OK) err = take_over_pins(EPD_OUT_MASK, GPIO_MODE_OUTPUT, 0);
    if (err == ESP_OK) err = take_over_pins(BUSY_MASK, GPIO_MODE_INPUT, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(err));
        return false;
    }

    /* ESP-IDF isolates every GPIO during automatic light sleep (its ESD
       reset workaround). These must keep driving through it: the latch, or
       the board switches off on battery, and the panel rail and lines, or the
       panel loses power or its RST/CS mid-refresh. Call before power
       management enables light sleep. */
    for (int gpio = 0; gpio < 64; gpio++) {
        if ((LATCH_MASK | PWR_EN_MASK | EPD_OUT_MASK) & (1ULL << gpio)) {
            gpio_sleep_sel_dis((gpio_num_t)gpio);
        }
    }
    return true;
}

void board_epd_power_on(void)
{
    if (s_epd_powered) return;

    /* The panel lines are still low here, so the controller is not fed
       through its inputs before its supply is up. */
    gpio_set_level(BOARD_EPD_PWR_EN_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(EPD_RAIL_SETTLE_MS));

    if (!spi_up()) {
        epd_lines_idle();
        gpio_set_level(BOARD_EPD_PWR_EN_GPIO, 1);
        return;
    }
    gpio_set_level(BOARD_EPD_CS_GPIO, 1);
    gpio_set_level(BOARD_EPD_RST_GPIO, 1);
    s_epd_powered = true;
}

void board_epd_power_off(void)
{
    if (!s_epd_powered) return;

    spi_down();
    epd_lines_idle();
    gpio_set_level(BOARD_EPD_PWR_EN_GPIO, 1);
    s_epd_powered = false;
}

bool board_epd_is_powered(void)
{
    return s_epd_powered;
}

void board_prepare_deep_sleep(void)
{
    board_epd_power_off();

    /* GPIO0-21 are RTC IOs on the S3: gpio_hold_en() latches them in the RTC
       domain, which stays powered through deep sleep. */
    gpio_set_level(BOARD_PWR_LATCH_GPIO, 1);
    gpio_set_level(BOARD_EPD_PWR_EN_GPIO, 1);
    epd_lines_idle();
    hold_pins(LATCH_MASK | PWR_EN_MASK | EPD_OUT_MASK | BUSY_MASK, true);
}

int board_epd_busy_level(void)
{
    return gpio_get_level(BOARD_EPD_BUSY_GPIO);
}
