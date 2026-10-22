#include "kr_batt.h"

#include <stdio.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"

#define TICK_MS   250
#define PERIOD_MS 60000
#define BURST_N   10

static battery_model_t           s_m;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;
static int64_t                   s_last_tick, s_last_burst = -PERIOD_MS;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

void kr_batt_init(void)
{
    gpio_config_t in = {
        .pin_bit_mask = (1ULL << PIN_CHRG_L) | (1ULL << PIN_STDBY_H),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&in);

    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    adc_cali_curve_fitting_config_t cf = {
        .unit_id = ADC_UNIT_1, .chan = ADC_BAT_CHANNEL,
        .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12,
    };
    bool ok = adc_oneshot_new_unit(&ucfg, &s_adc) == ESP_OK
           && adc_oneshot_config_channel(s_adc, ADC_BAT_CHANNEL, &ccfg) == ESP_OK
           && adc_cali_create_scheme_curve_fitting(&cf, &s_cali) == ESP_OK;
    if (!ok) {
        printf("[bat] ADC calibration unavailable: battery stays invalid\n");
        s_cali = NULL;
    }
    battery_model_init(&s_m, ok);
}

/* Counts only if all 10 raw reads + calibrated conversions succeed; mv is already ×2 (divider) */
static bool burst(uint16_t *mv_out)
{
    int64_t sum = 0;
    for (int i = 0; i < BURST_N; i++) {
        int raw = 0, v = 0;
        if (adc_oneshot_read(s_adc, ADC_BAT_CHANNEL, &raw) != ESP_OK
            || adc_cali_raw_to_voltage(s_cali, raw, &v) != ESP_OK) {
            return false;
        }
        sum += v;
    }
    int64_t mv = sum * 2 / BURST_N;
    if (mv < 0 || mv > 65535) {
        return false;
    }
    *mv_out = (uint16_t)mv;
    return true;
}

/* Don't sample while refreshing (rail on ∧ BUSY low): waveform current drags the reading down */
static bool gate_ok(void)
{
    return !(gpio_get_level(PIN_EPD_PWR) && gpio_get_level(PIN_EPD_BUSY) == 0);
}

void kr_batt_tick(void)
{
    int64_t now = now_ms();
    if (now - s_last_tick < TICK_MS) {
        return;
    }
    s_last_tick = now;
    battery_model_tick_lines(&s_m, gpio_get_level(PIN_CHRG_L), gpio_get_level(PIN_STDBY_H), now);
    if (!s_cali || !gate_ok()
        || !battery_model_sample_due(&s_m, now - s_last_burst >= PERIOD_MS, now)) {
        return;
    }
    uint16_t mv = 0;
    if (!burst(&mv)) {
        battery_model_burst_fail(&s_m, now);
    } else if (gate_ok()) {
        battery_model_burst_ok(&s_m, mv, now);
        s_last_burst = now;
    }
}

battery_snap_t kr_batt_snap(void)
{
    return s_m.snap;
}
