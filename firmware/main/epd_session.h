/* E-paper session: open (rail on, init, read temperature) / refresh (full every 4th partial) / close (park pins, rail off; image stays). */
#ifndef EPD_SESSION_H
#define EPD_SESSION_H 
#include <stdbool.h>
#include "epd_proto.h"
#include "epd_spi.h"
#include "esp_err.h"
#include "fb.h"
#define EPD_SESSION_FULL_EVERY 4
typedef struct {
    epd_io_t io;
    bool on;
    uint8_t booster;
    int partials;
} epd_session_t;
#define EPD_SESSION_ERR_PARK_STUCK ESP_ERR_NOT_FINISHED
bool epd_session_open(epd_session_t *s);
esp_err_t epd_session_close(epd_session_t *s);
int epd_session_refresh(epd_session_t *s, fb_t *cur, fb_t *prev, bool force_full);
#endif
