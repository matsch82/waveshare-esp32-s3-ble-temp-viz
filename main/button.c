#include "button.h"
#include "board.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define TAG "button"

/* Sample every 10 ms; a level must be stable for 3 samples (30 ms) to count,
   which filters contact bounce on both press and release. */
#define BUTTON_POLL_MS      10
#define BUTTON_STABLE_COUNT 3
#define BUTTON_LONG_POLLS   (BUTTON_LONG_PRESS_MS / BUTTON_POLL_MS)

static SemaphoreHandle_t s_event_sem = NULL;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static button_events_t s_events;

static bool s_pressed = false;
static uint8_t s_stable = 0;
static uint32_t s_held_polls = 0;
static bool s_long_sent = false;

static void report(bool long_press)
{
    taskENTER_CRITICAL(&s_lock);
    if (long_press) {
        s_events.long_presses++;
    } else {
        s_events.short_presses++;
    }
    taskEXIT_CRITICAL(&s_lock);
    xSemaphoreGive(s_event_sem);
}

static void button_poll_cb(void *arg)
{
    /* Active low: the button pulls the line to GND. */
    bool down = gpio_get_level(BOARD_BOOT_BUTTON_GPIO) == 0;

    if (down != s_pressed) {
        if (++s_stable < BUTTON_STABLE_COUNT) return;
        s_pressed = down;
        s_stable = 0;
        if (down) {
            s_held_polls = 0;
            s_long_sent = false;
        } else if (!s_long_sent) {
            report(false);
        }
        return;
    }

    s_stable = 0;
    if (s_pressed && !s_long_sent && ++s_held_polls >= BUTTON_LONG_POLLS) {
        s_long_sent = true;
        report(true);
    }
}

bool button_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BOARD_BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO config failed: %s", esp_err_to_name(err));
        return false;
    }

    s_event_sem = xSemaphoreCreateBinary();
    if (!s_event_sem) {
        ESP_LOGE(TAG, "failed to create semaphore");
        return false;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = button_poll_cb,
        .name = "button",
    };
    esp_timer_handle_t timer = NULL;
    err = esp_timer_create(&timer_args, &timer);
    if (err == ESP_OK) {
        err = esp_timer_start_periodic(timer, BUTTON_POLL_MS * 1000);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "poll timer failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool s_input_ready = false;

static void configure_input(void)
{
    if (s_input_ready) return;
    s_input_ready = true;
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BOARD_BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
}

bool button_is_pressed(void)
{
    configure_input();
    return gpio_get_level(BOARD_BOOT_BUTTON_GPIO) == 0;
}

uint32_t button_measure_hold_ms(uint32_t max_ms)
{
    configure_input();
    uint32_t held = 0;
    uint8_t released = 0;
    /* Count as released only after BUTTON_STABLE_COUNT high samples. */
    while (held < max_ms && released < BUTTON_STABLE_COUNT) {
        released = button_is_pressed() ? 0 : released + 1;
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
        held += BUTTON_POLL_MS;
    }
    return released >= BUTTON_STABLE_COUNT ? held - released * BUTTON_POLL_MS : held;
}

bool button_take_events(uint32_t timeout_ms, button_events_t *out)
{
    out->short_presses = 0;
    out->long_presses = 0;
    if (!s_event_sem) return false;
    if (xSemaphoreTake(s_event_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;

    taskENTER_CRITICAL(&s_lock);
    *out = s_events;
    s_events.short_presses = 0;
    s_events.long_presses = 0;
    taskEXIT_CRITICAL(&s_lock);
    return out->short_presses > 0 || out->long_presses > 0;
}
