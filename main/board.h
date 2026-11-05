#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Waveshare ESP32-S3-ePaper-1.54 GPIO allocation (same on V1 and V2) */
#define BOARD_EPD_BUSY_GPIO   8
#define BOARD_EPD_RST_GPIO    9
#define BOARD_EPD_DC_GPIO    10
#define BOARD_EPD_CS_GPIO    11
#define BOARD_EPD_SCLK_GPIO  12
#define BOARD_EPD_MOSI_GPIO  13
#define BOARD_EPD_PWR_EN_GPIO 6
#define BOARD_PWR_LATCH_GPIO 17

/* BOOT button (active low, external 10k pull-up). Only a strapping pin at
   reset, free at runtime. */
#define BOARD_BOOT_BUTTON_GPIO 0

/* PWR button (active low). On battery it powers the board up; the firmware
   keeps power on by driving BOARD_PWR_LATCH_GPIO high. */
#define BOARD_PWR_BUTTON_GPIO 18

/* I2C bus: PCF85063A RTC (0x51), SHTC3 (0x70), ES8311 codec (0x18). */
#define BOARD_I2C_SDA_GPIO   47
#define BOARD_I2C_SCL_GPIO   48

/* Micro SD slot, SDMMC 1-bit, external 10k pull-ups, no card detect. */
#define BOARD_SD_CLK_GPIO    39
#define BOARD_SD_CMD_GPIO    41
#define BOARD_SD_D0_GPIO     40

/* Panel geometry */
#define BOARD_EPD_WIDTH  200
#define BOARD_EPD_HEIGHT 200

/**
 * Board-level initialization, safe after power-on and after deep sleep:
 * takes over the GPIO holds set by board_prepare_deep_sleep() without
 * glitching (power latch stays high, panel stays unpowered with its lines
 * low). The panel rail is left OFF; the e-paper driver powers it (and SPI2)
 * when it needs it.
 * Must be called before any other board_* function.
 */
bool board_init(void);

/**
 * Switch the panel rail on (GPIO6 low, active low), wait for the supply to
 * settle and release CS/RST to their idle-high level. No-op if already on.
 */
void board_epd_power_on(void);

/**
 * Switch the panel rail off. Drives every panel input low first so the
 * unpowered controller is not fed through its IO pins. No-op if already off.
 */
void board_epd_power_off(void);

/** True while the panel rail is on. */
bool board_epd_is_powered(void);

/**
 * Freeze the pins for deep sleep: power latch high, panel rail off, panel
 * lines low, BUSY floating input. Call right before esp_deep_sleep_start(),
 * after board_epd_power_off().
 */
void board_prepare_deep_sleep(void);

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
