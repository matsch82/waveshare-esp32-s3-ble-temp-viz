#include "board.h"
#include "display_ui.h"
#include "ble_scanner.h"
#include "epd_ssd1680.h"
#include "govee_decode.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

#define TAG "main"

#define BLE_SCAN_TIMEOUT_MS    10000
#define CYCLE_PAUSE_MS         5000

/* Keep the previous frame off the main task stack: a 5 kB local buffer plus
   NimBLE overflows CONFIG_ESP_MAIN_TASK_STACK_SIZE and reboots before any
   e-paper refresh can run. */
static reading_t s_last_reading;
static uint8_t s_prev_fb[EPD_BUF_SIZE];

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (!board_init()) {
        ESP_LOGE(TAG, "board init failed");
        return;
    }

    if (!display_ui_create_layout()) {
        ESP_LOGE(TAG, "display UI init failed");
        return;
    }

    memset(&s_last_reading, 0, sizeof(s_last_reading));
    s_last_reading.battery = -1;
    memset(s_prev_fb, 0xFF, sizeof(s_prev_fb));
    uint32_t cycle = 0;

    board_epd_power_on();

    while (true) {
        ESP_LOGI(TAG, "starting BLE scanner (cycle %lu)", (unsigned long)cycle);
        reading_t reading = s_last_reading;
        if (ble_scanner_start_once()) {
            if (ble_scanner_get_one_reading(&reading, BLE_SCAN_TIMEOUT_MS)) {
                s_last_reading = reading;
            } else {
                ESP_LOGW(TAG, "no new BLE reading, using last known values");
            }
        } else {
            ESP_LOGE(TAG, "BLE scanner failed to start");
        }

        bool partial = (cycle != 0) &&
                       (cycle % CONFIG_APP_FULL_REFRESH_EVERY != 0);
        display_ui_render(&s_last_reading, s_prev_fb, partial, ble_scanner_adv_count());
        memcpy(s_prev_fb, display_ui_get_fb(), EPD_BUF_SIZE);
        cycle++;

        vTaskDelay(pdMS_TO_TICKS(CYCLE_PAUSE_MS));
    }
}
