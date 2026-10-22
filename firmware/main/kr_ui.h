/* E-paper status screens (Japanese). Runs in its own task: one refresh takes about 1 s and must not block the key / BLE app loop.
 * Refreshes only when the screen would change; page turns themselves never refresh it (partial refresh is 0.5 s + ghosting). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    KR_VIEW_PAIRING, /* no bond, not connected */
    KR_VIEW_WAITING, /* bonded, not connected (Kindle asleep / out of range) */
    KR_VIEW_READY,   /* connected and ready */
} kr_view_t;

typedef struct {
    kr_view_t view;
    bool      bat_visible;
    bool      charging;
    uint8_t   bat_seg; /* 0..4 */
} kr_ui_state_t;

void kr_ui_start(void);
/* Safe to call every app loop; an unchanged state doesn't trigger a refresh */
void kr_ui_post(const kr_ui_state_t *s);

/* For power off: the UI task draws "電源オフ" with a full refresh → closes the session (park + cut rail) → never refreshes again.
 * Blocks until done (≤10 s). true = drawn and epd_session_close == ESP_OK. */
bool kr_ui_show_off_and_stop(void);
