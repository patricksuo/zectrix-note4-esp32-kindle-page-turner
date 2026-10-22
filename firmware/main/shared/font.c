/* Bitmap font lookup and glyph drawing (platform independent). */
#include "font.h"
static int utf8_decode(const char **s, int avail, uint32_t *out_cp)
{
    const unsigned char *p = (const unsigned char *)*s;
    if (avail == 0) {
        return 0;
    }
    if (*p == 0) {
        return 0;
    }
    uint32_t cp;
    int extra;
    if (*p < 0x80) {
        cp = *p;
        extra = 0;
    } else if ((*p & 0xE0) == 0xC0) {
        cp = *p & 0x1Fu;
        extra = 1;
    } else if ((*p & 0xF0) == 0xE0) {
        cp = *p & 0x0Fu;
        extra = 2;
    } else if ((*p & 0xF8) == 0xF0) {
        cp = *p & 0x07u;
        extra = 3;
    } else {
        *s += 1;
        return -1;
    }
    if (avail >= 0 && extra + 1 > avail) {
        *s += 1;
        return -1;
    }
    for (int i = 0; i < extra; i++) {
        unsigned char c = p[1 + i];
        if ((c & 0xC0) != 0x80) {
            *s += 1;
            return -1;
        }
        cp = (cp << 6) | (uint32_t)(c & 0x3Fu);
    }
    static const uint32_t min_for[4] = { 0x0, 0x80, 0x800, 0x10000 };
    if (cp < min_for[extra] || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
        *s += 1;
        return -1;
    }
    *s += 1 + extra;
    *out_cp = cp;
    return 1;
}
int utf8_next(const char **s, uint32_t *out_cp)
{
    return utf8_decode(s, -1, out_cp);
}
int utf8_next_n(const char **s, const char *end, uint32_t *out_cp)
{
    return utf8_decode(s, (int)(end - *s), out_cp);
}
const glyph_t *font_find(const font_t *f, uint32_t cp)
{
    uint32_t lo = 0, hi = f->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t v = f->glyphs[mid].cp;
        if (v == cp) {
            return &f->glyphs[mid];
        }
        if (v < cp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}
static int draw_notdef(fb_t *fb, const font_t *f, int pen_x, int baseline_y, fb_ink_t ink)
{
    int w = f->px * 3 / 4;
    int h = f->px * 3 / 4;
    int x = pen_x + 1;
    int y = baseline_y - h;
    if (fb) {
        fb_rect(fb, x, y, w, h, ink);
    }
    return f->px;
}
int font_draw_glyph(fb_t *fb, const font_t *f, uint32_t cp,
                    int pen_x, int baseline_y, fb_ink_t ink)
{
    const glyph_t *g = font_find(f, cp);
    if (!g) {
        return draw_notdef(fb, f, pen_x, baseline_y, ink);
    }
    const int stride = (g->w + 7) / 8;
    const uint8_t *src = &f->bitmap[g->off];
    const int x0 = pen_x + g->bx;
    const int y0 = baseline_y - g->by;
    for (int row = 0; row < g->h; row++) {
        const uint8_t *line = &src[(size_t)row * stride];
        for (int col = 0; col < g->w; col++) {
            if (line[col >> 3] & (uint8_t)(0x80u >> (col & 7))) {
                fb_set_px(fb, x0 + col, y0 + row, ink);
            }
        }
    }
    return g->adv;
}
int font_measure(const font_t *f, const char *utf8, int len_bytes)
{
    const char *p = utf8;
    const char *end = utf8 + len_bytes;
    int width = 0;
    uint32_t cp;
    while (p < end) {
        int rc = utf8_next_n(&p, end, &cp);
        if (rc == 0) {
            break;
        }
        if (rc < 0) {
            continue;
        }
        const glyph_t *g = font_find(f, cp);
        width += g ? g->adv : f->px;
    }
    return width;
}
int font_count_missing(const font_t *f, const char *utf8)
{
    const char *p = utf8;
    uint32_t cp;
    int missing = 0;
    for (;;) {
        int rc = utf8_next(&p, &cp);
        if (rc == 0) {
            break;
        }
        if (rc < 0) {
            continue;
        }
        if (!font_find(f, cp)) {
            missing++;
        }
    }
    return missing;
}
