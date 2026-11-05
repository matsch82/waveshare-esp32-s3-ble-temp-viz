#include "battery.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

#define TAG "battery"

#define BATTERY_ADC_UNIT     ADC_UNIT_1
#define BATTERY_ADC_CHANNEL  ADC_CHANNEL_3   /* GPIO4 */
#define BATTERY_DIVIDER      2               /* 200k / 200k */
#define BATTERY_SAMPLES      8

int battery_read_mv(void)
{
    adc_oneshot_unit_handle_t unit = NULL;
    adc_cali_handle_t cali = NULL;
    int result = -1;

    const adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = BATTERY_ADC_UNIT };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &unit);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC unit init failed: %s", esp_err_to_name(err));
        return -1;
    }

    const adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = BATTERY_ADC_UNIT,
        .chan = BATTERY_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(unit, BATTERY_ADC_CHANNEL, &chan_cfg);
    if (err == ESP_OK) {
        err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali);
    }

    if (err == ESP_OK) {
        int sum = 0;
        int n = 0;
        for (int i = 0; i < BATTERY_SAMPLES; i++) {
            int mv = 0;
            if (adc_oneshot_get_calibrated_result(unit, cali, BATTERY_ADC_CHANNEL, &mv) == ESP_OK) {
                sum += mv;
                n++;
            }
        }
        if (n > 0) {
            result = sum / n * BATTERY_DIVIDER;
        }
    }
    if (result < 0) {
        ESP_LOGW(TAG, "battery read failed: %s", esp_err_to_name(err));
    }

    if (cali) adc_cali_delete_scheme_curve_fitting(cali);
    adc_oneshot_del_unit(unit);
    return result;
}
