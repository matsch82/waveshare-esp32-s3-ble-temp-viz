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

#include <string.h>

#define TAG "ui"

#if !LV_FONT_MONTSERRAT_12 || !LV_FONT_MONTSERRAT_14 || !LV_FONT_MONTSERRAT_16 || \
    !LV_FONT_MONTSERRAT_18 || !LV_FONT_MONTSERRAT_20 || !LV_FONT_MONTSERRAT_48
#error "UI fonts missing: delete sdkconfig so sdkconfig.defaults is applied again"
#endif

LV_FONT_DECLARE(font_digits_72);

/* Built-in Montserrat has no umlauts or sharp s, hence the capitals. */
#define TITLE_TEXT "AUSSEN"

/* Trend layout: chart area and the baseline of the big number in the
   focus layout, in logical (unrotated) pixels. */
#define CHART_X       12
#define CHART_Y       76
#define CHART_W       176
#define CHART_H       60
#define DOT_SIZE      8
#define FOCUS_BASELINE 104
#define FOCUS_UNIT_GAP 2

typedef struct {
    lv_obj_t *fill;
    int32_t max_w;
} battery_widget_t;

static uint8_t s_fb[EPD_BUF_SIZE];
static lv_display_t *s_disp = NULL;
static lv_obj_t *s_layout_root[DISPLAY_UI_LAYOUT_COUNT];
static display_ui_layout_t s_layout = DISPLAY_UI_LAYOUT_TREND;

/* Trend layout */
static lv_obj_t *s_t_temp, *s_t_max, *s_t_min, *s_t_hum, *s_t_batt;
static lv_obj_t *s_t_chart, *s_t_dot;
static lv_chart_series_t *s_t_series;
static battery_widget_t s_t_batt_icon;
static int32_t s_chart_vals[HISTORY_SLOTS];

/* Focus layout */
static lv_obj_t *s_f_temp, *s_f_unit, *s_f_hum, *s_f_batt;
static lv_obj_t *s_f_bars[4];
static battery_widget_t s_f_batt_icon;

/* LVGL 9 full-screen RGB565 render buffer (200x200x2 = 80 kB). */
static uint8_t *s_draw_buf = NULL;

/* Clockwise quarter turns applied in fb_flush(). */
static uint8_t s_rotation = DISPLAY_UI_ROTATION_DEFAULT;

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

            /* lv_display_set_rotation() only swaps the reported resolution;
               it does not rotate pixel data. Since the panel is square we
               rotate manually here: mapping each logical (px, py) pixel to
               its physical position after s_rotation clockwise turns. */
            int32_t dst_x, dst_y;
            switch (s_rotation) {
            case 0:
                dst_x = px;
                dst_y = py;
                break;
            case 1:
                dst_x = (EPD_WIDTH - 1) - py;
                dst_y = px;
                break;
            case 2:
                dst_x = (EPD_WIDTH - 1) - px;
                dst_y = (EPD_HEIGHT - 1) - py;
                break;
            default:
                dst_x = py;
                dst_y = (EPD_WIDTH - 1) - px;
                break;
            }

            uint32_t byte_idx = (dst_y * EPD_WIDTH + dst_x) / 8;
            uint8_t bit_mask = 0x80 >> (dst_x % 8);
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
    static lv_image_dsc_t desc[1];
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

static lv_obj_t *make_icon_image(lv_obj_t *parent, const lv_image_dsc_t *src, int32_t x, int32_t y)
{
    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, src);
    lv_obj_set_style_image_recolor(img, lv_color_black(), 0);
    lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
    lv_obj_set_pos(img, x, y);
    return img;
}

/* Plain rectangle without theme styles: filled black, or a black outline. */
static lv_obj_t *make_rect(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h,
                           int32_t border)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    if (border > 0) {
        lv_obj_set_style_border_width(obj, border, 0);
        lv_obj_set_style_border_color(obj, lv_color_black(), 0);
    } else {
        lv_obj_set_style_bg_color(obj, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    }
    return obj;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_label_set_text(label, text);
    return label;
}

/* Battery outline with a terminal nub and a fill bar that shows the level. */
static battery_widget_t make_battery(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    make_rect(parent, x, y, w, h, 2);
    make_rect(parent, x + w, y + h / 2 - 3, 3, 6, 0);
    battery_widget_t bw = {
        .fill = make_rect(parent, x + 3, y + 3, 0, h - 6, 0),
        .max_w = w - 6,
    };
    return bw;
}

static void set_battery(battery_widget_t *bw, int pct)
{
    int32_t w = pct > 0 ? (bw->max_w * pct + 50) / 100 : 0;
    lv_obj_set_width(bw->fill, w);
}

static lv_obj_t *make_layout_root(void)
{
    lv_obj_t *root = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, EPD_WIDTH, EPD_HEIGHT);
    lv_obj_set_scrollable(root, false);
    return root;
}

static void create_trend_layout(lv_obj_t *root, const lv_image_dsc_t *droplet)
{
    lv_obj_t *title = make_label(root, &lv_font_montserrat_12, TITLE_TEXT);
    lv_obj_set_style_text_letter_space(title, 1, 0);
    lv_obj_set_pos(title, 12, 5);

    s_t_temp = make_label(root, &lv_font_montserrat_48, "--.-°");
    lv_obj_set_pos(s_t_temp, 8, 20);

    s_t_max = make_label(root, &lv_font_montserrat_14, LV_SYMBOL_UP " --");
    lv_obj_align(s_t_max, LV_ALIGN_TOP_RIGHT, -12, 28);
    s_t_min = make_label(root, &lv_font_montserrat_14, LV_SYMBOL_DOWN " --");
    lv_obj_align(s_t_min, LV_ALIGN_TOP_RIGHT, -12, 50);

    /* Dotted baseline under the curve. */
    static lv_point_precise_t base_pts[] = {
        {CHART_X, CHART_Y + CHART_H + 3}, {CHART_X + CHART_W, CHART_Y + CHART_H + 3},
    };
    lv_obj_t *base = lv_line_create(root);
    lv_line_set_points(base, base_pts, 2);
    lv_obj_set_style_line_width(base, 1, 0);
    lv_obj_set_style_line_color(base, lv_color_black(), 0);
    lv_obj_set_style_line_dash_width(base, 2, 0);
    lv_obj_set_style_line_dash_gap(base, 3, 0);

    s_t_chart = lv_chart_create(root);
    lv_obj_remove_style_all(s_t_chart);
    lv_obj_set_pos(s_t_chart, CHART_X, CHART_Y);
    lv_obj_set_size(s_t_chart, CHART_W, CHART_H);
    lv_chart_set_type(s_t_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_t_chart, HISTORY_SLOTS);
    lv_chart_set_div_line_count(s_t_chart, 0, 0);
    /* 3 px survives the 1-bit threshold on steep segments; no point markers. */
    lv_obj_set_style_line_width(s_t_chart, 3, LV_PART_ITEMS);
    lv_obj_set_style_line_rounded(s_t_chart, true, LV_PART_ITEMS);
    lv_obj_set_style_width(s_t_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(s_t_chart, 0, LV_PART_INDICATOR);
    s_t_series = lv_chart_add_series(s_t_chart, lv_color_black(), LV_CHART_AXIS_PRIMARY_Y);

    s_t_dot = make_rect(root, 0, 0, DOT_SIZE, DOT_SIZE, 0);
    lv_obj_set_style_radius(s_t_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_hidden(s_t_dot, true);

    lv_obj_t *from = make_label(root, &lv_font_montserrat_12, "-24 h");
    lv_obj_set_pos(from, CHART_X, 142);
    lv_obj_t *to = make_label(root, &lv_font_montserrat_12, "jetzt");
    lv_obj_align(to, LV_ALIGN_TOP_RIGHT, -12, 142);

    make_rect(root, 12, 160, 176, 2, 0);

    make_icon_image(root, droplet, 13, 170);
    s_t_hum = make_label(root, &lv_font_montserrat_16, "--%");
    lv_obj_set_pos(s_t_hum, 31, 171);

    s_t_batt_icon = make_battery(root, 161, 174, 22, 12);
    s_t_batt = make_label(root, &lv_font_montserrat_16, "--%");
    lv_obj_align(s_t_batt, LV_ALIGN_TOP_RIGHT, -45, 171);
}

static void create_focus_layout(lv_obj_t *root, const lv_image_dsc_t *droplet)
{
    lv_obj_t *title = make_label(root, &lv_font_montserrat_14, TITLE_TEXT);
    lv_obj_set_style_text_letter_space(title, 1, 0);
    lv_obj_set_pos(title, 14, 10);

    /* Signal strength: four bars growing to the right, bottom-aligned. */
    for (int i = 0; i < 4; i++) {
        int32_t h = 5 + 4 * i;
        s_f_bars[i] = make_rect(root, 164 + 6 * i, 25 - h, 4, h, 0);
    }

    s_f_temp = make_label(root, &font_digits_72, "--.-");
    s_f_unit = make_label(root, &lv_font_montserrat_20, "°C");

    make_rect(root, 16, 140, 168, 2, 0);

    make_icon_image(root, droplet, 16, 159);
    s_f_hum = make_label(root, &lv_font_montserrat_18, "--%");
    lv_obj_set_pos(s_f_hum, 36, 160);

    s_f_batt_icon = make_battery(root, 153, 162, 27, 14);
    s_f_batt = make_label(root, &lv_font_montserrat_18, "--%");
    lv_obj_align(s_f_batt, LV_ALIGN_TOP_RIGHT, -54, 160);
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
    /* Rotation is handled manually in fb_flush() since lv_display_set_rotation()
       does not actually rotate pixel data in LVGL 9 (it only swaps the
       reported resolution). Widget layout coordinates below are unaffected. */

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

    const lv_image_dsc_t *droplet = make_icon(icon_droplet, ICON_DROPLET_WIDTH, ICON_DROPLET_HEIGHT);

    s_layout_root[DISPLAY_UI_LAYOUT_TREND] = make_layout_root();
    create_trend_layout(s_layout_root[DISPLAY_UI_LAYOUT_TREND], droplet);
    s_layout_root[DISPLAY_UI_LAYOUT_FOCUS] = make_layout_root();
    create_focus_layout(s_layout_root[DISPLAY_UI_LAYOUT_FOCUS], droplet);
    display_ui_set_layout(s_layout);

    return true;
}

const uint8_t *display_ui_get_fb(void)
{
    return s_fb;
}

void display_ui_set_rotation(uint8_t quarter_turns)
{
    s_rotation = quarter_turns % 4;
    /* Every pixel moves, so the whole screen must be rendered again. */
    if (s_disp) {
        lv_obj_invalidate(lv_screen_active());
    }
}

uint8_t display_ui_get_rotation(void)
{
    return s_rotation;
}

void display_ui_set_layout(display_ui_layout_t layout)
{
    s_layout = layout < DISPLAY_UI_LAYOUT_COUNT ? layout : DISPLAY_UI_LAYOUT_TREND;
    for (int i = 0; i < DISPLAY_UI_LAYOUT_COUNT; i++) {
        if (s_layout_root[i]) {
            lv_obj_set_hidden(s_layout_root[i], i != s_layout);
        }
    }
}

display_ui_layout_t display_ui_get_layout(void)
{
    return s_layout;
}

/* LVGL's built-in formatter does not support %f by default; format tenths
   manually. Handles -0.x correctly, where integer division loses the sign. */
static void fmt_deci(char *buf, size_t len, int32_t deci, const char *suffix)
{
    int32_t a = deci < 0 ? -deci : deci;
    lv_snprintf(buf, len, "%s%d.%d%s", deci < 0 ? "-" : "",
                (int)(a / 10), (int)(a % 10), suffix);
}

static int32_t to_deci(float v)
{
    return (int32_t)(v * 10.0f + (v >= 0 ? 0.5f : -0.5f));
}

/* "100.0%" does not fit next to the battery in the focus layout. */
static void fmt_humidity(char *buf, size_t len, float humidity)
{
    int32_t h = to_deci(humidity);
    if (h >= 1000) {
        lv_snprintf(buf, len, "100%%");
    } else {
        fmt_deci(buf, len, h, "%");
    }
}

static int rssi_bars(int rssi)
{
    if (rssi >= -60) return 4;
    if (rssi >= -70) return 3;
    if (rssi >= -80) return 2;
    return 1;
}

static void update_chart(const int16_t history[HISTORY_SLOTS])
{
    int16_t lo, hi;
    bool any = history_min_max(history, &lo, &hi);
    char buf[24];

    if (any) {
        fmt_deci(buf, sizeof(buf), hi, "");
        lv_label_set_text_fmt(s_t_max, LV_SYMBOL_UP " %s", buf);
        fmt_deci(buf, sizeof(buf), lo, "");
        lv_label_set_text_fmt(s_t_min, LV_SYMBOL_DOWN " %s", buf);
    } else {
        lv_label_set_text(s_t_max, LV_SYMBOL_UP " --");
        lv_label_set_text(s_t_min, LV_SYMBOL_DOWN " --");
    }

    /* Pad the range so the line is not clipped, and keep at least 3 degrees
       so sensor noise on a flat day does not look like a mountain range. */
    int32_t lo_r = any ? lo - 5 : 0;
    int32_t hi_r = any ? hi + 5 : 300;
    if (hi_r - lo_r < 30) {
        int32_t mid = (hi_r + lo_r) / 2;
        lo_r = mid - 15;
        hi_r = mid + 15;
    }
    lv_chart_set_axis_range(s_t_chart, LV_CHART_AXIS_PRIMARY_Y, lo_r, hi_r);

    for (int i = 0; i < HISTORY_SLOTS; i++) {
        s_chart_vals[i] = history[i] == HISTORY_NONE ? LV_CHART_POINT_NONE : history[i];
    }
    lv_chart_set_series_values(s_t_chart, s_t_series, s_chart_vals, HISTORY_SLOTS);
    lv_chart_refresh(s_t_chart);
}

/* Center the big number plus its unit; the unit's top lines up with the
   top of the digits. */
static void place_focus_temp(void)
{
    const lv_font_t *big = &font_digits_72;
    const lv_font_t *small = &lv_font_montserrat_20;
    int32_t tw = lv_obj_get_width(s_f_temp);
    int32_t uw = lv_obj_get_width(s_f_unit);
    int32_t x = (EPD_WIDTH - (tw + FOCUS_UNIT_GAP + uw)) / 2;
    int32_t cap_top = FOCUS_BASELINE - big->cap_height;

    lv_obj_set_pos(s_f_temp, x, FOCUS_BASELINE - (big->line_height - big->base_line));
    lv_obj_set_pos(s_f_unit, x + tw + FOCUS_UNIT_GAP,
                   cap_top + small->cap_height - (small->line_height - small->base_line));
}

bool display_ui_render(const reading_t *reading, const int16_t history[HISTORY_SLOTS],
                       const uint8_t *prev_fb, bool partial)
{
    char buf[24];

    /* seq is set by the scanner on every decoded packet, so 0 means no
       reading has been received since boot: keep the placeholders. */
    if (reading->seq > 0) {
        int32_t t = to_deci(reading->temp_c);
        fmt_deci(buf, sizeof(buf), t, "°");
        lv_label_set_text(s_t_temp, buf);
        fmt_deci(buf, sizeof(buf), t, "");
        lv_label_set_text(s_f_temp, buf);

        fmt_humidity(buf, sizeof(buf), reading->humidity);
        lv_label_set_text(s_t_hum, buf);
        lv_label_set_text(s_f_hum, buf);
    }

    if (reading->battery >= 0) {
        lv_snprintf(buf, sizeof(buf), "%d%%", reading->battery);
    } else {
        lv_snprintf(buf, sizeof(buf), "--%%");
    }
    lv_label_set_text(s_t_batt, buf);
    lv_label_set_text(s_f_batt, buf);
    set_battery(&s_t_batt_icon, reading->battery);
    set_battery(&s_f_batt_icon, reading->battery);

    int bars = reading->seq > 0 ? rssi_bars(reading->rssi) : 0;
    for (int i = 0; i < 4; i++) {
        bool on = i < bars;
        lv_obj_set_style_bg_opa(s_f_bars[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_f_bars[i], on ? 0 : 1, 0);
        lv_obj_set_style_border_color(s_f_bars[i], lv_color_black(), 0);
    }

    update_chart(history);

    /* Label sizes and chart geometry are needed for the manual placement. */
    lv_obj_update_layout(lv_screen_active());
    place_focus_temp();
    if (history[HISTORY_SLOTS - 1] != HISTORY_NONE) {
        lv_point_t p;
        lv_chart_get_point_pos_by_id(s_t_chart, s_t_series, HISTORY_SLOTS - 1, &p);
        lv_obj_set_pos(s_t_dot, CHART_X + p.x - DOT_SIZE / 2, CHART_Y + p.y - DOT_SIZE / 2);
        lv_obj_set_hidden(s_t_dot, false);
    } else {
        lv_obj_set_hidden(s_t_dot, true);
    }

    /* Render synchronously: lv_task_handler() only redraws when LVGL's
       refresh timer is due, which would leave s_fb stale. */
    lv_refr_now(s_disp);

    /* Nothing changed on screen: skip the refresh to spare the panel. */
    if (partial && memcmp(s_fb, prev_fb, EPD_BUF_SIZE) == 0) {
        ESP_LOGI(TAG, "screen unchanged, refresh skipped");
        return false;
    }

    epd_refresh_fb(s_fb, prev_fb, partial);
    ESP_LOGI(TAG, "screen updated: %.2f C / %.1f %% / %d%% batt (layout=%d partial=%d)",
             reading->temp_c, reading->humidity, reading->battery, s_layout, partial);
    return true;
}
