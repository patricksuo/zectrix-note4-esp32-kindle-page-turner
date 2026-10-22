/* 1bpp framebuffer, 400x300. Bit 1 = white, 0 = black, MSB = leftmost pixel. */
#ifndef FB_H
#define FB_H 
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define FB_WIDTH 400
#define FB_HEIGHT 300
#define FB_STRIDE ((FB_WIDTH + 7) / 8)
#define FB_BYTES (FB_STRIDE * FB_HEIGHT)
typedef struct {
    uint8_t *bits;
    int w, h, stride;
} fb_t;
typedef enum { FB_WHITE = 0, FB_BLACK = 1 } fb_ink_t;
void fb_init(fb_t *fb, uint8_t *storage);
void fb_clear(fb_t *fb, fb_ink_t ink);
void fb_set_px(fb_t *fb, int x, int y, fb_ink_t ink);
int fb_get_px(const fb_t *fb, int x, int y);
void fb_fill_rect(fb_t *fb, int x, int y, int w, int h, fb_ink_t ink);
void fb_rect(fb_t *fb, int x, int y, int w, int h, fb_ink_t ink);
void fb_hline(fb_t *fb, int x, int y, int len, fb_ink_t ink);
void fb_vline(fb_t *fb, int x, int y, int len, fb_ink_t ink);
size_t fb_diff_bits(const fb_t *a, const fb_t *b);
bool fb_blit_rle1(fb_t *fb, const uint8_t *rle, size_t len);
#endif
