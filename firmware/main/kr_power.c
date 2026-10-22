#include "kr_power.h"

#include <stdio.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_memory_utils.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "kr_ui.h"
#include "nimble/nimble_port.h"
#include "safe_state_sleep_model.h"

#define WAKE_MASK (1ULL << PIN_KEY_ENTER) /* Confirm key only. Not GPIO2 (CHRG), so it can power off with USB plugged in */

/* Cancel sleep: only reached with PA already low (the rails step confirmed it; in earlier steps PA is still
 * low + hold from early_init). GPIO17 is now digital output high + hold, which survives a soft reset. */
static void cancel_restart(const char *why)
{
    printf("[power] power-off cancelled: %s; restarting\n", why);
    gpio_deep_sleep_hold_dis();
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_restart();
}

/* ---- Before sleep, set each rail to its safe level and read each one back to confirm ---- */

static const struct {
    gpio_num_t  pin;
    int         level;
    const char *name;
} SLEEP_RAILS[] = {
    { PIN_PA_CTRL, 0, "PA(46)" },  { PIN_EPD_PWR, 0, "EPD(6)" },
    { PIN_NFC_PWR, 0, "NFC(21)" }, { PIN_LED, 1, "LED(3)=off" },
    { PIN_AVDD_EN, 0, "AVDD(42)" }, /* also powers the I2C pull-ups: turn off last */
};

static bool prepare_sleep_rails(void)
{
    if (gpio_get_level(PIN_PA_CTRL) != 0) {
        printf("[power] refused: PA(GPIO46) reads back high\n");
        return false;
    }
    /* Assert in esp_sleep_isolate_digital_gpio(): with deep-sleep digital hold enabled,
     * the task entering deep sleep must have its stack in internal RAM (sleep_gpio.c:193) */
    int stack_probe = 0;
    if (!esp_ptr_internal(&stack_probe)) {
        printf("[power] refused: task stack not in internal RAM\n");
        return false;
    }
    for (size_t i = 0; i < sizeof SLEEP_RAILS / sizeof SLEEP_RAILS[0]; i++) {
        gpio_hold_dis(SLEEP_RAILS[i].pin);
        gpio_set_level(SLEEP_RAILS[i].pin, SLEEP_RAILS[i].level);
        gpio_hold_en(SLEEP_RAILS[i].pin);
        if (gpio_get_level(SLEEP_RAILS[i].pin) != SLEEP_RAILS[i].level) {
            printf("[power] %s read back is not %d\n", SLEEP_RAILS[i].name, SLEEP_RAILS[i].level);
            return false;
        }
    }
    gpio_deep_sleep_hold_en(); /* hold on non-RTC pins (42/46) carries through deep sleep */
    return true;
}

/* ---- Hand GPIO17 over from digital hold to RTC hold ---- */

static esp_err_t vbat_rec_config(void)
{
    /* Never release hold: gpio_config calls rtc_gpio_deinit internally first (gpio.c:375-379) */
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_PWR_ON,
        .mode         = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

static bool vbat_exec(ss_sleep_op_t op)
{
    switch (op) {
    case SS_OP_RTC_SET_DIR:
        return rtc_gpio_set_direction(PIN_PWR_ON, RTC_GPIO_MODE_INPUT_OUTPUT) == ESP_OK;
    case SS_OP_RTC_PULLDOWN_DIS:
        return rtc_gpio_pulldown_dis(PIN_PWR_ON) == ESP_OK;
    case SS_OP_RTC_PULLUP_DIS:
        return rtc_gpio_pullup_dis(PIN_PWR_ON) == ESP_OK;
    case SS_OP_RTC_SET_LEVEL:
        return rtc_gpio_set_level(PIN_PWR_ON, 1) == ESP_OK;
    case SS_OP_RTC_INIT:
        return rtc_gpio_init(PIN_PWR_ON) == ESP_OK;
    case SS_OP_HOLD_DIS:
        return gpio_hold_dis(PIN_PWR_ON) == ESP_OK;
    case SS_OP_RTC_READ_PRE:
    case SS_OP_RTC_READ_POST:
        return rtc_gpio_get_level(PIN_PWR_ON) == 1;
    case SS_OP_RTC_HOLD_EN:
        return rtc_gpio_hold_en(PIN_PWR_ON) == ESP_OK;
    case SS_OP_REC_DIG_CONFIG:
        return vbat_rec_config() == ESP_OK;
    case SS_OP_REC_DIG_SET:
        return gpio_set_level(PIN_PWR_ON, 1) == ESP_OK;
    case SS_OP_REC_DIG_HOLD_EN:
        return gpio_hold_en(PIN_PWR_ON) == ESP_OK;
    case SS_OP_REC_DIG_READ:
        return gpio_get_level(PIN_PWR_ON) == 1;
    default:
        return false;
    }
}

static void print_trace(const ss_sleep_model_t *m)
{
    printf("[power] GPIO17 handoff trace (%s):", ss_vbat_result_name(m->result));
    for (int i = 0; i < m->n_trace; i++) {
        printf(" %s%s", ss_sleep_op_name(m->trace[i]), m->trace_ok[i] ? "" : "!");
    }
    printf("\n");
}

/* Neither high level can be confirmed: no restart, no deep sleep, no return (nothing would latch main power during a restart) */
static void fail_safe_park(const ss_sleep_model_t *m)
{
    print_trace(m);
    printf("\n⛔⛔ GPIO17 main power latch can't be confirmed high; fail-safe park. Plug in USB, then power on again\n\n");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        printf("[power] fail-safe parked\n");
    }
}

/* Returns true ⇒ RTC hold high is confirmed; the only next step is esp_deep_sleep_start() */
static bool vbat_rtc_hold(void)
{
    if (!rtc_gpio_is_valid_gpio(PIN_PWR_ON)) {
        return false; /* GPIO17 is still digital output high + hold */
    }
    ss_sleep_model_t m;
    ss_sleep_model_init(&m);
    while (!ss_sleep_model_done(&m)) {
        ss_sleep_op_t op = ss_sleep_model_next(&m);
        if (op == SS_OP_DONE) {
            break;
        }
        ss_sleep_model_feed(&m, vbat_exec(op));
    }
    if (ss_sleep_model_may_deep_sleep(&m)) {
        return true; /* no logging on the success path */
    }
    if (m.result == SS_VBAT_DIG_RECOVERED) {
        print_trace(&m);
        return false;
    }
    fail_safe_park(&m); /* does not return */
    return false;
}

/* ---- Orchestration ---- */

void kr_power_off(void)
{
    printf("[power] power off: stop BLE → screen → wait for confirm key release → rails → GPIO17 → deep sleep\n");

    /* 1. BLE: disconnect cleanly first (the Kindle sees the disconnect immediately instead of waiting for the supervision timeout), then stop the stack */
    struct ble_gap_conn_desc d;
    for (uint16_t h = 0; h < 4; h++) {
        if (ble_gap_conn_find(h, &d) == 0) {
            ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    ble_gap_adv_stop();
    vTaskDelay(pdMS_TO_TICKS(300));
    nimble_port_stop();

    /* 2. Power-off screen: full refresh, confirmed drawn, session parked and rail cut */
    if (!kr_ui_show_off_and_stop()) {
        cancel_restart("power-off screen not drawn / EPD not parked");
    }

    /* 3. Wait for the confirm key to be released (EXT1 ANY_LOW armed while it's held = wakes right after sleeping); up to 10 s */
    for (int i = 0; i < 1000 && gpio_get_level(PIN_KEY_ENTER) == 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (esp_sleep_enable_ext1_wakeup_io(WAKE_MASK, ESP_EXT1_WAKEUP_ANY_LOW) != ESP_OK) {
        cancel_restart("EXT1 config failed");
    }
    if (gpio_get_level(PIN_KEY_ENTER) == 0) { /* configure first, then read: closes the TOCTOU gap */
        cancel_restart("confirm key not released");
    }

    /* 4. rails (PA reading back low is the precondition check here) */
    if (!prepare_sleep_rails()) {
        cancel_restart("rails not at sleep-safe levels");
    }
    printf("[power] rails OK, handing off GPIO17\n");
    vTaskDelay(pdMS_TO_TICKS(20)); /* let the previous line drain out of the serial port */

    /* 5. GPIO17 → RTC hold high; the only action after this is deep sleep */
    if (!vbat_rtc_hold()) {
        cancel_restart("GPIO17 RTC handoff failed (confirmed back on digital hold high)");
    }
    esp_deep_sleep_start();
}
