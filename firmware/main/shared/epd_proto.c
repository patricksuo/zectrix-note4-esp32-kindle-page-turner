/* SSD2683 e-paper protocol (platform independent; I/O injected via epd_io_t). BUSY is active-low. */
#include "epd_proto.h"
#include <string.h>
#define CMD_PSR 0x00
#define CMD_POF 0x02
#define CMD_PON 0x04
#define CMD_DTM 0x10
#define CMD_DRF 0x12
#define CMD_TEMP_RD 0x40
#define CMD_CCSET 0xE0
#define CMD_TSSET 0xE6
#define CMD_UNKNOWN_E9 0xE9
#define CMD_LUT_ACTIVATE 0xA5
#define CMD_VCOM 0x50
#define CMD_PART_WIN 0x83
#define CHECK(expr) \
    do { \
        int _rc = (expr); \
        if (_rc != EPD_OK) { return _rc; } \
    } while (0)
static int wait_busy(const epd_io_t *io, const char *label)
{
    return io->busy_wait(io->ctx, EPD_BUSY_TIMEOUT_MS, label);
}
static int cmd0(const epd_io_t *io, uint8_t c)
{
    return io->cmd(io->ctx, c) == EPD_OK ? EPD_OK : EPD_ERR_IO;
}
static int cmd1(const epd_io_t *io, uint8_t c, uint8_t d)
{
    CHECK(cmd0(io, c));
    return io->data(io->ctx, &d, 1) == EPD_OK ? EPD_OK : EPD_ERR_IO;
}
void epd_pack_row(const uint8_t *prev, const uint8_t *now, uint8_t *out, int bytes_1bpp)
{
    for (int j = 0; j < bytes_1bpp; j++) {
        uint8_t b_prev = prev ? prev[j] : 0x00;
        uint8_t b_now = now[j];
        uint16_t result = 0;
        for (int k = 0; k < 8; k++) {
            int src_bit = 7 - k;
            result |= (uint16_t)(((b_prev >> src_bit) & 1u) << (2 * src_bit + 1));
            result |= (uint16_t)(((b_now >> src_bit) & 1u) << (2 * src_bit));
        }
        out[2 * j + 0] = (uint8_t)(result >> 8);
        out[2 * j + 1] = (uint8_t)(result & 0xFF);
    }
}
uint8_t epd_temp_to_boost(int celsius)
{
    if (celsius <= 5) return 232;
    if (celsius <= 10) return 235;
    if (celsius <= 20) return 238;
    if (celsius <= 30) return 241;
    if (celsius <= 127) return 244;
    return 232;
}
#ifndef EPD_PSR_B1_DEFAULT
#define EPD_PSR_B1_DEFAULT 0x2E
#endif
static uint8_t s_psr_b1 = EPD_PSR_B1_DEFAULT;
void epd_set_psr_b1(uint8_t v) { s_psr_b1 = v; }
uint8_t epd_psr_b1(void) { return s_psr_b1; }
int epd_init(const epd_io_t *io)
{
    if (!io || !io->cmd || !io->data || !io->busy_wait || !io->power
        || !io->reset || !io->delay_ms) {
        return EPD_ERR_ARG;
    }
    if (io->power(io->ctx, 1) != EPD_OK) { return EPD_ERR_IO; }
    io->delay_ms(io->ctx, 10);
    if (io->reset(io->ctx) != EPD_OK) { return EPD_ERR_IO; }
    CHECK(wait_busy(io, "init_reset"));
    CHECK(cmd0(io, CMD_PSR));
    const uint8_t psr[2] = { 0x2F, s_psr_b1 };
    if (io->data(io->ctx, psr, sizeof psr) != EPD_OK) { return EPD_ERR_IO; }
    CHECK(cmd1(io, CMD_UNKNOWN_E9, 0x01));
    CHECK(wait_busy(io, "init_e9"));
    return EPD_OK;
}
int epd_power_on(const epd_io_t *io)
{
    if (!io) { return EPD_ERR_ARG; }
    CHECK(cmd0(io, CMD_PON));
    return wait_busy(io, "power_on");
}
int epd_power_off(const epd_io_t *io)
{
    if (!io) { return EPD_ERR_ARG; }
    CHECK(cmd1(io, CMD_POF, 0x00));
    return wait_busy(io, "power_off");
}
static int trigger_refresh(const epd_io_t *io)
{
    CHECK(cmd1(io, CMD_DRF, 0x00));
    return wait_busy(io, "refresh");
}
static int turn_on_display(const epd_io_t *io)
{
    CHECK(epd_power_on(io));
    CHECK(trigger_refresh(io));
    return epd_power_off(io) == EPD_OK ? EPD_OK : EPD_ERR_POF;
}
int epd_read_temperature(const epd_io_t *io, int *out_celsius)
{
    if (!io || !out_celsius) { return EPD_ERR_ARG; }
    if (!io->read_temp) { return EPD_ERR_ARG; }
    CHECK(cmd0(io, CMD_TEMP_RD));
    CHECK(wait_busy(io, "temp_read"));
    return io->read_temp(io->ctx, out_celsius) == EPD_OK ? EPD_OK : EPD_ERR_IO;
}
static int apply_booster(const epd_io_t *io, uint8_t booster)
{
    CHECK(cmd1(io, CMD_CCSET, 0x02));
    CHECK(cmd1(io, CMD_TSSET, booster));
    CHECK(wait_busy(io, "temp_activate"));
    return EPD_OK;
}
static int send_frame(const epd_io_t *io, const fb_t *prev, const fb_t *now)
{
    uint8_t row[FB_STRIDE * 2];
    CHECK(cmd0(io, CMD_DTM));
    CHECK(wait_busy(io, "dtm"));
    for (int y = 0; y < now->h; y++) {
        const uint8_t *p = prev ? &prev->bits[(size_t)y * prev->stride] : NULL;
        const uint8_t *n = &now->bits[(size_t)y * now->stride];
        epd_pack_row(p, n, row, now->stride);
        if (io->data(io->ctx, row, (size_t)now->stride * 2) != EPD_OK) {
            return EPD_ERR_IO;
        }
    }
    return EPD_OK;
}
static int set_partial_window(const epd_io_t *io, const epd_rect_t *r);
static int send_frame_window(const epd_io_t *io, const fb_t *prev, const fb_t *now,
                             int x_byte, int nbytes, int y0, int h);
static int display_full_seq(const epd_io_t *io, const fb_t *now, uint8_t booster,
                            int norm)
{
    if (!io || !now || !now->bits) { return EPD_ERR_ARG; }
    CHECK(apply_booster(io, booster));
    CHECK(cmd0(io, CMD_LUT_ACTIVATE));
    CHECK(wait_busy(io, "lut_activate"));
    io->delay_ms(io->ctx, 10);
    CHECK(send_frame(io, NULL, now));
    if (!norm) {
        return turn_on_display(io);
    }
    CHECK(epd_power_on(io));
    CHECK(trigger_refresh(io));
    {
        epd_rect_t whole;
        whole.x = 0; whole.y = 0;
        whole.w = (uint16_t)now->w; whole.h = (uint16_t)now->h;
        CHECK(set_partial_window(io, &whole));
        CHECK(send_frame_window(io, now, now, 0, now->w / 8, 0, now->h));
    }
    return epd_power_off(io) == EPD_OK ? EPD_OK : EPD_ERR_POF;
}
int epd_display_full(const epd_io_t *io, const fb_t *now, uint8_t booster)
{
    return display_full_seq(io, now, booster, 0);
}
int epd_display_full_norm(const epd_io_t *io, const fb_t *now, uint8_t booster)
{
    return display_full_seq(io, now, booster, 1);
}
int epd_display_full_nopwr(const epd_io_t *io, const fb_t *now, uint8_t booster)
{
    if (!io || !now || !now->bits) { return EPD_ERR_ARG; }
    CHECK(apply_booster(io, booster));
    CHECK(cmd0(io, CMD_LUT_ACTIVATE));
    CHECK(wait_busy(io, "lut_activate"));
    io->delay_ms(io->ctx, 10);
    CHECK(send_frame(io, NULL, now));
    return trigger_refresh(io);
}
int epd_display_partial_nopwr(const epd_io_t *io, const fb_t *prev, const fb_t *now)
{
    if (!io || !now || !now->bits || !prev || !prev->bits) { return EPD_ERR_ARG; }
    CHECK(send_frame(io, prev, now));
    return trigger_refresh(io);
}
int epd_display_partial_hold_first(const epd_io_t *io, const fb_t *prev, const fb_t *now)
{
    if (!io || !now || !now->bits || !prev || !prev->bits) { return EPD_ERR_ARG; }
    CHECK(send_frame(io, prev, now));
    CHECK(epd_power_on(io));
    return trigger_refresh(io);
}
int epd_display_partial(const epd_io_t *io, const fb_t *prev, const fb_t *now)
{
    if (!io || !now || !now->bits || !prev || !prev->bits) { return EPD_ERR_ARG; }
    CHECK(send_frame(io, prev, now));
    return turn_on_display(io);
}
int epd_diff_rect(const fb_t *prev, const fb_t *now, epd_rect_t *out)
{
    int min_bx = -1, max_bx = -1, min_y = -1, max_y = -1;
    if (!prev || !now || !out || !prev->bits || !now->bits) { return 0; }
    if (prev->w != now->w || prev->h != now->h) { return 0; }
    for (int y = 0; y < now->h; y++) {
        const uint8_t *p = &prev->bits[(size_t)y * prev->stride];
        const uint8_t *n = &now->bits[(size_t)y * now->stride];
        for (int j = 0; j < now->stride; j++) {
            if (p[j] == n[j]) { continue; }
            if (min_bx < 0 || j < min_bx) { min_bx = j; }
            if (j > max_bx) { max_bx = j; }
            if (min_y < 0) { min_y = y; }
            max_y = y;
        }
    }
    if (min_bx < 0) { return 0; }
    out->x = (uint16_t)(min_bx * 8);
    out->w = (uint16_t)((max_bx - min_bx + 1) * 8);
    if (out->x + out->w > now->w) { out->w = (uint16_t)(now->w - out->x); }
    out->y = (uint16_t)min_y;
    out->h = (uint16_t)(max_y - min_y + 1);
    return 1;
}
static int s_partial_temp = -1;
void epd_set_partial_temp_override(int celsius) { s_partial_temp = celsius; }
int epd_partial_temp_override(void) { return s_partial_temp; }
int epd_load_fast_lut(const epd_io_t *io)
{
    if (!io) { return EPD_ERR_ARG; }
    CHECK(cmd1(io, CMD_VCOM, 0x77));
    if (s_partial_temp != -1) {
        CHECK(apply_booster(io, epd_temp_to_boost(s_partial_temp)));
    } else {
        CHECK(cmd1(io, CMD_CCSET, 0x00));
    }
    CHECK(cmd0(io, CMD_LUT_ACTIVATE));
    CHECK(wait_busy(io, "lut_fast"));
    io->delay_ms(io->ctx, 10);
    return EPD_OK;
}
static int set_partial_window(const epd_io_t *io, const epd_rect_t *r)
{
    uint8_t d[9];
    uint16_t x1 = (uint16_t)(r->x + r->w - 1);
    uint16_t y1 = (uint16_t)(r->y + r->h - 1);
    d[0] = (uint8_t)((r->x >> 8) & 0x03); d[1] = (uint8_t)(r->x & 0xFF);
    d[2] = (uint8_t)((x1 >> 8) & 0x03); d[3] = (uint8_t)(x1 & 0xFF);
    d[4] = (uint8_t)((r->y >> 8) & 0x03); d[5] = (uint8_t)(r->y & 0xFF);
    d[6] = (uint8_t)((y1 >> 8) & 0x03); d[7] = (uint8_t)(y1 & 0xFF);
    d[8] = 0x01;
    CHECK(cmd0(io, CMD_PART_WIN));
    return io->data(io->ctx, d, sizeof d) == EPD_OK ? EPD_OK : EPD_ERR_IO;
}
static int send_frame_window(const epd_io_t *io, const fb_t *prev, const fb_t *now,
                             int x_byte, int nbytes, int y0, int h)
{
    uint8_t row[FB_STRIDE * 2];
    CHECK(cmd0(io, CMD_DTM));
    CHECK(wait_busy(io, "dtm"));
    for (int y = y0; y < y0 + h; y++) {
        const uint8_t *p = &prev->bits[(size_t)y * prev->stride + x_byte];
        const uint8_t *n = &now->bits[(size_t)y * now->stride + x_byte];
        epd_pack_row(p, n, row, nbytes);
        if (io->data(io->ctx, row, (size_t)nbytes * 2) != EPD_OK) {
            return EPD_ERR_IO;
        }
    }
    return EPD_OK;
}
int epd_display_partial_window(const epd_io_t *io, const fb_t *prev, const fb_t *now,
                               const epd_rect_t *r, int hot)
{
    epd_rect_t full;
    int x_byte, nbytes;
    if (!io || !now || !now->bits || !prev || !prev->bits) { return EPD_ERR_ARG; }
    if (!r) {
        full.x = 0; full.y = 0;
        full.w = (uint16_t)now->w; full.h = (uint16_t)now->h;
        r = &full;
    }
    if (r->w == 0 || r->h == 0) { return EPD_ERR_ARG; }
    if (r->x + r->w > now->w || r->y + r->h > now->h) { return EPD_ERR_ARG; }
    if ((r->x & 7u) || (r->w & 7u)) { return EPD_ERR_ARG; }
    x_byte = r->x / 8;
    nbytes = r->w / 8;
    if (hot) {
        CHECK(cmd1(io, CMD_VCOM, 0x77));
    } else {
        CHECK(epd_load_fast_lut(io));
    }
    CHECK(set_partial_window(io, r));
    CHECK(send_frame_window(io, prev, now, x_byte, nbytes, r->y, r->h));
    if (!hot) {
        CHECK(epd_power_on(io));
    }
    return trigger_refresh(io);
}
int epd_display_partial_cfg(const epd_io_t *io, const fb_t *prev, const fb_t *now,
                            const epd_rect_t *r, int use_window, int fast_lut,
                            int hot, int keep_power)
{
    if (!io || !now || !now->bits || !prev || !prev->bits) { return EPD_ERR_ARG; }
    if (use_window) {
        if (!r || r->w == 0 || r->h == 0) { return EPD_ERR_ARG; }
        if (r->x + r->w > now->w || r->y + r->h > now->h) { return EPD_ERR_ARG; }
        if ((r->x & 7u) || (r->w & 7u)) { return EPD_ERR_ARG; }
    } else if (r) {
        return EPD_ERR_ARG;
    }
    if (!hot && fast_lut) {
        CHECK(epd_load_fast_lut(io));
    } else if (use_window) {
        CHECK(cmd1(io, CMD_VCOM, 0x77));
    }
    if (use_window) {
        CHECK(set_partial_window(io, r));
        CHECK(send_frame_window(io, prev, now, r->x / 8, r->w / 8, r->y, r->h));
    } else {
        CHECK(send_frame(io, prev, now));
    }
    if (!hot) {
        CHECK(epd_power_on(io));
    }
    CHECK(trigger_refresh(io));
    if (keep_power) {
        return EPD_OK;
    }
    return epd_power_off(io) == EPD_OK ? EPD_OK : EPD_ERR_POF;
}
int epd_ram_normalize(const epd_io_t *io, const fb_t *now, const epd_rect_t *r)
{
    int x_byte, nbytes;
    if (!io || !now || !now->bits || !r) { return EPD_ERR_ARG; }
    if (r->w == 0 || r->h == 0) { return EPD_ERR_ARG; }
    if (r->x + r->w > now->w || r->y + r->h > now->h) { return EPD_ERR_ARG; }
    if ((r->x & 7u) || (r->w & 7u)) { return EPD_ERR_ARG; }
    x_byte = r->x / 8;
    nbytes = r->w / 8;
    CHECK(set_partial_window(io, r));
    return send_frame_window(io, now, now, x_byte, nbytes, r->y, r->h);
}
