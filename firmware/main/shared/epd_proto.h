/* SSD2683 e-paper protocol (platform independent; I/O injected via epd_io_t). BUSY is active-low. Partial refresh still sends the whole 400x300 frame. */
#ifndef EPD_PROTO_H
#define EPD_PROTO_H 
#include <stddef.h>
#include <stdint.h>
#include "fb.h"
#define EPD_OK 0
#define EPD_ERR_BUSY_TIMEOUT (-1)
#define EPD_ERR_IO (-2)
#define EPD_ERR_ARG (-3)
#define EPD_ERR_POF (-4)
#define EPD_BUSY_TIMEOUT_MS 5000
typedef struct {
    int (*cmd)(void *ctx, uint8_t c);
    int (*data)(void *ctx, const uint8_t *buf, size_t len);
    int (*busy_wait)(void *ctx, int timeout_ms, const char *label);
    int (*power)(void *ctx, int on);
    int (*reset)(void *ctx);
    void (*delay_ms)(void *ctx, int ms);
    int (*read_temp)(void *ctx, int *out_celsius);
    void *ctx;
} epd_io_t;
void epd_pack_row(const uint8_t *prev, const uint8_t *now, uint8_t *out, int bytes_1bpp);
uint8_t epd_temp_to_boost(int celsius);
int epd_init(const epd_io_t *io);
void epd_set_psr_b1(uint8_t v);
uint8_t epd_psr_b1(void);
int epd_read_temperature(const epd_io_t *io, int *out_celsius);
int epd_power_on(const epd_io_t *io);
int epd_power_off(const epd_io_t *io);
int epd_display_full_nopwr(const epd_io_t *io, const fb_t *now, uint8_t booster);
int epd_display_partial_nopwr(const epd_io_t *io, const fb_t *prev, const fb_t *now);
int epd_display_partial_hold_first(const epd_io_t *io, const fb_t *prev, const fb_t *now);
int epd_display_full(const epd_io_t *io, const fb_t *now, uint8_t booster);
int epd_display_full_norm(const epd_io_t *io, const fb_t *now, uint8_t booster);
int epd_display_partial(const epd_io_t *io, const fb_t *prev, const fb_t *now);
typedef struct {
    uint16_t x, y, w, h;
} epd_rect_t;
int epd_display_partial_cfg(const epd_io_t *io, const fb_t *prev, const fb_t *now,
                            const epd_rect_t *r, int use_window, int fast_lut,
                            int hot, int keep_power);
int epd_diff_rect(const fb_t *prev, const fb_t *now, epd_rect_t *out);
int epd_load_fast_lut(const epd_io_t *io);
void epd_set_partial_temp_override(int celsius);
int epd_partial_temp_override(void);
int epd_display_partial_window(const epd_io_t *io, const fb_t *prev, const fb_t *now,
                               const epd_rect_t *r, int hot);
int epd_ram_normalize(const epd_io_t *io, const fb_t *now, const epd_rect_t *r);
#endif
