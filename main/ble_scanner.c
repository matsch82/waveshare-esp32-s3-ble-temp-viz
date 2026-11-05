#include "ble_scanner.h"
#include "govee_decode.h"
#include "sysclock.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <time.h>

#define TAG "ble"

/* Setup mode listens half of each interval so advertising and the connection
   get airtime; scan-once mode listens all the time for the fastest hit. */
#define SETUP_SCAN_ITVL BLE_GAP_SCAN_ITVL_MS(60)
#define SETUP_SCAN_WIN  BLE_GAP_SCAN_WIN_MS(30)
#define ONCE_SCAN_ITVL  BLE_GAP_SCAN_ITVL_MS(30)
#define ONCE_SCAN_WIN   BLE_GAP_SCAN_WIN_MS(30)

#define ADV_ITVL_MIN BLE_GAP_ADV_ITVL_MS(100)
#define ADV_ITVL_MAX BLE_GAP_ADV_ITVL_MS(150)

/* How long ble_stop() waits for a just-created host task to switch the host on. */
#define STOP_WAIT_ENABLED_MS 1000

#define SENSOR_MAGIC 0x47563531u

typedef struct {
    uint32_t magic;
    ble_addr_t addr;
    char name[32];
} saved_sensor_t;

/* Survives deep sleep, so a wake can match the sensor by address alone. */
static RTC_DATA_ATTR saved_sensor_t s_saved;

static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x6b, 0x5a, 0x4f, 0x3e, 0x2d, 0x1c, 0x8b, 0x9a,
    0x5f, 0x4e, 0x2d, 0x3c, 0x00, 0x10, 0x6b, 0x7a);
static const ble_uuid128_t s_time_uuid = BLE_UUID128_INIT(
    0x6b, 0x5a, 0x4f, 0x3e, 0x2d, 0x1c, 0x8b, 0x9a,
    0x5f, 0x4e, 0x2d, 0x3c, 0x01, 0x10, 0x6b, 0x7a);
static const ble_uuid128_t s_tz_uuid = BLE_UUID128_INIT(
    0x6b, 0x5a, 0x4f, 0x3e, 0x2d, 0x1c, 0x8b, 0x9a,
    0x5f, 0x4e, 0x2d, 0x3c, 0x02, 0x10, 0x6b, 0x7a);

static int s_seq = 0;

static const char *s_prefix = CONFIG_APP_SENSOR_NAME_PREFIX;

static ble_mode_t s_mode = BLE_MODE_SCAN_ONCE;
static uint8_t s_own_addr_type;
static bool s_started = false;
static volatile bool s_stopping = false;
static volatile bool s_connected = false;
static bool s_delivered = false;

static QueueHandle_t s_reading_q = NULL;

static portMUX_TYPE s_clock_lock = portMUX_INITIALIZER_UNLOCKED;
static ble_clock_request_t s_clock_req;

/* The name arrives in the scan response and the mfr data in the advertisement,
   so both are tied together by the advertiser's address. A remembered address
   is used right away; it stays replaceable by a name match until the sensor
   is confirmed (by name or by a reading) during this start. */
static bool s_target_valid = false;
static bool s_target_locked = false;
static ble_addr_t s_target_addr;
static char s_target_name[32] = {0};
static uint8_t s_pending_mfr[32] = {0};
static uint8_t s_pending_mfr_len = 0;
static ble_addr_t s_pending_addr;

static void adv_start(void);

/* After a failed connection the stack frees the connection slot only once
   the CONNECT callback returns, so advertising restarts from the queue. */
static struct ble_npl_event s_adv_retry_ev;
static bool s_adv_retry_init = false;
static void scan_start(void);

static void addr_to_str(const ble_addr_t *addr, char *out, size_t len)
{
    const uint8_t *v = addr->val;
    snprintf(out, len, "%02x:%02x:%02x:%02x:%02x:%02x", v[5], v[4], v[3], v[2], v[1], v[0]);
}

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

static bool scan_wanted(void)
{
    return !s_stopping && !(s_mode == BLE_MODE_SCAN_ONCE && s_delivered);
}

/* False while stopping or while the host resets; sync_cb restarts after a reset. */
static bool can_restart(void)
{
    return !s_stopping && ble_hs_synced();
}

static void decode_and_deliver(uint16_t company, const uint8_t *mfr_payload, uint8_t mfr_len, int rssi)
{
    if (s_mode == BLE_MODE_SCAN_ONCE && s_delivered) return;

    reading_t r = {0};
    if (!govee_decode(company, mfr_payload, mfr_len, &r)) return;

    s_target_locked = true;
    r.rssi = rssi;
    r.seq = ++s_seq;
    r.timestamp = time(NULL);

    ESP_LOGI(TAG, "%s: %.2f C / %.1f %% / %d%% batt  rssi %d  %s",
             s_target_name, r.temp_c, r.humidity, r.battery, r.rssi, r.codec);

    xQueueOverwrite(s_reading_q, &r);

    if (s_mode == BLE_MODE_SCAN_ONCE) {
        s_delivered = true;
        ble_gap_disc_cancel();
    }
}

static void try_decode_pending_mfr(int rssi)
{
    if (s_pending_mfr_len < 2) return;
    if (ble_addr_cmp(&s_pending_addr, &s_target_addr) == 0) {
        uint16_t company = s_pending_mfr[0] | (s_pending_mfr[1] << 8);
        decode_and_deliver(company, s_pending_mfr + 2, s_pending_mfr_len - 2, rssi);
    }
    s_pending_mfr_len = 0;
}

static void on_name_match(const ble_addr_t *addr, const char *name, int rssi)
{
    bool same = s_target_valid && ble_addr_cmp(addr, &s_target_addr) == 0;
    if (!same && s_target_locked) return;   /* first sensor confirmed in this start wins */

    s_target_locked = true;
    if (same && strcmp(name, s_target_name) == 0) return;

    s_target_valid = true;
    s_target_addr = *addr;
    strlcpy(s_target_name, name, sizeof(s_target_name));

    s_saved.addr = *addr;
    strlcpy(s_saved.name, name, sizeof(s_saved.name));
    s_saved.magic = SENSOR_MAGIC;

    char addr_str[18];
    addr_to_str(addr, addr_str, sizeof(addr_str));
    ESP_LOGI(TAG, "target sensor found: %s (%s)", name, addr_str);

    try_decode_pending_mfr(rssi);
}

static void log_adv(const struct ble_gap_disc_desc *disc, const char *name,
                    const uint8_t *mfr, uint8_t mfr_len)
{
    if (!ESP_LOG_ENABLED(ESP_LOG_DEBUG)) return;

    char addr_str[18];
    char hex[65] = "-";
    addr_to_str(&disc->addr, addr_str, sizeof(addr_str));
    for (uint8_t i = 0; i < mfr_len && i < 32; i++) {
        snprintf(hex + i * 2, sizeof(hex) - i * 2, "%02x", mfr[i]);
    }
    ESP_LOGD(TAG, "adv: %s rssi=%d name='%s' mfr=%s", addr_str, disc->rssi,
             name[0] ? name : "-", hex);
}

static void handle_report(const struct ble_gap_disc_desc *disc)
{
    if (s_mode == BLE_MODE_SCAN_ONCE && s_delivered) return;

    uint8_t adv_len = disc->length_data > 31 ? 31 : disc->length_data;
    if (adv_len == 0) return;

    const uint8_t *p = disc->data;
    const uint8_t *end = p + adv_len;
    bool got_mfr = false;
    uint16_t mfr_company = 0;
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
            mfr_full = payload;
            mfr_full_len = payload_len;
            got_mfr = true;
        }
        p += len + 1;
    }

    log_adv(disc, adv_name, mfr_full, mfr_full_len);

    if (adv_name[0] != '\0' && name_matches_prefix(adv_name)) {
        on_name_match(&disc->addr, adv_name, disc->rssi);
    }

    if (!got_mfr) return;

    if (s_target_valid && ble_addr_cmp(&disc->addr, &s_target_addr) == 0) {
        decode_and_deliver(mfr_company, mfr_full + 2, mfr_full_len - 2, disc->rssi);
    } else if (!s_target_locked) {
        /* Keep the mfr data until the name tells whether this is the sensor. */
        memcpy(s_pending_mfr, mfr_full, mfr_full_len);
        s_pending_mfr_len = mfr_full_len;
        s_pending_addr = disc->addr;
    }
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        handle_report(&event->disc);
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (scan_wanted() && can_restart()) scan_start();
        break;
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_connected = true;
            ESP_LOGI(TAG, "central connected");
        } else {
            ESP_LOGW(TAG, "connect failed: %d", event->connect.status);
            if (can_restart()) ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_adv_retry_ev);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_connected = false;
        ESP_LOGI(TAG, "central disconnected, reason 0x%x", event->disconnect.reason);
        if (can_restart()) adv_start();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (can_restart()) adv_start();
        break;
    default:
        break;
    }
    return 0;
}

static void scan_start(void)
{
    bool setup = s_mode == BLE_MODE_SETUP;
    struct ble_gap_disc_params params = {
        .itvl = setup ? SETUP_SCAN_ITVL : ONCE_SCAN_ITVL,
        .window = setup ? SETUP_SCAN_WIN : ONCE_SCAN_WIN,
        .passive = 0,           /* active: the name is in the scan response */
        .filter_duplicates = 0, /* every advertisement carries a new reading */
    };

    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &params, gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "scan start failed: %d", rc);
    }
}

static void adv_start(void)
{
    struct ble_hs_adv_fields fields = {
        .flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        .uuids128 = &s_svc_uuid,
        .num_uuids128 = 1,
        .uuids128_is_complete = 1,
    };
    int rc = ble_gap_adv_set_fields(&fields);

    if (rc == 0) {
        const char *name = ble_svc_gap_device_name();
        size_t name_len = strlen(name);
        struct ble_hs_adv_fields rsp = {
            .name = (const uint8_t *)name,
            .name_len = name_len > BLE_HS_ADV_MAX_FIELD_SZ ? BLE_HS_ADV_MAX_FIELD_SZ : name_len,
            .name_is_complete = name_len <= BLE_HS_ADV_MAX_FIELD_SZ,
        };
        rc = ble_gap_adv_rsp_set_fields(&rsp);
    }

    if (rc == 0) {
        struct ble_gap_adv_params params = {
            .conn_mode = BLE_GAP_CONN_MODE_UND,
            .disc_mode = BLE_GAP_DISC_MODE_GEN,
            .itvl_min = ADV_ITVL_MIN,
            .itvl_max = ADV_ITVL_MAX,
        };
        rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event_cb, NULL);
    }

    /* ENOMEM: the only connection slot is in use, expected while connected. */
    if (rc != 0 && rc != BLE_HS_EALREADY && !(rc == BLE_HS_ENOMEM && s_connected)) {
        ESP_LOGE(TAG, "advertising start failed: %d", rc);
    }
}

static void adv_retry_cb(struct ble_npl_event *ev)
{
    if (!s_connected && can_restart()) adv_start();
}

static int time_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint8_t buf[8];

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint64_t t = (uint64_t)(int64_t)time(NULL);
        for (int i = 0; i < 8; i++) {
            buf[i] = (uint8_t)(t >> (8 * i));
        }
        return os_mbuf_append(ctxt->om, buf, sizeof(buf)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;

    if (OS_MBUF_PKTLEN(ctxt->om) != sizeof(buf)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint16_t n = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &n) != 0 || n != sizeof(buf)) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint64_t t = 0;
    for (int i = 7; i >= 0; i--) {
        t = (t << 8) | buf[i];
    }
    int64_t utc = (int64_t)t;
    if (utc < SYSCLOCK_MIN_VALID_UTC) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;

    /* Applied by the app task: setting the RTC is I2C work. */
    taskENTER_CRITICAL(&s_clock_lock);
    s_clock_req.has_utc = true;
    s_clock_req.utc = utc;
    s_clock_req.utc_rx_us = esp_timer_get_time();
    taskEXIT_CRITICAL(&s_clock_lock);
    ESP_LOGI(TAG, "clock write: utc %lld", (long long)utc);
    return 0;
}

static int tz_access(uint16_t conn_handle, uint16_t attr_handle,
                     struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    char tz[SYSCLOCK_TZ_MAX_LEN + 1];

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        sysclock_get_tz(tz, sizeof(tz));
        return os_mbuf_append(ctxt->om, tz, strlen(tz)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0 || len > SYSCLOCK_TZ_MAX_LEN) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    uint16_t n = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, tz, SYSCLOCK_TZ_MAX_LEN, &n) != 0 || n != len) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    for (uint16_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)tz[i];
        if (c < 0x20 || c > 0x7e) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    }
    tz[n] = '\0';

    /* Applied by the app task: setenv/tzset and the NVS commit. */
    taskENTER_CRITICAL(&s_clock_lock);
    s_clock_req.has_tz = true;
    memcpy(s_clock_req.tz, tz, n + 1);
    taskEXIT_CRITICAL(&s_clock_lock);
    ESP_LOGI(TAG, "clock write: tz %s", tz);
    return 0;
}

static const struct ble_gatt_chr_def s_clock_chrs[] = {
    {
        .uuid = &s_time_uuid.u,
        .access_cb = time_access,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
    },
    {
        .uuid = &s_tz_uuid.u,
        .access_cb = tz_access,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE,
    },
    { 0 },
};

static const struct ble_gatt_svc_def s_clock_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = s_clock_chrs,
    },
    { 0 },
};

/* Must run between nimble_port_init() and nimble_port_freertos_init():
   deinit drops the GATT database, and it is frozen once the host runs. */
static bool register_clock_service(void)
{
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_clock_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(s_clock_svcs);
    if (rc == 0) rc = ble_svc_gap_device_name_set(CONFIG_APP_BLE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "clock service setup failed: %d", rc);
        return false;
    }
    return true;
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "no usable BLE address: %d", rc);
        return;
    }
    if (s_stopping) return;

    if (s_mode == BLE_MODE_SETUP) adv_start();
    if (scan_wanted()) scan_start();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason %d", reason);
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

bool ble_start(ble_mode_t mode)
{
    if (s_started) {
        ESP_LOGE(TAG, "already started");
        return false;
    }

    if (!s_reading_q) {
        s_reading_q = xQueueCreate(1, sizeof(reading_t));
        if (!s_reading_q) {
            ESP_LOGE(TAG, "failed to create reading queue");
            return false;
        }
    }
    xQueueReset(s_reading_q);

    /* Per-start state; the remembered sensor address is kept. */
    s_mode = mode;
    s_stopping = false;
    s_connected = false;
    s_delivered = false;
    s_pending_mfr_len = 0;
    s_target_locked = false;
    s_target_valid = s_saved.magic == SENSOR_MAGIC;
    if (s_target_valid) {
        s_target_addr = s_saved.addr;
        strlcpy(s_target_name, s_saved.name, sizeof(s_target_name));
    } else {
        s_target_name[0] = '\0';
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return false;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    if (mode == BLE_MODE_SETUP && !register_clock_service()) {
        nimble_port_deinit();
        return false;
    }

    ble_npl_event_init(&s_adv_retry_ev, adv_retry_cb, NULL);
    s_adv_retry_init = true;
    s_started = true;
    nimble_port_freertos_init(host_task);

    if (s_target_valid) {
        char addr_str[18];
        addr_to_str(&s_target_addr, addr_str, sizeof(addr_str));
        ESP_LOGI(TAG, "started (%s), remembered sensor %s (%s)",
                 mode == BLE_MODE_SETUP ? "setup" : "scan once", s_target_name, addr_str);
    } else {
        ESP_LOGI(TAG, "started (%s)", mode == BLE_MODE_SETUP ? "setup" : "scan once");
    }
    return true;
}

bool ble_wait_reading(reading_t *out, uint32_t timeout_ms)
{
    if (!s_reading_q) return false;
    return xQueueReceive(s_reading_q, out, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

bool ble_take_clock_request(ble_clock_request_t *out)
{
    taskENTER_CRITICAL(&s_clock_lock);
    *out = s_clock_req;
    memset(&s_clock_req, 0, sizeof(s_clock_req));
    taskEXIT_CRITICAL(&s_clock_lock);
    return out->has_utc || out->has_tz;
}

bool ble_is_connected(void)
{
    return s_connected;
}

void ble_stop(void)
{
    if (!s_started) return;

    s_stopping = true;

    /* The host task switches the host on; nimble_port_stop() before that
       returns BLE_HS_EALREADY and would leave the stack running. */
    for (int waited = 0; !ble_hs_is_enabled() && waited < STOP_WAIT_ENABLED_MS; waited += 10) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    int rc = nimble_port_stop();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_stop failed: %d, skipping deinit", rc);
        return;
    }
    if (s_adv_retry_init) {
        ble_npl_eventq_remove(nimble_port_get_dflt_eventq(), &s_adv_retry_ev);
        ble_npl_event_deinit(&s_adv_retry_ev);
        s_adv_retry_init = false;
    }
    nimble_port_deinit();
    s_started = false;
    s_connected = false;
    ESP_LOGI(TAG, "stopped");
}
