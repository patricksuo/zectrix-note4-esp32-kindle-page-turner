/* E-paper session: open (rail on, init, read temperature) / refresh (full every 4th partial) / close (park pins, rail off; image stays). */
#include "epd_session.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "safe_state.h"
#define BOOSTER_FALLBACK 241
static esp_err_t park_then_rail_off(void)
{
    esp_err_t e = epd_spi_prepare_power_off();
    if (e != ESP_OK) {
        printf("[epd] SPI park 失败 err=%s —— rail 保持供电"
               "（引脚状态未知时断电会向断电面板反向灌电）\n",
               esp_err_to_name(e));
        return e;
    }
    safe_state_set_epd_power(false);
    return ESP_OK;
}
bool epd_session_open(epd_session_t *s)
{
    if (s->on) {
        return true;
    }
    safe_state_set_epd_power(true);
    vTaskDelay(pdMS_TO_TICKS(100));
    if (epd_spi_init(&s->io) != ESP_OK) {
        (void)park_then_rail_off();
        return false;
    }
    if (epd_init(&s->io) != EPD_OK) {
        (void)park_then_rail_off();
        return false;
    }
    int celsius = 0;
    if (epd_read_temperature(&s->io, &celsius) == EPD_OK && celsius > -20
        && celsius < 80) {
        s->booster = epd_temp_to_boost(celsius);
    } else {
        s->booster = BOOSTER_FALLBACK;
        printf("[epd] 读温度失败，booster 显式降级 =%u\n", s->booster);
    }
    s->on = true;
    return true;
}
esp_err_t epd_session_close(epd_session_t *s)
{
    if (!s->on) {
        return ESP_OK;
    }
    esp_err_t park = park_then_rail_off();
    if (park != ESP_OK) {
        esp_err_t re = epd_spi_init(&s->io);
        if (re == ESP_OK) {
            printf("[epd] park 失败后已恢复会话（rail 仍供电）\n");
            return park;
        }
        printf("[epd] park 失败且恢复 SPI 也失败 err=%s —— rail 保持供电，"
               "引脚状态未知，会话作废\n",
               esp_err_to_name(re));
        s->on = false;
        return EPD_SESSION_ERR_PARK_STUCK;
    }
    s->on = false;
    return ESP_OK;
}
int epd_session_refresh(epd_session_t *s, fb_t *cur, fb_t *prev, bool force_full)
{
    if (!s->on) {
        return EPD_ERR_ARG;
    }
    bool do_full = force_full || prev == NULL || s->partials >= EPD_SESSION_FULL_EVERY;
    int rc = epd_init(&s->io);
    if (rc == EPD_OK) {
        rc = do_full ? epd_display_full(&s->io, cur, s->booster)
                     : epd_display_partial(&s->io, prev, cur);
    }
    if (rc != EPD_OK) {
        return rc;
    }
    s->partials = do_full ? 0 : s->partials + 1;
    if (prev != NULL) {
        memcpy(prev->bits, cur->bits, FB_BYTES);
    }
    return EPD_OK;
}
