/* Bitmap font lookup and glyph drawing (platform independent). */
#ifndef FONT_H
#define FONT_H 
#include <stdint.h>
#include "fb.h"
typedef struct {
    uint32_t cp;
    uint8_t w, h;
    int8_t bx, by;
    uint8_t adv;
    uint32_t off;
} glyph_t;
typedef struct {
    const glyph_t *glyphs;
    uint32_t count;
    const uint8_t *bitmap;
    uint8_t px;
    uint8_t ascent;
    uint8_t descent;
    uint8_t line_height;
} font_t;
const glyph_t *font_find(const font_t *f, uint32_t cp);
int font_draw_glyph(fb_t *fb, const font_t *f, uint32_t cp,
                    int pen_x, int baseline_y, fb_ink_t ink);
int font_measure(const font_t *f, const char *utf8, int len_bytes);
int font_count_missing(const font_t *f, const char *utf8);
int utf8_next(const char **s, uint32_t *out_cp);
int utf8_next_n(const char **s, const char *end, uint32_t *out_cp);
#endif
