#include "display_ui.h"
#include "board.h"
#include "epd_ssd1680.h"
#include "icons.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "ui"

static uint8_t s_fb[EPD_BUF_SIZE];
static lv_display_t *s_disp = NULL;
static lv_obj_t *s_temp_label = NULL;
static lv_obj_t *s_hum_label = NULL;
static lv_obj_t *s_batt_label = NULL;
static lv_obj_t *s_rssi_label = NULL;
static lv_obj_t *s_footer_label = NULL;
static lv_obj_t *s_bt_icon = NULL;
static lv_obj_t *s_therm_icon = NULL;
static lv_obj_t *s_drop_icon = NULL;
static lv_obj_t *s_batt_icon = NULL;

/* LVGL 9 full-screen RGB565 render buffer (200x200x2 = 80 kB). */
static uint8_t *s_draw_buf = NULL;

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc((uint32_t)(uintptr_t)arg);
}

static void fb_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    uint16_t *color_p = (uint16_t *)px_map;

    for (int32_t y = 0; y < h; y++) {
        int32_t py = area->y1 + y;
        for (int32_t x = 0; x < w; x++) {
            int32_t px = area->x1 + x;
            uint16_t c = color_p[y * w + x];
            uint8_t r = (c >> 11) & 0x1F;
            uint8_t g = (c >> 5) & 0x3F;
            uint8_t b = c & 0x1F;
            uint8_t r8 = (r << 3) | (r >> 2);
            uint8_t g8 = (g << 2) | (g >> 4);
            uint8_t b8 = (b << 3) | (b >> 2);
            uint8_t luma = (uint8_t)((r8 * 299 + g8 * 587 + b8 * 114) / 1000);
            bool black = luma < 128;
            uint32_t byte_idx = (py * EPD_WIDTH + px) / 8;
            uint8_t bit_mask = 0x80 >> (px % 8);
            if (black) {
                s_fb[byte_idx] &= ~bit_mask; // black = 0 (panel expects 0=Black, 1=White)
            } else {
                s_fb[byte_idx] |= bit_mask;  // white = 1
            }
        }
    }
    lv_display_flush_ready(disp);
}

static const lv_image_dsc_t *make_icon(const uint8_t *data, uint16_t w, uint16_t h)
{
    static lv_image_dsc_t desc[4];
    static int idx = 0;
    lv_image_dsc_t *d = &desc[idx++];
    d->header.cf = LV_COLOR_FORMAT_A1;
    d->header.w = w;
    d->header.h = h;
    d->header.stride = (w + 7) / 8;
    d->data_size = ((w + 7) / 8) * h;
    d->data = data;
    return d;
}

bool display_ui_create_layout(void)
{
    memset(s_fb, 0xFF, sizeof(s_fb));

    lv_init();

    s_disp = lv_display_create(EPD_WIDTH, EPD_HEIGHT);
    if (!s_disp) {
        ESP_LOGE(TAG, "lv_display_create failed");
        return false;
    }

    s_draw_buf = heap_caps_malloc(EPD_WIDTH * EPD_HEIGHT * 2, MALLOC_CAP_DMA);
    if (!s_draw_buf) {
        ESP_LOGE(TAG, "failed to allocate LVGL draw buffer");
        return false;
    }
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(s_disp, fb_flush);
    lv_display_set_buffers(s_disp, s_draw_buf, NULL,
                           EPD_WIDTH * EPD_HEIGHT * 2,
                           LV_DISPLAY_RENDER_MODE_FULL);

    /* LVGL needs a periodic tick source. */
    const esp_timer_create_args_t tick_timer_args = {
        .callback = lvgl_tick_cb,
        .arg = (void *)5,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    if (esp_timer_create(&tick_timer_args, &tick_timer) == ESP_OK) {
        esp_timer_start_periodic(tick_timer, 5 * 1000);
    }

    lv_obj_t *scr = lv_screen_active();
    /* Header */
    lv_obj_t *header = lv_label_create(scr);
    lv_obj_set_style_text_font(header, &lv_font_montserrat_20, 0);
    lv_label_set_text(header, "AUSSEN");
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 6);

    s_bt_icon = lv_image_create(scr);
    lv_image_set_src(s_bt_icon, make_icon(icon_bluetooth, ICON_BLUETOOTH_WIDTH, ICON_BLUETOOTH_HEIGHT));
    lv_obj_set_style_image_recolor(s_bt_icon, lv_color_black(), 0);
    lv_obj_set_style_image_recolor_opa(s_bt_icon, LV_OPA_COVER, 0);
    lv_obj_align(s_bt_icon, LV_ALIGN_TOP_RIGHT, -8, 6);

    s_rssi_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_rssi_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_rssi_label, "--");
    lv_obj_align_to(s_rssi_label, s_bt_icon, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);

    /* Large temperature */
    s_therm_icon = lv_image_create(scr);
    lv_image_set_src(s_therm_icon, make_icon(icon_thermometer, ICON_THERMOMETER_WIDTH, ICON_THERMOMETER_HEIGHT));
    lv_obj_set_style_image_recolor(s_therm_icon, lv_color_black(), 0);
    lv_obj_set_style_image_recolor_opa(s_therm_icon, LV_OPA_COVER, 0);
    lv_obj_align(s_therm_icon, LV_ALIGN_LEFT_MID, 12, -20);

    s_temp_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_temp_label, &lv_font_montserrat_48, 0);
    lv_label_set_text(s_temp_label, "--.-°");
    lv_obj_align_to(s_temp_label, s_therm_icon, LV_ALIGN_OUT_RIGHT_MID, 4, 0);

    /* Humidity */
    s_drop_icon = lv_image_create(scr);
    lv_image_set_src(s_drop_icon, make_icon(icon_droplet, ICON_DROPLET_WIDTH, ICON_DROPLET_HEIGHT));
    lv_obj_set_style_image_recolor(s_drop_icon, lv_color_black(), 0);
    lv_obj_set_style_image_recolor_opa(s_drop_icon, LV_OPA_COVER, 0);
    lv_obj_align(s_drop_icon, LV_ALIGN_BOTTOM_LEFT, 12, -40);

    s_hum_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_hum_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_hum_label, "--%");
    lv_obj_align_to(s_hum_label, s_drop_icon, LV_ALIGN_OUT_RIGHT_MID, 4, 0);

    /* Battery */
    s_batt_icon = lv_image_create(scr);
    lv_image_set_src(s_batt_icon, make_icon(icon_battery, ICON_BATTERY_WIDTH, ICON_BATTERY_HEIGHT));
    lv_obj_set_style_image_recolor(s_batt_icon, lv_color_black(), 0);
    lv_obj_set_style_image_recolor_opa(s_batt_icon, LV_OPA_COVER, 0);
    lv_obj_align(s_batt_icon, LV_ALIGN_BOTTOM_RIGHT, -12, -40);

    s_batt_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_batt_label, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_batt_label, "--%");
    lv_obj_align_to(s_batt_label, s_batt_icon, LV_ALIGN_OUT_LEFT_MID, -4, 0);

    /* Footer */
    s_footer_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_footer_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_footer_label, "scanning ...");
    lv_obj_align(s_footer_label, LV_ALIGN_BOTTOM_MID, 0, -6);

    return true;
}

const uint8_t *display_ui_get_fb(void)
{
    return s_fb;
}

void display_ui_render(const reading_t *reading, const uint8_t *prev_fb, bool partial)
{
    char buf[64];

    /* LVGL's built-in formatter does not support %f by default; format
       temperature and humidity manually as fixed-point strings. */
    int t = (int)(reading->temp_c * 10.0f +
                  (reading->temp_c >= 0 ? 0.5f : -0.5f));
    int t_whole = t / 10;
    int t_frac = t % 10;
    if (t_frac < 0) t_frac = -t_frac;
    lv_snprintf(buf, sizeof(buf), "%d.%d°", t_whole, t_frac);
    lv_label_set_text(s_temp_label, buf);

    int h = (int)(reading->humidity * 10.0f + 0.5f);
    int h_whole = h / 10;
    int h_frac = h % 10;
    lv_snprintf(buf, sizeof(buf), "%d.%d%%", h_whole, h_frac);
    lv_label_set_text(s_hum_label, buf);

    lv_snprintf(buf, sizeof(buf), "%d%%", reading->battery >= 0 ? reading->battery : 0);
    lv_label_set_text(s_batt_label, buf);
    lv_label_set_text_fmt(s_rssi_label, "%d", reading->rssi);

    uint32_t uptime = xTaskGetTickCount() / configTICK_RATE_HZ;
    lv_snprintf(buf, sizeof(buf), "#%d  %lus", reading->seq, uptime);
    lv_label_set_text(s_footer_label, buf);

    lv_task_handler();
    epd_refresh_fb(s_fb, prev_fb, partial);
    ESP_LOGI(TAG, "screen updated: %.2f C / %.1f %% / %d%% batt (partial=%d)",
             reading->temp_c, reading->humidity, reading->battery, partial);
}
