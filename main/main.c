#include "battery.h"
#include "board.h"
#include "button.h"
#include "display_ui.h"
#include "ble_scanner.h"
#include "epd_ssd1680.h"
#include "govee_decode.h"
#include "history.h"
#include "sdlog.h"
#include "sensor_math.h"
#include "shtc3.h"
#include "sysclock.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rtc_time.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "driver/rtc_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <sys/time.h>
#include <time.h>

#define TAG "main"

#define NVS_NAMESPACE          "ui"
#define NVS_KEY_ROTATION       "rotation"
#define NVS_KEY_LAYOUT         "layout"

/* BOOT held this long after a deep-sleep wake opens the setup window again;
   shorter holds rotate (short) or switch the layout (BUTTON_LONG_PRESS_MS). */
#define SETUP_PRESS_MS         5000

/* In the setup window a new reading alone redraws at most this often;
   a new minute, a button press or a clock write redraws at once. */
#define SETUP_MIN_REDRAW_SEC   15

/* The web page may still be connected when the window ends; wait this much
   longer for it to finish. */
#define SETUP_GRACE_SEC        120

/* Wake this long after the minute starts. The RTC only has whole seconds
   (system time lags up to 1 s after a sync) and the ESP32's RC sleep timer
   drifts; neither may make the display show the previous minute. */
#define WAKE_OFFSET_MS         3000

/* Longest wait for BOOT to be released before sleeping. A button that is
   still held after this (stuck, or pressed by a case) must not keep the
   board awake: it then sleeps with the timer as the only wake source. */
#define RELEASE_WAIT_MS        10000

/* Readings older than this are not written to the SD card. */
#define SD_MAX_READING_AGE_SEC 600

#define STATE_MAGIC            0x45504432  /* "EPD2" */

/* Kept in RTC memory across deep sleep. Every other reset (power-on,
   flashing, panic) reloads the initializer, which fails the magic check. */
typedef struct {
    uint32_t magic;
    reading_t reading;
    int64_t reading_mono;     /* mono_sec() of the last reading, -1 = none */
    uint32_t cycle;           /* display refreshes since power-up */
    int64_t log_slot;         /* last SD log interval written or attempted */
    uint64_t next_wake_us;    /* esp_rtc_get_time_us() target while the clock is unset */
    bool indoor_valid;        /* on-board SHTC3 */
    float indoor_temp_c;
    int device_battery_pct;   /* -1 = unknown */
} rtc_state_t;

RTC_DATA_ATTR static rtc_state_t s_state;
RTC_DATA_ATTR static uint8_t s_prev_fb[EPD_BUF_SIZE];
static int16_t s_history[HISTORY_SLOTS];

/* Wall-clock time of the last frame drawn; the next wake is planned from it. */
static time_t s_shown_now;

/* Seconds since power-on; keeps counting through deep sleep and is not
   affected by setting the clock. */
static int64_t mono_sec(void)
{
    return (int64_t)(esp_rtc_get_time_us() / 1000000ULL);
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

static void reset_state(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.magic = STATE_MAGIC;
    s_state.reading.battery = -1;
    s_state.reading_mono = -1;
    s_state.log_slot = -1;
    s_state.device_battery_pct = -1;
    memset(s_prev_fb, 0xFF, sizeof(s_prev_fb));
    history_reset();
}

/* Indoor temperature and, with `battery`, the board's own battery. Read the
   battery only while the radio is off: its current pulls the voltage down. */
static void measure_board(bool battery)
{
    float temp, humidity;
    s_state.indoor_valid = shtc3_measure(&temp, &humidity);
    if (s_state.indoor_valid) {
        s_state.indoor_temp_c = temp;
        ESP_LOGI(TAG, "indoor %.2f C %.1f %%", temp, humidity);
    } else {
        ESP_LOGI(TAG, "indoor n/a");
    }
    if (battery) {
        int mv = battery_read_mv();
        s_state.device_battery_pct = mv > 0 ? battery_percent_from_mv(mv) : -1;
        ESP_LOGI(TAG, "battery %d mV (%d %%)", mv, s_state.device_battery_pct);
    }
}

static void store_reading(const reading_t *r)
{
    s_state.reading = *r;
    s_state.reading_mono = mono_sec();
    history_add((int16_t)(r->temp_c * 10.0f + (r->temp_c >= 0 ? 0.5f : -0.5f)),
                s_state.reading_mono);
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

static void apply_clock_request(const ble_clock_request_t *req)
{
    /* The web page sends the zone first, so the time is shown in it at once. */
    if (req->has_tz && sysclock_set_tz(req->tz) == ESP_OK) {
        ESP_LOGI(TAG, "time zone set to %s", req->tz);
    }
    /* The write waited in the queue (and maybe behind a refresh). */
    if (req->has_utc &&
        sysclock_set_utc_us(req->utc * 1000000LL + (esp_timer_get_time() - req->utc_rx_us)) == ESP_OK) {
        ESP_LOGI(TAG, "clock set to %lld UTC", (long long)req->utc);
    }
}

/* Draw the current state. Rotation and layout changes need `force_full`:
   every pixel moves and a partial refresh would ghost. */
static void show(bool force_full, bool ble_active)
{
    bool full = force_full || (s_state.cycle % CONFIG_APP_FULL_REFRESH_EVERY == 0);
    display_status_t status = {
        .now = time(NULL),
        .time_valid = sysclock_is_valid(),
        .ble_active = ble_active,
        .indoor_valid = s_state.indoor_valid,
        .indoor_temp_c = s_state.indoor_temp_c,
        .device_battery_pct = s_state.device_battery_pct,
    };
    s_shown_now = status.now;
    history_get(s_history, mono_sec());
    if (display_ui_render(&s_state.reading, s_history, s_prev_fb, !full, &status)) {
        memcpy(s_prev_fb, display_ui_get_fb(), EPD_BUF_SIZE);
        s_state.cycle++;
    }
}

/* Append one row per log interval, on the first cycle with a fresh reading.
   A missing card is not retried before the next interval. */
static void maybe_log_sd(void)
{
    if (!sysclock_is_valid() || s_state.reading_mono < 0) return;
    time_t now = time(NULL);
    int64_t slot = (int64_t)now / CONFIG_APP_SD_LOG_INTERVAL_SEC;
    if (slot == s_state.log_slot) return;
    if (mono_sec() - s_state.reading_mono > SD_MAX_READING_AGE_SEC) return;
    s_state.log_slot = slot;
    sdlog_append(&s_state.reading, now);
}

/* Awake with the BLE clock service: after power-up, or after holding BOOT
   for 5 s. Also the time window in which the board can be flashed. */
static void run_setup_window(bool first_frame_full)
{
    ESP_LOGI(TAG, "setup window: %d s, BLE clock service \"%s\"",
             CONFIG_APP_SETUP_WINDOW_SEC, CONFIG_APP_BLE_NAME);
    if (!button_init()) {
        ESP_LOGW(TAG, "button init failed, BOOT button disabled");
    }
    measure_board(true);
    bool ble_ok = ble_start(BLE_MODE_SETUP);
    if (!ble_ok) {
        ESP_LOGE(TAG, "BLE start failed, clock cannot be set");
    }

    int64_t start = mono_sec();
    int64_t last_draw = 0;
    int64_t last_minute = -1;
    bool dirty = true;
    bool force_full = first_frame_full;
    bool new_reading = false;

    while (true) {
        int64_t elapsed = mono_sec() - start;
        if (elapsed >= CONFIG_APP_SETUP_WINDOW_SEC &&
            (!ble_is_connected() || elapsed >= CONFIG_APP_SETUP_WINDOW_SEC + SETUP_GRACE_SEC)) {
            break;
        }

        button_events_t ev;
        if (button_take_events(200, &ev) && apply_button_events(&ev)) {
            dirty = force_full = true;
        }

        reading_t r;
        if (ble_ok && ble_wait_reading(&r, 0)) {
            bool first = s_state.reading_mono < 0;
            store_reading(&r);
            new_reading = true;
            if (first) dirty = true;
        }

        ble_clock_request_t req;
        if (ble_ok && ble_take_clock_request(&req)) {
            apply_clock_request(&req);
            dirty = true;
        }

        int64_t minute = (int64_t)time(NULL) / 60;
        if (minute != last_minute) {
            measure_board(!ble_ok);
            dirty = true;
        }
        if (new_reading && mono_sec() - last_draw >= SETUP_MIN_REDRAW_SEC) dirty = true;

        if (dirty) {
            show(force_full, ble_ok);
            last_draw = mono_sec();
            last_minute = minute;
            dirty = force_full = new_reading = false;
        }
        maybe_log_sd();
    }

    if (ble_ok) {
        ble_stop();
        /* A write may have arrived after the last poll. */
        ble_clock_request_t req;
        if (ble_take_clock_request(&req)) {
            apply_clock_request(&req);
        }
    }
    ESP_LOGI(TAG, "setup window over");
    if (ble_ok) {
        measure_board(true);
    }
    /* Remove the BLE symbol before sleeping. */
    show(false, false);
}

/* BOOT is down: classify the press by its length and act on it. `already_ms`
   is how long it has been held before this call. */
static void handle_boot_press(uint32_t already_ms)
{
    uint32_t limit = SETUP_PRESS_MS > already_ms ? SETUP_PRESS_MS - already_ms : 0;
    uint32_t held = already_ms + button_measure_hold_ms(limit);
    ESP_LOGI(TAG, "BOOT held %lu ms", (unsigned long)held);
    if (held >= SETUP_PRESS_MS) {
        /* Wait for the release, or the window's button handler would read
           the same press as a long press. */
        button_measure_hold_ms(RELEASE_WAIT_MS);
        if (button_is_pressed()) {
            ESP_LOGW(TAG, "BOOT not released, setup window skipped");
            return;
        }
        run_setup_window(false);
        return;
    }
    button_events_t ev = {
        .short_presses = held < BUTTON_LONG_PRESS_MS ? 1 : 0,
        .long_presses = held >= BUTTON_LONG_PRESS_MS ? 1 : 0,
    };
    apply_button_events(&ev);
    show(true, false);
}

static void run_wake_cycle(void)
{
    measure_board(true);
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) {
        /* The press started before the boot; esp_log_timestamp() counts
           from the wake reset. A quick tap is already over by now. */
        handle_boot_press(button_is_pressed() ? esp_log_timestamp() : 0);
        return;
    }

    if (ble_start(BLE_MODE_SCAN_ONCE)) {
        reading_t r;
        if (ble_wait_reading(&r, CONFIG_APP_WAKE_SCAN_TIMEOUT_MS)) {
            store_reading(&r);
        } else {
            ESP_LOGW(TAG, "no sensor reading within %d ms, showing last values",
                     CONFIG_APP_WAKE_SCAN_TIMEOUT_MS);
        }
        /* Radio off before the slow e-paper refresh. */
        ble_stop();
    } else {
        ESP_LOGE(TAG, "BLE scanner failed to start");
    }
    show(false, false);
    maybe_log_sd();

    /* A press during the cycle; presses already released are missed. */
    if (button_is_pressed()) {
        handle_boot_press(0);
    }
}

/* With a valid clock, wake just after the next interval boundary (the next
   minute for 60 s), so HH:MM is right; otherwise keep a steady interval. */
static uint64_t next_sleep_us(void)
{
    const int64_t interval_us = (int64_t)CONFIG_APP_WAKE_INTERVAL_SEC * 1000000;

    if (sysclock_is_valid()) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        int64_t now_us = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
        /* Plan from the minute on the glass: if a refresh ran past the
           boundary, wake at once to draw the new minute. */
        int64_t shown_us = (int64_t)s_shown_now * 1000000;
        int64_t next_us = (shown_us / interval_us + 1) * interval_us + WAKE_OFFSET_MS * 1000;
        if (next_us < now_us + 100000) {
            next_us = now_us + 100000;
        }
        return (uint64_t)(next_us - now_us);
    }

    /* Clock unset: keep a steady schedule; a button wake before the target
       keeps it. */
    uint64_t now = esp_rtc_get_time_us();
    uint64_t target = s_state.next_wake_us;
    if (target == 0 || target + (uint64_t)interval_us <= now) {
        target = now + interval_us;
    } else {
        while (target <= now + 1000000) {
            target += interval_us;
        }
    }
    s_state.next_wake_us = target;
    return target - now;
}

static void enter_deep_sleep(void)
{
    epd_power_off();
    /* BOOT is an ANY_LOW wake source: sleeping while it is held would wake
       at once. */
    button_measure_hold_ms(RELEASE_WAIT_MS);
    bool boot_stuck = button_is_pressed();
    board_prepare_deep_sleep();

    uint64_t sleep_us = next_sleep_us();
    esp_sleep_enable_timer_wakeup(sleep_us);
    if (boot_stuck) {
        ESP_LOGW(TAG, "BOOT still pressed, waking by timer only");
    } else {
        esp_sleep_enable_ext1_wakeup_io(1ULL << BOARD_BOOT_BUTTON_GPIO, ESP_EXT1_WAKEUP_ANY_LOW);
        rtc_gpio_pullup_en(BOARD_BOOT_BUTTON_GPIO);
        rtc_gpio_pulldown_dis(BOARD_BOOT_BUTTON_GPIO);
    }

    ESP_LOGI(TAG, "deep sleep for %llu ms", (unsigned long long)(sleep_us / 1000));
    esp_deep_sleep_disable_rom_logging();
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

    bool woke = esp_reset_reason() == ESP_RST_DEEPSLEEP && s_state.magic == STATE_MAGIC;

    if (!board_init()) {
        ESP_LOGE(TAG, "board init failed");
        return;
    }
    sysclock_init();
    shtc3_init(sysclock_i2c_bus());

    if (!display_ui_create_layout()) {
        ESP_LOGE(TAG, "display UI init failed");
        return;
    }
    display_ui_set_rotation(load_u8(NVS_KEY_ROTATION, DISPLAY_UI_ROTATION_DEFAULT));
    display_ui_set_layout(load_u8(NVS_KEY_LAYOUT, DISPLAY_UI_LAYOUT_TREND));

    if (woke) {
        run_wake_cycle();
    } else {
        ESP_LOGI(TAG, "power-up (reset reason %d)", esp_reset_reason());
        reset_state();
        run_setup_window(true);
    }
    enter_deep_sleep();
}
