#include "ble_scanner.h"
#include "govee_decode.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <time.h>

#define TAG "ble"

static int s_seq = 0;

static const char *s_prefix = CONFIG_APP_SENSOR_NAME_PREFIX;

static char s_target_name[32] = {0};
static uint8_t s_pending_mfr[32] = {0};
static uint8_t s_pending_mfr_len = 0;

static SemaphoreHandle_t s_found_sem = NULL;
static reading_t s_reading = {0};
static bool s_found = false;
static uint32_t s_adv_count = 0;

static bool name_matches_prefix(const char *name)
{
    size_t prefix_len = strlen(s_prefix);
    size_t name_len = strlen(name);
    if (prefix_len == 0 || name_len < prefix_len) return false;

    /* Case-insensitive substring match: the configured "prefix" can appear
       anywhere in the advertised name (e.g. H5171_xxxx, Govee_H5171, ...). */
    for (size_t i = 0; i <= name_len - prefix_len; i++) {
        bool match = true;
        for (size_t j = 0; j < prefix_len; j++) {
            if (toupper((unsigned char)name[i + j]) !=
                toupper((unsigned char)s_prefix[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

static void decode_and_store(uint16_t company, const uint8_t *mfr_payload, uint8_t mfr_len, int rssi, const char *name)
{
    if (s_found) return;

    reading_t r = {0};
    if (!govee_decode(company, mfr_payload, mfr_len, &r)) return;

    r.rssi = rssi;
    r.seq = ++s_seq;
    r.timestamp = time(NULL);
    s_reading = r;
    s_found = true;

    ESP_LOGI(TAG, "%s: %.2f C / %.1f %% / %d%% batt  rssi %d  %s",
             name, r.temp_c, r.humidity, r.battery, r.rssi, r.codec);

    if (s_found_sem) {
        xSemaphoreGive(s_found_sem);
    }
}

static void try_decode_pending_mfr(int rssi)
{
    if (s_target_name[0] == '\0' || s_pending_mfr_len < 2) return;
    uint16_t company = s_pending_mfr[0] | (s_pending_mfr[1] << 8);
    decode_and_store(company, s_pending_mfr + 2, s_pending_mfr_len - 2, rssi, s_target_name);
    s_pending_mfr_len = 0;
}

static int event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        const struct ble_gap_disc_desc *disc = &event->disc;
        uint8_t adv_len = disc->length_data > 31 ? 31 : disc->length_data;
        if (adv_len == 0) break;
        s_adv_count++;

        const uint8_t *data = disc->data;
        const uint8_t *end = data + adv_len;

        const uint8_t *p = data;
        bool got_mfr = false;
        uint16_t mfr_company = 0;
        const uint8_t *mfr_payload = NULL;
        uint8_t mfr_payload_len = 0;
        const uint8_t *mfr_full = NULL;
        uint8_t mfr_full_len = 0;
        char adv_name[32] = {0};

        while (p < end) {
            uint8_t len = p[0];
            if (len == 0 || p + len + 1 > end) break;
            uint8_t type = p[1];
            const uint8_t *payload = p + 2;
            uint8_t payload_len = len - 1;

            if (type == 0x08 || type == 0x09) { /* Short or complete local name */
                uint8_t copy_len = payload_len < sizeof(adv_name) - 1 ? payload_len : sizeof(adv_name) - 1;
                memcpy(adv_name, payload, copy_len);
                adv_name[copy_len] = '\0';
            } else if (type == 0xFF && payload_len >= 2 && payload_len <= sizeof(s_pending_mfr)) {
                mfr_company = payload[0] | (payload[1] << 8);
                mfr_payload = payload + 2;
                mfr_payload_len = payload_len - 2;
                mfr_full = payload;
                mfr_full_len = payload_len;
                got_mfr = true;
            }
            p += len + 1;
        }

        /* Log every received advertisement so the user can see what is around. */
        if (got_mfr) {
            char hex[65] = {0};
            for (uint8_t i = 0; i < mfr_full_len && i < 32; i++) {
                snprintf(hex + i * 2, sizeof(hex) - i * 2, "%02x", mfr_full[i]);
            }
            ESP_LOGI(TAG, "adv: rssi=%d name='%s' mfr=%s", disc->rssi,
                     adv_name[0] ? adv_name : "-", hex);
        } else {
            ESP_LOGI(TAG, "adv: rssi=%d name='%s' (no mfr)", disc->rssi,
                     adv_name[0] ? adv_name : "-");
        }

        if (adv_name[0] != '\0' && name_matches_prefix(adv_name)) {
            strncpy(s_target_name, adv_name, sizeof(s_target_name) - 1);
            s_target_name[sizeof(s_target_name) - 1] = '\0';
            ESP_LOGI(TAG, "target sensor found: %s", s_target_name);
            try_decode_pending_mfr(disc->rssi);
        }

        if (got_mfr) {
            if (s_target_name[0] != '\0') {
                decode_and_store(mfr_company, mfr_payload, mfr_payload_len, disc->rssi, s_target_name);
            } else if (!s_found) {
                /* Save mfr data until we know the name of the target sensor. */
                uint8_t full_len = mfr_payload_len + 2;
                if (full_len <= sizeof(s_pending_mfr)) {
                    s_pending_mfr[0] = mfr_company & 0xFF;
                    s_pending_mfr[1] = (mfr_company >> 8) & 0xFF;
                    memcpy(s_pending_mfr + 2, mfr_payload, mfr_payload_len);
                    s_pending_mfr_len = full_len;
                }
            }
        }
        break;
    }
    default:
        break;
    }
    return 0;
}

static void start_scan(void)
{
    struct ble_gap_disc_params params = {
        .itvl = 0,
        .window = 0,
        .passive = 0,   /* active scan to receive scan responses (name + mfg data) */
        .filter_duplicates = 0,
        .filter_policy = 0,
        .limited = 0,
    };

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &params, event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "BLE active scan started");
    }
}

static void on_sync(void)
{
    start_scan();
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

bool ble_scanner_start_once(void)
{
    /* Reset per-scan state. */
    s_found = false;
    s_target_name[0] = '\0';
    s_pending_mfr_len = 0;
    memset(&s_reading, 0, sizeof(s_reading));

    if (s_found_sem) {
        vSemaphoreDelete(s_found_sem);
    }
    s_found_sem = xSemaphoreCreateBinary();
    if (!s_found_sem) {
        ESP_LOGE(TAG, "failed to create semaphore");
        return false;
    }

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        vSemaphoreDelete(s_found_sem);
        s_found_sem = NULL;
        return false;
    }

    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
    return true;
}

bool ble_scanner_get_one_reading(reading_t *out, uint32_t timeout_ms)
{
    if (!s_found_sem) {
        return false;
    }

    bool got = (xSemaphoreTake(s_found_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE);
    if (got && out) {
        *out = s_reading;
    } else if (!got) {
        ESP_LOGW(TAG, "no sensor reading within %lu ms", (unsigned long)timeout_ms);
    }

    ble_scanner_stop();
    return got;
}

void ble_scanner_stop(void)
{
    ble_gap_disc_cancel();
    nimble_port_stop();
    if (s_found_sem) {
        vSemaphoreDelete(s_found_sem);
        s_found_sem = NULL;
    }
    nimble_port_deinit();
}

uint32_t ble_scanner_adv_count(void)
{
    return s_adv_count;
}
