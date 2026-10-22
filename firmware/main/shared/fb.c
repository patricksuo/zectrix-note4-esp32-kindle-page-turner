/* 1bpp framebuffer, 400x300. Bit 1 = white, 0 = black, MSB = leftmost pixel. */
#include "fb.h"
#include <string.h>
void fb_init(fb_t *fb, uint8_t *storage)
{
    fb->bits = storage;
    fb->w = FB_WIDTH;
    fb->h = FB_HEIGHT;
    fb->stride = FB_STRIDE;
}
void fb_clear(fb_t *fb, fb_ink_t ink)
{
    memset(fb->bits, ink == FB_BLACK ? 0x00 : 0xFF, (size_t)fb->stride * fb->h);
}
void fb_set_px(fb_t *fb, int x, int y, fb_ink_t ink)
{
    if (x < 0 || y < 0 || x >= fb->w || y >= fb->h) {
        return;
    }
    uint8_t *p = &fb->bits[(size_t)y * fb->stride + (x >> 3)];
    uint8_t mask = (uint8_t)(0x80u >> (x & 7));
    if (ink == FB_BLACK) {
        *p = (uint8_t)(*p & ~mask);
    } else {
        *p = (uint8_t)(*p | mask);
    }
}
int fb_get_px(const fb_t *fb, int x, int y)
{
    if (x < 0 || y < 0 || x >= fb->w || y >= fb->h) {
        return 0;
    }
    uint8_t b = fb->bits[(size_t)y * fb->stride + (x >> 3)];
    return (b & (uint8_t)(0x80u >> (x & 7))) ? 0 : 1;
}
void fb_fill_rect(fb_t *fb, int x, int y, int w, int h, fb_ink_t ink)
{
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            fb_set_px(fb, x + i, y + j, ink);
        }
    }
}
void fb_hline(fb_t *fb, int x, int y, int len, fb_ink_t ink)
{
    for (int i = 0; i < len; i++) {
        fb_set_px(fb, x + i, y, ink);
    }
}
void fb_vline(fb_t *fb, int x, int y, int len, fb_ink_t ink)
{
    for (int i = 0; i < len; i++) {
        fb_set_px(fb, x, y + i, ink);
    }
}
void fb_rect(fb_t *fb, int x, int y, int w, int h, fb_ink_t ink)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    fb_hline(fb, x, y, w, ink);
    fb_hline(fb, x, y + h - 1, w, ink);
    fb_vline(fb, x, y, h, ink);
    fb_vline(fb, x + w - 1, y, h, ink);
}
static bool leb128(const uint8_t *p, size_t len, size_t *i, uint32_t *out)
{
    uint32_t v = 0;
    unsigned shift = 0;
    for (;;) {
        uint8_t b;
        if (*i >= len) {
            return false;
        }
        b = p[(*i)++];
        if (shift > 28u || (28u == shift && (b & 0x7Fu) > 0x0Fu)) {
            return false;
        }
        v |= (uint32_t)(b & 0x7Fu) << shift;
        if (0 == (b & 0x80u)) {
            *out = v;
            return true;
        }
        shift += 7u;
    }
}
static void set_span(uint8_t *row, int x, int n, int white)
{
    while (n > 0 && (x & 7)) {
        uint8_t m = (uint8_t)(0x80u >> (x & 7));
        row[x >> 3] = white ? (uint8_t)(row[x >> 3] | m)
                            : (uint8_t)(row[x >> 3] & ~m);
        x++;
        n--;
    }
    if (n >= 8) {
        int bytes = n >> 3;
        memset(&row[x >> 3], white ? 0xFF : 0x00, (size_t)bytes);
        x += bytes << 3;
        n -= bytes << 3;
    }
    while (n > 0) {
        uint8_t m = (uint8_t)(0x80u >> (x & 7));
        row[x >> 3] = white ? (uint8_t)(row[x >> 3] | m)
                            : (uint8_t)(row[x >> 3] & ~m);
        x++;
        n--;
    }
}
bool fb_blit_rle1(fb_t *fb, const uint8_t *rle, size_t len)
{
    size_t i;
    uint32_t w, h, total, sum;
    int x, y, white;
    if (!fb || !fb->bits || !rle || len < 4u) {
        return false;
    }
    w = (uint32_t)rle[0] | ((uint32_t)rle[1] << 8);
    h = (uint32_t)rle[2] | ((uint32_t)rle[3] << 8);
    if (fb->w <= 0 || fb->h <= 0 || w != (uint32_t)fb->w || h != (uint32_t)fb->h) {
        return false;
    }
    total = w * h;
    sum = 0;
    for (i = 4u; i < len;) {
        uint32_t n;
        if (!leb128(rle, len, &i, &n)) {
            return false;
        }
        if (n > total - sum) {
            return false;
        }
        sum += n;
        if (sum == total && i < len) {
            return false;
        }
    }
    if (sum != total) {
        return false;
    }
    x = 0;
    y = 0;
    white = 1;
    for (i = 4u; i < len;) {
        uint32_t n = 0;
        (void)leb128(rle, len, &i, &n);
        while (n > 0u) {
            int take = (int)fb->w - x;
            if ((uint32_t)take > n) {
                take = (int)n;
            }
            set_span(&fb->bits[(size_t)y * fb->stride], x, take, white);
            x += take;
            n -= (uint32_t)take;
            if (x >= fb->w) {
                x = 0;
                y++;
            }
        }
        white = !white;
    }
    return true;
}
size_t fb_diff_bits(const fb_t *a, const fb_t *b)
{
    size_t n = (size_t)a->stride * a->h;
    size_t bits = 0;
    for (size_t i = 0; i < n; i++) {
        bits += (size_t)__builtin_popcount((unsigned)(a->bits[i] ^ b->bits[i]));
    }
    return bits;
}
