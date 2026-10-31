#include "board.h"
#include "button.h"
#include "display_ui.h"
#include "ble_scanner.h"
#include "epd_ssd1680.h"
#include "govee_decode.h"
#include "history.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

#define TAG "main"

#define BLE_SCAN_TIMEOUT_MS    10000
#define CYCLE_PAUSE_MS         5000

#define NVS_NAMESPACE          "ui"
#define NVS_KEY_ROTATION       "rotation"
#define NVS_KEY_LAYOUT         "layout"

/* Keep the previous frame off the main task stack: a 5 kB local buffer plus
   NimBLE overflows CONFIG_ESP_MAIN_TASK_STACK_SIZE and reboots before any
   e-paper refresh can run. */
static reading_t s_last_reading;
static uint8_t s_prev_fb[EPD_BUF_SIZE];
static int16_t s_history[HISTORY_SLOTS];

static int64_t uptime_sec(void)
{
    return esp_timer_get_time() / 1000000;
}

static uint8_t load_u8(const char *key, uint8_t fallback)
{
    uint8_t value = fallback;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, key, &value);
        nvs_close(nvs);
    }
    return value;
}

static void save_u8(const char *key, uint8_t value)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, key, value);
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "saving %s failed: %s", key, esp_err_to_name(err));
    }
}

/* A short press turns the UI by another 90 degrees clockwise, a long press
   switches to the next layout. Returns true if the screen must be redrawn. */
static bool apply_button_events(const button_events_t *ev)
{
    if (ev->short_presses > 0) {
        uint8_t rotation = (display_ui_get_rotation() + ev->short_presses) % 4;
        display_ui_set_rotation(rotation);
        save_u8(NVS_KEY_ROTATION, rotation);
        ESP_LOGI(TAG, "rotation set to %u deg clockwise", rotation * 90);
    }
    if (ev->long_presses > 0) {
        display_ui_layout_t layout = (display_ui_get_layout() + ev->long_presses) %
                                     DISPLAY_UI_LAYOUT_COUNT;
        display_ui_set_layout(layout);
        save_u8(NVS_KEY_LAYOUT, layout);
        ESP_LOGI(TAG, "layout set to %d", layout);
    }
    return ev->short_presses > 0 || ev->long_presses > 0;
}

static void show(bool partial)
{
    history_get(s_history, uptime_sec());
    if (display_ui_render(&s_last_reading, s_history, s_prev_fb, partial)) {
        memcpy(s_prev_fb, display_ui_get_fb(), EPD_BUF_SIZE);
    }
}

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
    display_ui_set_rotation(load_u8(NVS_KEY_ROTATION, DISPLAY_UI_ROTATION_DEFAULT));
    display_ui_set_layout(load_u8(NVS_KEY_LAYOUT, DISPLAY_UI_LAYOUT_TREND));

    if (!button_init()) {
        ESP_LOGW(TAG, "button init failed, rotation button disabled");
    }

    memset(&s_last_reading, 0, sizeof(s_last_reading));
    s_last_reading.battery = -1;
    memset(s_prev_fb, 0xFF, sizeof(s_prev_fb));
    history_reset();
    uint32_t cycle = 0;

    board_epd_power_on();

    while (true) {
        ESP_LOGI(TAG, "starting BLE scanner (cycle %lu)", (unsigned long)cycle);
        reading_t reading = s_last_reading;
        if (ble_scanner_start_once()) {
            if (ble_scanner_get_one_reading(&reading, BLE_SCAN_TIMEOUT_MS)) {
                s_last_reading = reading;
                history_add((int16_t)(reading.temp_c * 10.0f + (reading.temp_c >= 0 ? 0.5f : -0.5f)),
                            uptime_sec());
            } else {
                ESP_LOGW(TAG, "no new BLE reading, using last known values");
            }
        } else {
            ESP_LOGE(TAG, "BLE scanner failed to start");
        }

        /* Rotation and layout changes move every pixel; a partial refresh
           would ghost. */
        button_events_t ev;
        bool full = (cycle % CONFIG_APP_FULL_REFRESH_EVERY == 0);
        if (button_take_events(0, &ev) && apply_button_events(&ev)) full = true;
        show(!full);
        cycle++;

        /* Wait for the next cycle, redrawing at once on a button press. */
        TickType_t pause = pdMS_TO_TICKS(CYCLE_PAUSE_MS);
        TickType_t start = xTaskGetTickCount();
        TickType_t elapsed;
        while ((elapsed = xTaskGetTickCount() - start) < pause) {
            if (!button_take_events(pdTICKS_TO_MS(pause - elapsed), &ev)) break;
            if (apply_button_events(&ev)) show(false);
        }
    }
}
