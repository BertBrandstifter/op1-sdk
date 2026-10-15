/* op1-sdk overlay graphics, see include/op1/gfx.h. */
#include <op1/gfx.h>

#define TE_CANVAS_W ((volatile int32_t *)0xff8017c0)   /* panel columns used (156 on FE/94) */
#define TE_CANVAS_H ((volatile int32_t *)0xff8017bc)   /* panel lines (320) */

int gfx_w = 320, gfx_h = 156;
static uint16_t *fb;

/* 3x5 glyphs for ASCII 32..90, 3 bits per row, top row in bits 14..12; 0x7fff = no glyph */
static const uint16_t font[59] = {
    0x0000, 0x7fff, 0x7fff, 0x2eba, 0x7fff, 0x7fff, 0x7fff, 0x7fff,     /* 35 # (a 3x5 hash reads as H) */
    0x7fff, 0x7fff, 0x0aa8, 0x05d0,                                     /* 42 '*' */ 0x7fff, 0x01c0, 0x0002, 0x12a4,
    0x7b6f, 0x2c97, 0x73e7, 0x73cf, 0x5bc9, 0x79cf, 0x79ef, 0x7252,
    0x7bef, 0x7bcf, 0x0410, 0x7fff, 0x7fff, 0x0e38, 0x7fff, 0x7282,     /* 63 '?' */
    0x7fff, 0x2bed, 0x6bae, 0x3923, 0x6b6e, 0x79a7, 0x79a4, 0x396b,
    0x5bed, 0x7497, 0x126a, 0x5bad, 0x4927, 0x5fed, 0x6b6d, 0x2b6a,
    0x6ba4, 0x2b73, 0x6bad, 0x388e, 0x7492, 0x5b6f, 0x5b6a, 0x5bfd,
    0x5aad, 0x5a92, 0x72a7,
};

void gfx_begin(uint16_t *buf)
{
    int32_t cw = *TE_CANVAS_W, ch = *TE_CANVAS_H;

    fb = buf;
    if (ch > 0 && ch <= 320 && cw > 0 && cw <= GFX_STRIDE) {
        gfx_w = ch;
        gfx_h = cw;
    }
}

void gfx_pixel(int x, int y, uint16_t c)
{
    if (fb && (unsigned int)x < (unsigned int)gfx_w && (unsigned int)y < (unsigned int)gfx_h)
        fb[(gfx_w - 1 - x) * GFX_STRIDE + y] = c;
}

void gfx_rect(int x, int y, int w, int h, uint16_t c)
{
    int i, j;

    /* clip once, then fill: one UI column is one contiguous run of a panel line */
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > gfx_w)
        w = gfx_w - x;
    if (y + h > gfx_h)
        h = gfx_h - y;
    if (!fb || w <= 0 || h <= 0)
        return;
    for (i = 0; i < w; i++) {
        uint16_t *p = fb + (gfx_w - 1 - (x + i)) * GFX_STRIDE + y;

        for (j = 0; j < h; j++)
            p[j] = c;
    }
}

void gfx_disc(int cx, int cy, int r, uint16_t c)
{
    int dx, dy;

    for (dy = -r; dy <= r; dy++)
        for (dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r + r)
                gfx_pixel(cx + dx, cy + dy, c);
}

static void glyph(int x, int y, char ch, uint16_t c, int scale)
{
    unsigned int bits, row, col;

    if (ch >= 'a' && ch <= 'z')
        ch -= 32;
    bits = (ch >= 32 && ch <= 90) ? font[ch - 32] : 0x7fff;
    if (fb && scale >= 1 && x >= 0 && y >= 0 && x + 3 * scale <= gfx_w && y + 5 * scale <= gfx_h) {
        uint16_t *p = fb + (gfx_w - 1 - x) * GFX_STRIDE + y;    /* inside: plain stores */
        int sx, sy;

        for (col = 0; col < 3; col++)
            for (sx = 0; sx < scale; sx++, p -= GFX_STRIDE)
                for (row = 0; row < 5; row++)
                    if (bits & (1u << (14 - row * 3 - col)))
                        for (sy = 0; sy < scale; sy++)
                            p[row * scale + sy] = c;
        return;
    }
    for (row = 0; row < 5; row++)
        for (col = 0; col < 3; col++)
            if (bits & (1u << (14 - row * 3 - col)))
                gfx_rect(x + col * scale, y + row * scale, scale, scale, c);
}

int gfx_text(int x, int y, const char *s, uint16_t c, int scale)
{
    for (; *s; s++, x += 4 * scale)
        glyph(x, y, *s, c, scale);
    return x;
}

int gfx_hex(int x, int y, uint32_t v, int digits, uint16_t c, int scale)
{
    char s[9];
    int i;

    if (digits < 1 || digits > 8)
        digits = 8;
    for (i = digits - 1; i >= 0; i--, v >>= 4)
        s[i] = "0123456789ABCDEF"[v & 15];
    s[digits] = 0;
    return gfx_text(x, y, s, c, scale);
}

int gfx_dec(int x, int y, int32_t v, uint16_t c, int scale)
{
    char s[12], *p = s + sizeof(s) - 1;
    uint32_t u = v < 0 ? -(uint32_t)v : (uint32_t)v;

    *p = 0;
    do
        *--p = '0' + u % 10;
    while (u /= 10);
    if (v < 0)
        *--p = '-';
    return gfx_text(x, y, p, c, scale);
}
