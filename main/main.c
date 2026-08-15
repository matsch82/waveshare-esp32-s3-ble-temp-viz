#include "board.h"
#include "display_ui.h"
#include "ble_scanner.h"
#include "epd_ssd1680.h"
#include "govee_decode.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "main"

#define DEBUG_NO_SLEEP         1

#define SLEEP_SECONDS          60
#define BLE_SCAN_TIMEOUT_MS    10000

/* RTC memory persists across deep sleep. */
RTC_NOINIT_ATTR static uint32_t rtc_wake_count;
RTC_NOINIT_ATTR static uint8_t  rtc_prev_fb[EPD_BUF_SIZE];
RTC_NOINIT_ATTR static reading_t rtc_last_reading;

static bool is_first_boot(void)
{
    return esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER;
}

static void init_rtc_state_on_first_boot(void)
{
    if (!is_first_boot()) {
        return;
    }
    rtc_wake_count = 0;
    memset(&rtc_last_reading, 0, sizeof(rtc_last_reading));
    rtc_last_reading.battery = -1;
    memset(rtc_prev_fb, 0xFF, sizeof(rtc_prev_fb));
}

static void __attribute__((unused)) enter_deep_sleep(void)
{
    /* Hold EPD power off during sleep. */
    gpio_hold_en(BOARD_EPD_PWR_EN_GPIO);
    gpio_deep_sleep_hold_en();

    esp_sleep_enable_timer_wakeup(SLEEP_SECONDS * 1000000ULL);
    ESP_LOGI(TAG, "entering deep sleep for %d seconds", SLEEP_SECONDS);
    esp_deep_sleep_start();
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* Release the deep-sleep hold so we can drive the EPD power pin again. */
    gpio_hold_dis(BOARD_EPD_PWR_EN_GPIO);
    gpio_deep_sleep_hold_dis();

    init_rtc_state_on_first_boot();

    if (!board_init()) {
        ESP_LOGE(TAG, "board init failed");
        return;
    }

    if (!display_ui_create_layout()) {
        ESP_LOGE(TAG, "display UI init failed");
        return;
    }

    while (true) {
        board_epd_power_on();

        ESP_LOGI(TAG, "starting BLE scanner (wake %lu)", (unsigned long)rtc_wake_count);
        reading_t reading = rtc_last_reading;
        if (ble_scanner_start_once()) {
            if (ble_scanner_get_one_reading(&reading, BLE_SCAN_TIMEOUT_MS)) {
                rtc_last_reading = reading;
            } else {
                ESP_LOGW(TAG, "no new BLE reading, using last known values");
            }
        } else {
            ESP_LOGE(TAG, "BLE scanner failed to start");
        }

        /* Determine refresh mode: full on first boot/wake and every N wakes. */
        bool partial = (rtc_wake_count != 0) &&
                       (rtc_wake_count % CONFIG_APP_FULL_REFRESH_EVERY != 0);

        display_ui_render(&rtc_last_reading, rtc_prev_fb, partial);

        /* Save the new framebuffer as the next partial-update baseline. */
        memcpy(rtc_prev_fb, display_ui_get_fb(), EPD_BUF_SIZE);
        rtc_wake_count++;

        board_epd_power_off();

#if DEBUG_NO_SLEEP
        ESP_LOGI(TAG, "DEBUG: pausing 5s before next cycle");
        vTaskDelay(pdMS_TO_TICKS(5000));
#else
        enter_deep_sleep();
#endif
    }
}
