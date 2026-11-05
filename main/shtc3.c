#include "shtc3.h"
#include "sensor_math.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "shtc3"

#define SHTC3_ADDR       0x70
#define SHTC3_FREQ_HZ    100000
#define SHTC3_TIMEOUT_MS 50
#define SHTC3_WAKEUP_US  300    /* datasheet maximum 240 us */
#define SHTC3_MEASURE_MS 13     /* normal mode, datasheet maximum 12.1 ms */

static const uint8_t CMD_WAKEUP[2] = { 0x35, 0x17 };
static const uint8_t CMD_SLEEP[2] = { 0xB0, 0x98 };
/* Normal mode, temperature first, no clock stretching. */
static const uint8_t CMD_MEASURE[2] = { 0x78, 0x66 };

static i2c_master_dev_handle_t s_dev = NULL;

static void sensor_sleep(void)
{
    esp_err_t err = i2c_master_transmit(s_dev, CMD_SLEEP, sizeof(CMD_SLEEP), SHTC3_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "sleep failed: %s", esp_err_to_name(err));
    }
}

/* A sleeping sensor may NACK the wakeup command itself, so the ACK check is
   off for this device; the CRCs catch a read that went wrong. */
static void sensor_wakeup(void)
{
    i2c_master_transmit(s_dev, CMD_WAKEUP, sizeof(CMD_WAKEUP), SHTC3_TIMEOUT_MS);
    esp_rom_delay_us(SHTC3_WAKEUP_US);
}

esp_err_t shtc3_init(i2c_master_bus_handle_t bus)
{
    if (s_dev) return ESP_OK;
    if (!bus) return ESP_ERR_INVALID_STATE;

    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SHTC3_ADDR,
        .scl_speed_hz = SHTC3_FREQ_HZ,
        .flags.disable_ack_check = 1,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "add device failed: %s", esp_err_to_name(err));
        return err;
    }
    sensor_wakeup();
    sensor_sleep();
    return ESP_OK;
}

bool shtc3_measure(float *temp_c, float *humidity)
{
    if (!s_dev) return false;

    sensor_wakeup();
    uint8_t buf[6] = { 0 };
    esp_err_t err = i2c_master_transmit(s_dev, CMD_MEASURE, sizeof(CMD_MEASURE), SHTC3_TIMEOUT_MS);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(SHTC3_MEASURE_MS));
        err = i2c_master_receive(s_dev, buf, sizeof(buf), SHTC3_TIMEOUT_MS);
    }
    sensor_sleep();

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "measure failed: %s", esp_err_to_name(err));
        return false;
    }
    if (shtc3_crc8(&buf[0], 2) != buf[2] || shtc3_crc8(&buf[3], 2) != buf[5]) {
        ESP_LOGW(TAG, "CRC mismatch: %02x %02x %02x %02x %02x %02x",
                 buf[0], buf[1], buf[2], buf[3], buf[4], buf[5]);
        return false;
    }
    *temp_c = shtc3_temp_c((uint16_t)(buf[0] << 8 | buf[1]));
    *humidity = shtc3_humidity((uint16_t)(buf[3] << 8 | buf[4]));
    return true;
}
