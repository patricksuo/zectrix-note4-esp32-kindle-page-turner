/* ZECTRIX NOTE4 当 Kindle Click 翻页器用的固件。说明见 ../../README.md。
 *
 * ⚠️ app_main() 的前两件事顺序不可改：
 *   1. 主电源自锁 GPIO17 —— 否则松开开机键即断电
 *   2. 功放 GPIO46 拉低并 hold —— 本固件从不给 AVDD 上电，但同样先压住
 * 本固件不写 content 分区、不擦 nvs。 */
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board_pins.h"
#include "kr_batt.h"
#include "kr_ble.h"
#include "kr_power.h"
#include "kr_ui.h"
#include "safe_state.h"
#include "nvs_flash.h"

#define LOOP_MS          5
#define DEBOUNCE_MS      15
#define MIN_PRESS_MS     30
#define HOLD_TEST_MS     2000  /* 串口 U/D/F：按住 2 s */
#define CHORD_WINDOW_MS  50    /* 上下键在这段时间内先后按下 = 组合键，不翻页 */
#define SLEEP_HOLD_MS    1000  /* 上 + 下一起按住 = 让 Kindle 休眠（同官方「两侧键长按」） */
#define POWER_OFF_HOLD_MS 5000 /* 确认键单独按住 = 本机关机（深睡，按确认键开机） */

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ======================== 安全上电 ======================== */

static void config_output_hold(gpio_num_t pin, int level)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT_OUTPUT, /* 纯 OUTPUT 读回恒 0 */
    };
    gpio_hold_dis(pin);
    gpio_config(&cfg);
    gpio_set_level(pin, level);
    gpio_hold_en(pin);
}

esp_err_t safe_state_set_epd_power(bool on)
{
    gpio_hold_dis(PIN_EPD_PWR);
    gpio_set_level(PIN_EPD_PWR, on ? 1 : 0);
    gpio_hold_en(PIN_EPD_PWR);
    return ESP_OK;
}

static void safe_early_init(void)
{
    /* GPIO17：hold 还锁着时先 config，再摆 1，再松 hold、重新 hold
     * ——任一瞬间 pad 都有人驱高或被 hold 锁着高（深睡唤醒时数字输出寄存器是 0） */
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_PWR_ON,
        .mode = GPIO_MODE_INPUT_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_set_level(PIN_PWR_ON, 1);
    gpio_hold_dis(PIN_PWR_ON);
    gpio_set_level(PIN_PWR_ON, 1);
    gpio_hold_en(PIN_PWR_ON);
    if (gpio_get_level(PIN_PWR_ON) != 1) {
        config_output_hold(PIN_PWR_ON, 1);
    }

    gpio_deep_sleep_hold_dis();
    config_output_hold(PIN_PA_CTRL, 0); /* 先于一切 AVDD 动作 */
    config_output_hold(PIN_AVDD_EN, 0);
    config_output_hold(PIN_EPD_PWR, 0);
    config_output_hold(PIN_NFC_PWR, 0);

    gpio_config_t led = { .pin_bit_mask = 1ULL << PIN_LED, .mode = GPIO_MODE_INPUT_OUTPUT };
    gpio_hold_dis(PIN_LED);
    gpio_config(&led);
    gpio_set_level(PIN_LED, 1);

    /* 板上有外部上拉，不开内部上下拉 */
    gpio_config_t keys = {
        .pin_bit_mask = (1ULL << PIN_KEY_ENTER) | (1ULL << PIN_KEY_PGUP) | (1ULL << PIN_KEY_PGDN),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&keys);
}

/* ======================== 按键 → HID 报告 ======================== */
/* 报告带完整按键状态，重发最新一份总是对的；发不出去就下一轮重试（同 zuyan9）。 */

typedef enum { K_NEXT, K_PREV, K_BRUP, K_BRDN, K_POWER, K_N, K_NONE = -1 } key_t_;

static const struct { const char *name; uint16_t usage; bool consumer; } KEYS[K_N] = {
    [K_NEXT]  = { "page forward",    0x4E,   false }, /* KEY_PAGEDOWN */
    [K_PREV]  = { "page back",       0x4B,   false }, /* KEY_PAGEUP */
    [K_BRUP]  = { "brightness up",   0x006F, true },
    [K_BRDN]  = { "brightness down", 0x0070, true },
    [K_POWER] = { "sleep",           0x0030, true },  /* KEY_POWER，只发一次 */
};

static bool     s_down[K_N], s_release_wanted[K_N];
static uint32_t s_down_at[K_N], s_auto_release_at[K_N];
static bool     s_dirty;
static uint32_t s_flash_at;

static bool send_state(void)
{
    uint8_t r[KR_REPORT_LEN] = { 0 };
    int n = 2;
    uint16_t usage = 0;
    for (int k = 0; k < K_N; k++) {
        if (!s_down[k]) { continue; }
        if (KEYS[k].consumer) { usage = KEYS[k].usage; }
        else if (n < 8) { r[n++] = (uint8_t)KEYS[k].usage; }
    }
    r[8] = usage & 0xFF;
    r[9] = usage >> 8;
    return kr_ble_send_report(r);
}

static void flush_reports(void)
{
    if (!kr_ble_connected()) {
        s_dirty = false;
        return;
    }
    if (s_dirty && send_state()) {
        s_dirty = false;
    }
}

static void clear_keys(void)
{
    for (int k = 0; k < K_N; k++) {
        s_down[k] = s_release_wanted[k] = false;
        s_auto_release_at[k] = 0;
    }
    s_dirty = false;
}

static void key_down(key_t_ k)
{
    if (!kr_ble_ready()) { /* Kindle 还没就绪：直接丢掉，不排队补发 */
        printf("[key] %s ignored: not ready\n", KEYS[k].name);
        return;
    }
    if (s_down[k]) {                  /* 释放还没发出去又按了：继续按着 */
        s_release_wanted[k] = false;
        return;
    }
    s_down[k] = true;
    s_down_at[k] = now_ms();
    s_release_wanted[k] = false;
    s_dirty = true;
    flush_reports();
    s_flash_at = now_ms();
    printf("[key] %s down\n", KEYS[k].name);
}

static void key_up(key_t_ k)
{
    if (s_down[k]) { s_release_wanted[k] = true; }
}

static void tap(key_t_ k)
{
    key_down(k);
    key_up(k);
}

static void hold(key_t_ k, uint32_t ms)
{
    key_down(k);
    if (s_down[k]) { s_auto_release_at[k] = now_ms() + ms; }
}

static void service_releases(void)
{
    uint32_t now = now_ms();
    for (int k = 0; k < K_N; k++) {
        if (s_down[k] && s_auto_release_at[k] && (int32_t)(now - s_auto_release_at[k]) >= 0) {
            s_auto_release_at[k] = 0;
            s_release_wanted[k] = true;
        }
        if (s_down[k] && s_release_wanted[k] && !s_auto_release_at[k] &&
            now - s_down_at[k] >= MIN_PRESS_MS) {
            s_down[k] = s_release_wanted[k] = false;
            s_dirty = true;
            printf("[key] %s up\n", KEYS[k].name);
        }
    }
    flush_reports();
}

/* 休眠：先松开亮度键（否则 Kindle 会继续调亮度），再发一次 KEY_POWER */
static void sleep_action(void)
{
    if (!kr_ble_ready()) {
        printf("[key] sleep ignored: not ready\n");
        return;
    }
    bool any = false;
    for (int k = 0; k < K_N; k++) { any |= s_down[k]; }
    clear_keys();
    if (any) {
        s_dirty = true;
        flush_reports();
    }
    tap(K_POWER);
}

/* ======================== 机身按键 ======================== */

typedef struct { gpio_num_t pin; bool pressed; uint32_t changed_at; } btn_t;
static btn_t s_btn_up = { .pin = PIN_KEY_PGUP }, s_btn_dn = { .pin = PIN_KEY_PGDN },
             s_btn_ok = { .pin = PIN_KEY_ENTER };

/* 上下键的状态：
 *   IDLE   都没按
 *   WAIT   按下了一个，等 CHORD_WINDOW_MS 看另一个会不会跟上
 *   SINGLE 单键翻页，松开时发释放
 *   CHORD  上下一起按着：不翻页，按满 SLEEP_HOLD_MS 让 Kindle 休眠一次，两个都松开才回 IDLE */
typedef enum { PG_IDLE, PG_WAIT, PG_SINGLE, PG_CHORD } pg_state_t;
static pg_state_t s_pg = PG_IDLE;
static key_t_     s_pg_key;
static btn_t     *s_pg_btn;
static uint32_t   s_pg_since;
static bool       s_chord_fired;
/* 确认键 + 上/下 = 亮度（按住连续调）。这次确认键按下期间用过组合，松开 / 按满 5 s 都不算单按 */
static bool       s_up_bright, s_dn_bright, s_ok_combo;

/* +1 按下沿，-1 松开沿，0 无变化（低有效、去抖） */
static int read_btn(btn_t *b)
{
    bool raw = gpio_get_level(b->pin) == 0;
    uint32_t now = now_ms();
    if (raw != b->pressed && now - b->changed_at > DEBOUNCE_MS) {
        b->pressed = raw;
        b->changed_at = now;
        return raw ? 1 : -1;
    }
    return 0;
}

static void poll_buttons(void)
{
    int eu = read_btn(&s_btn_up);
    int ed = read_btn(&s_btn_dn);
    int eo = read_btn(&s_btn_ok);
    uint32_t now = now_ms();

    if (eo == 1) { s_ok_combo = false; }

    /* 确认键按着时，上/下 = 亮度 ±，松开上/下才释放（与确认键先松开无关） */
    if (s_pg == PG_IDLE && s_btn_ok.pressed) {
        if (eu == 1) { key_down(K_BRUP); s_up_bright = s_ok_combo = true; eu = 0; }
        if (ed == 1) { key_down(K_BRDN); s_dn_bright = s_ok_combo = true; ed = 0; }
    }
    if (eu == -1 && s_up_bright) { key_up(K_BRUP); s_up_bright = false; eu = 0; }
    if (ed == -1 && s_dn_bright) { key_up(K_BRDN); s_dn_bright = false; ed = 0; }

    switch (s_pg) {
    case PG_IDLE:
        if (eu == 1 && ed == 1) {
            s_pg = PG_CHORD; s_pg_since = now; s_chord_fired = false;
        } else if (eu == 1 || ed == 1) {
            s_pg = PG_WAIT; s_pg_since = now;
            s_pg_key = eu == 1 ? K_PREV : K_NEXT;
            s_pg_btn = eu == 1 ? &s_btn_up : &s_btn_dn;
        }
        break;
    case PG_WAIT: {
        int other = s_pg_btn == &s_btn_up ? ed : eu;
        int own   = s_pg_btn == &s_btn_up ? eu : ed;
        if (other == 1) {
            s_pg = PG_CHORD; s_pg_since = now; s_chord_fired = false;
        } else if (own == -1) {                 /* 不到 50 ms 就松开：照样翻一页 */
            tap(s_pg_key);
            s_pg = PG_IDLE;
        } else if (now - s_pg_since >= CHORD_WINDOW_MS) {
            key_down(s_pg_key);
            s_pg = PG_SINGLE;
        }
        break;
    }
    case PG_SINGLE:
        if (!s_pg_btn->pressed) {
            key_up(s_pg_key);
            s_pg = PG_IDLE;
        }
        break;
    case PG_CHORD:
        if (s_btn_up.pressed && s_btn_dn.pressed && !s_chord_fired
            && now - s_pg_since >= SLEEP_HOLD_MS) {
            s_chord_fired = true;
            sleep_action();
        }
        if (!s_btn_up.pressed && !s_btn_dn.pressed) { s_pg = PG_IDLE; }
        break;
    }

    /* 确认键单独按住 5 s = 本机关机 */
    if (s_btn_ok.pressed && !s_ok_combo && !s_up_bright && !s_dn_bright
        && now - s_btn_ok.changed_at >= POWER_OFF_HOLD_MS) {
        kr_power_off(); /* 成功不返回 */
    }
    if (eo == -1 && !s_ok_combo) {
        printf("[key] OK: + up/down = brightness, hold 5 s = power off\n");
    }
}

/* ======================== 电量 → Kindle / 画面 ======================== */

static bool s_batt_override; /* 串口 0-9/B 测低电提示用；R 回到实测 */
static int  s_batt_sent_pct = -1;
static kr_view_t s_view = KR_VIEW_PAIRING;

static void service_battery_and_ui(void)
{
    kr_batt_tick();
    battery_snap_t b = kr_batt_snap();

    /* 告诉 Kindle 的百分比：放电中用滤波后的 pct；充电中放电环不更新，
     * 从充电读数减去 BM_CHG_COMP_MV 的保守补偿来估。还没有有效读数就不发（不拿占位值冒充电量）。 */
    if (!s_batt_override && b.valid) {
        int pct = b.charging ? battery_model_pct(b.mv_diag > BM_CHG_COMP_MV
                                                     ? b.mv_diag - BM_CHG_COMP_MV : 0)
                             : b.pct;
        if (pct != s_batt_sent_pct) {
            s_batt_sent_pct = pct;
            kr_ble_set_battery((uint8_t)pct);
        }
    }

    /* 连上但还没就绪（Kindle 正在装遥控器，约 2 s）不换画面 */
    if (kr_ble_ready()) { s_view = KR_VIEW_READY; }
    else if (!kr_ble_connected()) { s_view = kr_ble_has_bonds() ? KR_VIEW_WAITING : KR_VIEW_PAIRING; }
    kr_ui_state_t st = {
        .view = s_view, .bat_visible = b.valid, .charging = b.charging, .bat_seg = b.seg,
    };
    kr_ui_post(&st);
}

/* ======================== 串口命令 ======================== */

static void print_help(void)
{
    printf("commands: f/b page fwd/back | u/d brightness | U/D/F hold 2 s | s sleep\n"
           "          0-9 battery 0..90%%, B 100%% (test override) | R real battery\n"
           "          c clear bonds | P power off (deep sleep) | ? status | h help\n"
           "buttons:  down = page forward, up = page back, OK + up/down = brightness,\n"
           "          up + down held 1 s = Kindle sleep, OK held 5 s = power off (OK to wake)\n");
}

static void handle_serial(void)
{
    uint8_t buf[32];
    int n = usb_serial_jtag_read_bytes(buf, sizeof buf, 0);
    for (int i = 0; i < n; i++) {
        char ch = (char)buf[i];
        switch (ch) {
        case 'f': tap(K_NEXT); break;
        case 'b': tap(K_PREV); break;
        case 'u': tap(K_BRUP); break;
        case 'd': tap(K_BRDN); break;
        case 'F': hold(K_NEXT, HOLD_TEST_MS); break;
        case 'U': hold(K_BRUP, HOLD_TEST_MS); break;
        case 'D': hold(K_BRDN, HOLD_TEST_MS); break;
        case 's': sleep_action(); break;
        case 'B': s_batt_override = true; kr_ble_set_battery(100); break;
        case 'R': s_batt_override = false; s_batt_sent_pct = -1; break;
        case 'P': kr_power_off(); break;
        case 'c': clear_keys(); kr_ble_clear_bonds(); break;
        case '?': kr_ble_print_status(); break;
        case 'h': print_help(); break;
        case '\r': case '\n': case ' ': break;
        default:
            if (ch >= '0' && ch <= '9') {
                s_batt_override = true;
                kr_ble_set_battery((uint8_t)((ch - '0') * 10));
            }
            break;
        }
    }
}

/* ======================== LED ======================== */
/* 未连接：1 Hz 短闪｜已连接未就绪：5 Hz｜就绪：常亮，发键时灭 60 ms */
static void service_led(void)
{
    uint32_t t = now_ms();
    bool on;
    if (!kr_ble_connected()) { on = (t % 1000) < 80; }
    else if (!kr_ble_ready()) { on = (t % 200) < 100; }
    else { on = t - s_flash_at >= 60; }
    gpio_set_level(PIN_LED, on ? 0 : 1);
}

void app_main(void)
{
    safe_early_init();

    /* 串口：只装 driver、不切 vfs（vfs 的阻塞写在没接电脑时会卡死） */
    usb_serial_jtag_driver_config_t ucfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_serial_jtag_driver_install(&ucfg);
    vTaskDelay(pdMS_TO_TICKS(600));

    printf("\n=== NOTE4 · Kindle Click remote ===\n");

    /* 配对记录存这里。失败只记录、绝不擦除：代价是配对记录跨不过复位 */
    esp_err_t e = nvs_flash_init();
    if (e != ESP_OK) {
        printf("⚠️ nvs_flash_init: %s —— bonds will not persist (NOT erasing)\n", esp_err_to_name(e));
    }

    kr_batt_init();
    kr_ui_start();
    kr_ble_start();
    print_help();

    bool was_connected = false;
    while (1) {
        handle_serial();
        poll_buttons();
        service_releases();
        kr_ble_service();
        service_battery_and_ui();
        service_led();

        bool c = kr_ble_connected();
        if (was_connected && !c) { clear_keys(); }
        was_connected = c;

        vTaskDelay(pdMS_TO_TICKS(LOOP_MS));
    }
}
