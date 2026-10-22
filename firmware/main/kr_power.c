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

#define WAKE_MASK (1ULL << PIN_KEY_ENTER) /* 只用决定键。不加 GPIO2(CHRG)：插着 USB 也能关机 */

/* 取消入睡：只在 PA 已低时走到这里（rails 步骤先确认过；更早的步骤 PA 一直是
 * early_init 的低 + hold）。GPIO17 此刻是数字输出高 + hold，带过软复位。 */
static void cancel_restart(const char *why)
{
    printf("[power] 电源オフ取消：%s —— 重启\n", why);
    gpio_deep_sleep_hold_dis();
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_restart();
}

/* ---- 睡前把各路电源设到安全电平并逐一读回确认 ---- */

static const struct {
    gpio_num_t  pin;
    int         level;
    const char *name;
} SLEEP_RAILS[] = {
    { PIN_PA_CTRL, 0, "PA(46)" },  { PIN_EPD_PWR, 0, "EPD(6)" },
    { PIN_NFC_PWR, 0, "NFC(21)" }, { PIN_LED, 1, "LED(3)=消灯" },
    { PIN_AVDD_EN, 0, "AVDD(42)" }, /* 同时给 I2C 上拉供电：最后关 */
};

static bool prepare_sleep_rails(void)
{
    if (gpio_get_level(PIN_PA_CTRL) != 0) {
        printf("[power] 拒绝：PA(GPIO46) 读回为高\n");
        return false;
    }
    /* esp_sleep_isolate_digital_gpio() 的 assert：开了 deep-sleep digital hold，
     * 调深睡的任务栈必须在内部 RAM（sleep_gpio.c:193） */
    int stack_probe = 0;
    if (!esp_ptr_internal(&stack_probe)) {
        printf("[power] 拒绝：任务栈不在内部 RAM\n");
        return false;
    }
    for (size_t i = 0; i < sizeof SLEEP_RAILS / sizeof SLEEP_RAILS[0]; i++) {
        gpio_hold_dis(SLEEP_RAILS[i].pin);
        gpio_set_level(SLEEP_RAILS[i].pin, SLEEP_RAILS[i].level);
        gpio_hold_en(SLEEP_RAILS[i].pin);
        if (gpio_get_level(SLEEP_RAILS[i].pin) != SLEEP_RAILS[i].level) {
            printf("[power] %s 读回不是 %d\n", SLEEP_RAILS[i].name, SLEEP_RAILS[i].level);
            return false;
        }
    }
    gpio_deep_sleep_hold_en(); /* 非 RTC 脚（42/46）的 hold 带过深睡 */
    return true;
}

/* ---- GPIO17 从数字 hold 交接到 RTC hold ---- */

static esp_err_t vbat_rec_config(void)
{
    /* 全程不松 hold：gpio_config 内部先 rtc_gpio_deinit（gpio.c:375-379） */
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
    printf("[power] GPIO17 交接痕迹（%s）:", ss_vbat_result_name(m->result));
    for (int i = 0; i < m->n_trace; i++) {
        printf(" %s%s", ss_sleep_op_name(m->trace[i]), m->trace_ok[i] ? "" : "!");
    }
    printf("\n");
}

/* 两种高电平都确认不了：不重启、不深睡、不返回（重启窗口里主电源无人锁存） */
static void fail_safe_park(const ss_sleep_model_t *m)
{
    print_trace(m);
    printf("\n⛔⛔ GPIO17 主电源锁存无法确认为高 —— fail-safe park，请插 USB 后重开\n\n");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        printf("[power] fail-safe park 中\n");
    }
}

/* 返回 true ⇒ RTC hold 高已确认，下一步只能是 esp_deep_sleep_start() */
static bool vbat_rtc_hold(void)
{
    if (!rtc_gpio_is_valid_gpio(PIN_PWR_ON)) {
        return false; /* GPIO17 仍是数字输出高 + hold */
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
        return true; /* 成功路上不打日志 */
    }
    if (m.result == SS_VBAT_DIG_RECOVERED) {
        print_trace(&m);
        return false;
    }
    fail_safe_park(&m); /* 不返回 */
    return false;
}

/* ---- 编排 ---- */

void kr_power_off(void)
{
    printf("[power] 电源オフ: BLE 停止 → 画面 → 決定キー離し待ち → rails → GPIO17 → deep sleep\n");

    /* 1. BLE：先体面断开（Kindle 侧立即看到断线，而不是等 supervision timeout），再停栈 */
    struct ble_gap_conn_desc d;
    for (uint16_t h = 0; h < 4; h++) {
        if (ble_gap_conn_find(h, &d) == 0) {
            ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    ble_gap_adv_stop();
    vTaskDelay(pdMS_TO_TICKS(300));
    nimble_port_stop();

    /* 2. 关机画面：全刷、确认画成、会话已 park 且 rail 已断 */
    if (!kr_ui_show_off_and_stop()) {
        cancel_restart("关机画面没画成 / EPD 没 park");
    }

    /* 3. 等决定键松开（按着就配 EXT1 ANY_LOW = 刚睡就醒）；最多 10 s */
    for (int i = 0; i < 1000 && gpio_get_level(PIN_KEY_ENTER) == 0; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (esp_sleep_enable_ext1_wakeup_io(WAKE_MASK, ESP_EXT1_WAKEUP_ANY_LOW) != ESP_OK) {
        cancel_restart("EXT1 配置失败");
    }
    if (gpio_get_level(PIN_KEY_ENTER) == 0) { /* 先配后读，关 TOCTOU */
        cancel_restart("决定键没松");
    }

    /* 4. rails（PA 读回为低是这里的前提检查） */
    if (!prepare_sleep_rails()) {
        cancel_restart("rails 没到睡眠安全电平");
    }
    printf("[power] rails OK，交接 GPIO17\n");
    vTaskDelay(pdMS_TO_TICKS(20)); /* 让上一行出串口 */

    /* 5. GPIO17 → RTC hold 高；之后唯一的动作是深睡 */
    if (!vbat_rtc_hold()) {
        cancel_restart("GPIO17 RTC 交接失败（已确认回到数字 hold 高）");
    }
    esp_deep_sleep_start();
}
