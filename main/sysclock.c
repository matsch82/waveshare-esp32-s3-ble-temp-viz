#include "sysclock.h"
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "board.h"
#include "civil_time.h"
#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#define TAG "sysclock"

#define I2C_FREQ_HZ     100000
#define I2C_TIMEOUT_MS  50

/* PCF85063A RTC. Registers 0x00..0x0A (Control_1, Control_2, Offset,
   RAM_byte, Seconds..Years) are read in one access. */
#define PCF_ADDR            0x51
#define PCF_REG_CONTROL_1   0x00
#define PCF_REG_SECONDS     0x04
#define PCF_READ_LEN        11
#define PCF_TIME_POS        (PCF_REG_SECONDS - PCF_REG_CONTROL_1)
#define PCF_CTRL1_CAP_SEL   0x01    /* crystal load capacitance: keep as found */
#define PCF_CTRL1_12_24     0x02
#define PCF_CTRL1_STOP      0x20
#define PCF_CTRL2_VALUE     0x07    /* COF = 111 (CLKOUT low), AIE/MI/HMI off, AF/TF cleared */
#define PCF_SECONDS_OS      0x80    /* oscillator stopped: time is lost */
#define PCF_YEAR_BASE       2000

/* 2099-12-31 23:59:59 UTC, the last second the two-digit year can hold. */
#define RTC_MAX_UTC         4102444799LL


#define NVS_NAMESPACE   "clock"
#define NVS_KEY_TZ      "tz"

_Static_assert(sizeof(CONFIG_APP_DEFAULT_TZ) - 1 <= SYSCLOCK_TZ_MAX_LEN, "CONFIG_APP_DEFAULT_TZ too long");

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_rtc = NULL;

/* Copy of the active TZ for sysclock_get_tz(), which other tasks may call. */
static portMUX_TYPE s_tz_lock = portMUX_INITIALIZER_UNLOCKED;

/* Set when writing a new time to the RTC failed. The system time survives
   deep sleep, so the next wake writes it to the RTC instead of loading the
   old RTC time back. Cleared on power-up, when the system time is gone. */
RTC_DATA_ATTR static bool s_rtc_stale;
static char s_tz[SYSCLOCK_TZ_MAX_LEN + 1] = CONFIG_APP_DEFAULT_TZ;

static esp_err_t bus_init(void)
{
    if (s_bus) return ESP_OK;

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_I2C_SDA_GPIO,
        .scl_io_num = BOARD_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) return err;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF_ADDR,
        .scl_speed_hz = I2C_FREQ_HZ,
    };
    return i2c_master_bus_add_device(s_bus, &dev_cfg, &s_rtc);
}

/* Tells a missing/NACKing device (ESP_ERR_NOT_FOUND) from a stuck bus (ESP_ERR_TIMEOUT). */
static esp_err_t rtc_probe(void)
{
    return s_bus ? i2c_master_probe(s_bus, PCF_ADDR, I2C_TIMEOUT_MS) : ESP_ERR_INVALID_STATE;
}

static esp_err_t rtc_write(time_t utc)
{
    if (!s_rtc) return ESP_ERR_INVALID_STATE;

    civil_time_t t;
    civil_from_unix(utc, &t);
    /* Seconds..Years in one access so the clock cannot tick in between; OS = 0. */
    const uint8_t buf[8] = {
        PCF_REG_SECONDS,
        civil_bin_to_bcd(t.second),
        civil_bin_to_bcd(t.minute),
        civil_bin_to_bcd(t.hour),
        civil_bin_to_bcd(t.day),
        (uint8_t)t.weekday,
        civil_bin_to_bcd(t.month),
        civil_bin_to_bcd(t.year - PCF_YEAR_BASE),
    };
    return i2c_master_transmit(s_rtc, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

/* Converts the registers read from 0x00 to UTC. Returns NULL if the time is
   valid, otherwise why it is not. */
static const char *rtc_decode(const uint8_t r[PCF_READ_LEN], time_t *utc)
{
    const uint8_t *tr = &r[PCF_TIME_POS];
    if (r[0] & PCF_CTRL1_STOP) return "clock stopped";
    if (r[0] & PCF_CTRL1_12_24) return "12-hour mode";
    if (tr[0] & PCF_SECONDS_OS) return "oscillator stopped";

    int year = civil_bcd_to_bin(tr[6]);
    civil_time_t t = {
        .second = civil_bcd_to_bin(tr[0] & 0x7f),
        .minute = civil_bcd_to_bin(tr[1] & 0x7f),
        .hour = civil_bcd_to_bin(tr[2] & 0x3f),
        .day = civil_bcd_to_bin(tr[3] & 0x3f),
        .month = civil_bcd_to_bin(tr[5] & 0x1f),
        .year = year < 0 ? -1 : PCF_YEAR_BASE + year,
    };
    if (!civil_is_valid(&t)) return "field out of range";

    *utc = (time_t)civil_to_unix(&t);
    if (*utc < SYSCLOCK_MIN_VALID_UTC) return "never set";
    return NULL;
}

static void log_time(const char *what, time_t utc)
{
    struct tm tm_utc;
    struct tm tm_local;
    char s_utc[24];
    char s_local[40];
    gmtime_r(&utc, &tm_utc);
    localtime_r(&utc, &tm_local);
    strftime(s_utc, sizeof(s_utc), "%Y-%m-%d %H:%M:%S", &tm_utc);
    strftime(s_local, sizeof(s_local), "%Y-%m-%d %H:%M:%S %Z", &tm_local);
    ESP_LOGI(TAG, "%s %s UTC, local %s", what, s_utc, s_local);
}

/* Configures the RTC and sets the system time from it. */
static void rtc_load(void)
{
    uint8_t r[PCF_READ_LEN];
    const uint8_t start = PCF_REG_CONTROL_1;
    esp_err_t err = i2c_master_transmit_receive(s_rtc, &start, 1, r, sizeof(r), I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RTC unreachable: %s (probe 0x%02x: %s)",
                 esp_err_to_name(err), PCF_ADDR, esp_err_to_name(rtc_probe()));
        return;
    }

    /* Running, 24-hour mode, no correction interrupt, CLKOUT and interrupts off. */
    const uint8_t ctrl[3] = { PCF_REG_CONTROL_1, r[0] & PCF_CTRL1_CAP_SEL, PCF_CTRL2_VALUE };
    if (ctrl[1] != r[0] || ctrl[2] != r[1]) {
        err = i2c_master_transmit(s_rtc, ctrl, sizeof(ctrl), I2C_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "RTC control write failed: %s", esp_err_to_name(err));
        }
    }

    if (s_rtc_stale && sysclock_is_valid()) {
        time_t now = time(NULL);
        if (now <= RTC_MAX_UTC && rtc_write(now) == ESP_OK) {
            s_rtc_stale = false;
            log_time("RTC resynced from system time", now);
        }
        return;
    }

    time_t utc = 0;
    const char *problem = rtc_decode(r, &utc);
    if (problem) {
        const uint8_t *tr = &r[PCF_TIME_POS];
        ESP_LOGW(TAG, "RTC time invalid (%s): ctrl %02x %02x, time %02x %02x %02x %02x %02x %02x %02x",
                 problem, r[0], r[1], tr[0], tr[1], tr[2], tr[3], tr[4], tr[5], tr[6]);
        /* After a deep-sleep wake the system time may still be good. */
        time_t now = time(NULL);
        if (sysclock_is_valid() && now <= RTC_MAX_UTC && rtc_write(now) == ESP_OK) {
            log_time("RTC restored from system time", now);
        } else if (r[0] & (PCF_CTRL1_STOP | PCF_CTRL1_12_24)) {
            /* The control fix-up above made the registers look sane; store a
               time before SYSCLOCK_MIN_VALID_UTC so the next boot still
               treats the clock as unset. */
            rtc_write(946684800);   /* 2000-01-01 00:00:00 UTC */
        }
        return;
    }

    struct timeval tv = { .tv_sec = utc, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGE(TAG, "settimeofday failed");
        return;
    }
    log_time("RTC", utc);
}

static bool tz_is_valid(const char *tz)
{
    if (!tz) return false;
    size_t len = strnlen(tz, SYSCLOCK_TZ_MAX_LEN + 1);
    if (len == 0 || len > SYSCLOCK_TZ_MAX_LEN) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)tz[i];
        if (c < 0x20 || c > 0x7e) return false;
    }
    return true;
}

/* App task only: setenv/tzset must not race with each other. */
static void apply_tz(const char *tz)
{
    size_t len = strlen(tz);
    setenv("TZ", tz, 1);
    tzset();
    taskENTER_CRITICAL(&s_tz_lock);
    memcpy(s_tz, tz, len + 1);
    taskEXIT_CRITICAL(&s_tz_lock);
}

static void load_tz(void)
{
    char tz[SYSCLOCK_TZ_MAX_LEN + 1];
    size_t len = sizeof(tz);
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        err = nvs_get_str(nvs, NVS_KEY_TZ, tz, &len);
        nvs_close(nvs);
    }
    if (err == ESP_OK && tz_is_valid(tz)) {
        apply_tz(tz);
        return;
    }
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "stored TZ unusable (%s), using default",
                 err == ESP_OK ? "invalid" : esp_err_to_name(err));
    }
    apply_tz(CONFIG_APP_DEFAULT_TZ);
}

bool sysclock_init(void)
{
    /* TZ first so the log line below shows the right local time. */
    load_tz();

    esp_err_t err = bus_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        return sysclock_is_valid();
    }
    rtc_load();
    return sysclock_is_valid();
}

i2c_master_bus_handle_t sysclock_i2c_bus(void)
{
    return s_bus;
}

bool sysclock_is_valid(void)
{
    return time(NULL) >= SYSCLOCK_MIN_VALID_UTC;
}

esp_err_t sysclock_set_utc_us(int64_t utc_us)
{
    int64_t utc = utc_us / 1000000;
    if (utc < SYSCLOCK_MIN_VALID_UTC || utc > RTC_MAX_UTC) {
        ESP_LOGW(TAG, "rejected time %lld", (long long)utc);
        return ESP_ERR_INVALID_ARG;
    }

    struct timeval tv = { .tv_sec = (time_t)utc, .tv_usec = (suseconds_t)(utc_us % 1000000) };
    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGE(TAG, "settimeofday failed");
        return ESP_FAIL;
    }

    /* The RTC counts whole seconds from the moment it is written: write it
       right at the next second boundary. */
    gettimeofday(&tv, NULL);
    /* +2 ms: tick rounding must not land just before the boundary. */
    vTaskDelay(pdMS_TO_TICKS((1000000 - tv.tv_usec) / 1000 + 2));
    time_t now = time(NULL);
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 3 && (err = rtc_write(now)) != ESP_OK; i++) {
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    s_rtc_stale = err != ESP_OK;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RTC write failed: %s (probe 0x%02x: %s)",
                 esp_err_to_name(err), PCF_ADDR, esp_err_to_name(rtc_probe()));
        return err;
    }
    log_time("clock set to", now);
    return ESP_OK;
}

esp_err_t sysclock_set_tz(const char *posix_tz)
{
    if (!tz_is_valid(posix_tz)) return ESP_ERR_INVALID_ARG;

    /* NVS skips the flash write when the value is unchanged. */
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, NVS_KEY_TZ, posix_tz);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving TZ failed: %s", esp_err_to_name(err));
        return err;
    }
    apply_tz(posix_tz);
    ESP_LOGI(TAG, "time zone \"%s\"", posix_tz);
    return ESP_OK;
}

void sysclock_get_tz(char *buf, size_t len)
{
    if (!buf || len == 0) return;
    taskENTER_CRITICAL(&s_tz_lock);
    size_t n = strnlen(s_tz, len - 1);
    memcpy(buf, s_tz, n);
    taskEXIT_CRITICAL(&s_tz_lock);
    buf[n] = '\0';
}
