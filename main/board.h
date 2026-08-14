#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Waveshare ESP32-S3-ePaper-1.54 GPIO allocation */
#define BOARD_EPD_BUSY_GPIO   8
#define BOARD_EPD_RST_GPIO    9
#define BOARD_EPD_DC_GPIO    10
#define BOARD_EPD_CS_GPIO    11
#define BOARD_EPD_SCLK_GPIO  12
#define BOARD_EPD_MOSI_GPIO  13
#define BOARD_EPD_PWR_EN_GPIO 6
#define BOARD_PWR_LATCH_GPIO 17

/* Panel geometry */
#define BOARD_EPD_WIDTH  200
#define BOARD_EPD_HEIGHT 200

/**
 * Board-level initialization: configure e-paper power, battery latch, SPI2 bus.
 * Must be called before any other board_* function.
 */
bool board_init(void);

/**
 * Enable e-paper power and hold the battery latch. Note that the EPD power
 * enable on GPIO6 is ACTIVE LOW on the ESP32-S3-ePaper-1.54 V2 board, while
 * the VBAT latch on GPIO17 is active high.
 */
void board_epd_power_on(void);

/**
 * Disable e-paper power (GPIO6 high).
 */
void board_epd_power_off(void);

/**
 * SPI2 handle used by the e-paper driver.
 */
extern void *board_epd_spi_handle;

/**
 * Busy GPIO level (0 = idle, 1 = busy) using the polarity documented by the
 * panel (BUSY pin active high on the Waveshare board).
 */
int board_epd_busy_level(void);

#ifdef __cplusplus
}
#endif
