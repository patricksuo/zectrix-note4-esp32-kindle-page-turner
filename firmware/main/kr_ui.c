#include "kr_ui.h"

#include <stdio.h>
#include <string.h>

#include "epd_session.h"
#include "fb.h"
#include "font.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

extern const font_t font_jp16, font_jp24;

/* Refresh immediately on entering Ready; other changes must hold this long first (connect / disconnect flapping while the Kindle reconnects isn't worth two refreshes) */
#define SETTLE_MS 2000
#define POLL_MS   100

#define MARGIN 12
#define BAR_H  34

static portMUX_TYPE   s_mux = portMUX_INITIALIZER_UNLOCKED;
static kr_ui_state_t  s_want;
static bool           s_have_want;
static volatile bool  s_off_req;
static SemaphoreHandle_t s_off_done;
static bool           s_off_ok;

static uint8_t        s_cur_bits[FB_BYTES], s_prev_bits[FB_BYTES];
static fb_t           s_cur, s_prev;
static epd_session_t  s_epd;

/* ---------------- Drawing ---------------- */

static int text(const font_t *f, int x, int baseline, const char *s)
{
    int pen = x;
    uint32_t cp;
    while (utf8_next(&s, &cp) == 1) {
        pen += font_draw_glyph(&s_cur, f, cp, pen, baseline, FB_BLACK);
    }
    return pen;
}

static int text_w(const font_t *f, const char *s)
{
    return font_measure(f, s, (int)strlen(s));
}

/* 4-segment battery icon, top right; when charging, "充電中" to its left */
static void battery_icon(const kr_ui_state_t *st)
{
    if (!st->bat_visible) {
        return;
    }
    const int bw = 34, bh = 16, x = FB_WIDTH - MARGIN - bw - 3, y = (BAR_H - bh) / 2;
    fb_rect(&s_cur, x, y, bw, bh, FB_BLACK);
    fb_rect(&s_cur, x + 1, y + 1, bw - 2, bh - 2, FB_BLACK);
    fb_fill_rect(&s_cur, x + bw, y + 5, 3, bh - 10, FB_BLACK);
    for (int i = 0; i < st->bat_seg && i < 4; i++) {
        fb_fill_rect(&s_cur, x + 4 + i * 7, y + 4, 5, bh - 8, FB_BLACK);
    }
    if (st->charging) {
        const char *c = "充電中";
        text(&font_jp16, x - 8 - text_w(&font_jp16, c), 23, c);
    }
}

static void top_bar(const kr_ui_state_t *st)
{
    static const char *const STATUS[] = {
        [KR_VIEW_PAIRING] = "未ペアリング",
        [KR_VIEW_WAITING] = "待機中",
        [KR_VIEW_READY]   = "接続中",
    };
    int x = text(&font_jp16, MARGIN, 23, "Kindle リモコン");
    text(&font_jp16, x + 20, 23, STATUS[st->view]);
    battery_icon(st);
    fb_fill_rect(&s_cur, MARGIN, BAR_H, FB_WIDTH - 2 * MARGIN, 2, FB_BLACK);
}

static void lines16(int y0, const char *const *ls, int n)
{
    for (int i = 0; i < n; i++) {
        text(&font_jp16, MARGIN + 8, y0 + i * 26, ls[i]);
    }
}

static void view_pairing(void)
{
    text(&font_jp24, MARGIN + 8, 82, "ペアリング待ち");
    static const char *const L[] = {
        "Kindle の設定で Bluetooth デバイスを",
        "スキャンして、「Kindle Click」を",
        "選んでください。",
        "",
        "スキャンは 8 秒で終わります。",
        "見つからなければ、もう一度スキャン。",
    };
    lines16(124, L, sizeof L / sizeof L[0]);
}

static void view_waiting(void)
{
    text(&font_jp24, MARGIN + 8, 82, "Kindle を待っています");
    static const char *const L[] = {
        "Kindle がスリープ中はつながりません。",
        "起こすと数秒で自動的につながります。",
    };
    lines16(124, L, sizeof L / sizeof L[0]);
}

static void view_ready(void)
{
    static const struct { const char *key, *what; } ROWS[] = {
        { "上",         "前のページ" },
        { "下",         "次のページ" },
        { "決定＋上/下", "明るさ" },
        { "上＋下 長押し", "スリープ" },
    };
    for (int i = 0; i < 4; i++) {
        int base = 80 + i * 44;
        text(&font_jp24, MARGIN + 8, base, ROWS[i].key);
        text(&font_jp24, MARGIN + 196, base, ROWS[i].what);
    }
    fb_hline(&s_cur, MARGIN, 230, FB_WIDTH - 2 * MARGIN, FB_BLACK);
    text(&font_jp16, MARGIN + 8, 256, "決定キー 5 秒長押しで電源オフ");
    text(&font_jp16, MARGIN + 8, 282, "リモコンでは Kindle を起こせません");
}

static void view_off(void)
{
    fb_clear(&s_cur, FB_WHITE);
    text(&font_jp16, MARGIN, 23, "Kindle リモコン");
    fb_fill_rect(&s_cur, MARGIN, BAR_H, FB_WIDTH - 2 * MARGIN, 2, FB_BLACK);
    const char *h = "電源オフ";
    text(&font_jp24, (FB_WIDTH - text_w(&font_jp24, h)) / 2, 150, h);
    const char *s = "決定キーを押すと起動します";
    text(&font_jp16, (FB_WIDTH - text_w(&font_jp16, s)) / 2, 196, s);
}

static void render(const kr_ui_state_t *st)
{
    fb_clear(&s_cur, FB_WHITE);
    top_bar(st);
    switch (st->view) {
    case KR_VIEW_PAIRING: view_pairing(); break;
    case KR_VIEW_WAITING: view_waiting(); break;
    case KR_VIEW_READY:   view_ready(); break;
    }
}

/* Boot self-test: print missing glyphs / overflow for every UI string (missing glyphs draw as hollow boxes, never silently skipped) */
static void check_strings(void)
{
    static const struct { const font_t *f; const char *s; } ALL[] = {
        { &font_jp16, "Kindle リモコン未ペアリング待機中接続中充電中決定キーを押すと起動します" },
        { &font_jp24, "電源オフ" },
        { &font_jp24, "ペアリング待ちKindle を待っています上下決定＋/前のページ次のページ明るさ長押しスリープ" },
        { &font_jp16, "決定キー 5 秒長押しで電源オフ" },
        { &font_jp16, "Kindle の設定で Bluetooth デバイスをスキャンして、「Kindle Click」を選んでください。"
                      "スキャンは 8 秒で終わります。見つからなければ、もう一度スキャン。"
                      "Kindle がスリープ中はつながりません。起こすと数秒で自動的につながります。"
                      "リモコンでは Kindle を起こせません" },
    };
    for (size_t i = 0; i < sizeof ALL / sizeof ALL[0]; i++) {
        int miss = font_count_missing(ALL[i].f, ALL[i].s);
        if (miss) {
            printf("[ui] ⚠️ %d glyphs missing in jp%u set %u\n", miss, ALL[i].f->px, (unsigned)i);
        }
    }
}

/* ---------------- Task ---------------- */

static bool same(const kr_ui_state_t *a, const kr_ui_state_t *b)
{
    return a->view == b->view && a->bat_visible == b->bat_visible
        && (!a->bat_visible || (a->charging == b->charging && a->bat_seg == b->bat_seg));
}

static void ui_task(void *arg)
{
    (void)arg;
    kr_ui_state_t drawn = { 0 }, pending = { 0 };
    bool have_drawn = false;
    TickType_t pending_since = 0;

    check_strings();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        if (s_off_req) {
            /* Full refresh: the power-off screen stays up for a long time, so leave no partial-refresh ghosting */
            view_off();
            int rc = EPD_ERR_ARG;
            esp_err_t close = ESP_FAIL;
            if (epd_session_open(&s_epd)) {
                rc = epd_session_refresh(&s_epd, &s_cur, &s_prev, true);
                close = epd_session_close(&s_epd);
            }
            printf("[ui] power-off screen rc=%d close=%s\n", rc, esp_err_to_name(close));
            s_off_ok = (rc == EPD_OK && close == ESP_OK);
            xSemaphoreGive(s_off_done);
            for (;;) {
                vTaskDelay(portMAX_DELAY);
            }
        }
        kr_ui_state_t want;
        taskENTER_CRITICAL(&s_mux);
        bool have = s_have_want;
        want = s_want;
        taskEXIT_CRITICAL(&s_mux);
        if (!have || (have_drawn && same(&want, &drawn))) {
            continue;
        }
        if (!same(&want, &pending)) {
            pending = want;
            pending_since = xTaskGetTickCount();
        }
        bool settled = xTaskGetTickCount() - pending_since >= pdMS_TO_TICKS(SETTLE_MS);
        if (have_drawn && want.view != KR_VIEW_READY && !settled) {
            continue;
        }

        render(&want);
        TickType_t t0 = xTaskGetTickCount();
        int rc = EPD_ERR_ARG;
        if (epd_session_open(&s_epd)) {
            rc = epd_session_refresh(&s_epd, &s_cur, &s_prev, !have_drawn);
            (void)epd_session_close(&s_epd);
        }
        printf("[ui] screen %d bat=%d/%u%s rc=%d %lu ms\n", want.view, want.bat_visible, want.bat_seg,
               want.charging ? " chg" : "", rc,
               (unsigned long)pdTICKS_TO_MS(xTaskGetTickCount() - t0));
        if (rc == EPD_OK) {
            drawn = want;
            have_drawn = true;
        } else {
            /* No retry loop on failure: try again on the next state change */
            drawn = want;
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
}

void kr_ui_start(void)
{
    fb_init(&s_cur, s_cur_bits);
    fb_init(&s_prev, s_prev_bits);
    fb_clear(&s_prev, FB_WHITE);
    s_off_done = xSemaphoreCreateBinary();
    xTaskCreate(ui_task, "kr_ui", 4096, NULL, 3, NULL);
}

void kr_ui_post(const kr_ui_state_t *s)
{
    taskENTER_CRITICAL(&s_mux);
    s_want = *s;
    s_have_want = true;
    taskEXIT_CRITICAL(&s_mux);
}

bool kr_ui_show_off_and_stop(void)
{
    s_off_req = true;
    /* One refresh takes about 1 s; an in-flight refresh plus the full power-off refresh is still well under 10 s */
    if (xSemaphoreTake(s_off_done, pdMS_TO_TICKS(10000)) != pdTRUE) {
        return false;
    }
    return s_off_ok;
}
