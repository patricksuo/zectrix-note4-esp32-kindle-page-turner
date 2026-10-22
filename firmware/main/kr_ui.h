/* 墨水屏状态画面（日文）。自己一个任务：刷新一次约 1 s，不能卡住按键 / BLE 的 app 循环。
 * 只在「画面会变」时刷——翻页本身从不刷屏（局刷 0.5 s + 残影）。 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    KR_VIEW_PAIRING, /* 无绑定、未连接 */
    KR_VIEW_WAITING, /* 有绑定、未连接（Kindle 睡着 / 不在旁边） */
    KR_VIEW_READY,   /* 已连接且就绪 */
} kr_view_t;

typedef struct {
    kr_view_t view;
    bool      bat_visible;
    bool      charging;
    uint8_t   bat_seg; /* 0..4 */
} kr_ui_state_t;

void kr_ui_start(void);
/* app 循环每轮都可以调；相同状态不会触发刷新 */
void kr_ui_post(const kr_ui_state_t *s);

/* 关机用：UI 任务画「電源オフ」全刷 → 关会话（park + 断 rail）→ 之后永不再刷。
 * 阻塞到完成（≤10 s）。true = 画成且 epd_session_close == ESP_OK。 */
bool kr_ui_show_off_and_stop(void);
