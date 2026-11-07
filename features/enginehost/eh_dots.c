/* Engine host UI helpers (eh_host.h): the dot-matrix displays of the engine screens - every cell a
 * 5 x 7 grid of dots, unlit dots drawn too (the display's look), the characters from eh_font57.h -
 * and the rounded frames around them. */
#include <op1/gfx.h>
#include "eh_host.h"
#include "eh_font57.h"

static void dot(const eh_lcd *d, int x, int y, uint16_t c)
{
    if (d->dot == 3) {                          /* a round lamp: the corners stay dark */
        gfx_rect(x + 1, y, 1, 3, c);
        gfx_rect(x, y + 1, 3, 1, c);
    } else {
        gfx_rect(x, y, d->dot, d->dot, c);
    }
}

static void cell(const eh_lcd *d, int x, int y, int ch, uint16_t on)
{
    const uint8_t *g = eh_font57[ch >= 0x20 && ch < 0x7f ? ch - 0x20 : '?' - 0x20];
    int r, c, solid = d->dot >= d->pitch;      /* dots touching: the unlit cell is one rectangle */

    if (solid)
        gfx_rect(x, y, 4 * d->pitch + d->dot, 6 * d->pitch + d->dot, d->off);
    for (r = 0; r < 7; r++) {
        if (solid && !g[r])
            continue;
        for (c = 0; c < 5; c++)
            if (g[r] >> (4 - c) & 1)
                dot(d, x + c * d->pitch, y + r * d->pitch, on);
            else if (!solid)
                dot(d, x + c * d->pitch, y + r * d->pitch, d->off);
    }
}

int eh_lcd_text(const eh_lcd *d, int row, int col, const char *s, uint16_t on)
{
    int y = d->y + row * d->line;

    for (; col < d->cols && *s; col++, s++)
        if (col >= 0)
            cell(d, d->x + col * d->adv, y, (unsigned char)*s, on);
    return col;
}

void eh_lcd_blank(const eh_lcd *d, int rows)
{
    int r, c;

    for (r = 0; r < rows; r++)
        for (c = 0; c < d->cols; c++)
            cell(d, d->x + c * d->adv, d->y + r * d->line, ' ', d->on);
}

int eh_lcd_w(const eh_lcd *d)
{
    return d->cols * d->adv - (d->adv - 4 * d->pitch - d->dot);
}

/* corner inset of row i of a radius-r corner (a quarter circle through the pixel centres) */
static int inset(int r, int i)
{
    int dx = 0, e = 2 * r - 2 * i - 1;

    while (dx < r && (2 * r - 2 * dx - 1) * (2 * r - 2 * dx - 1) + e * e > 4 * r * r)
        dx++;
    return dx;
}

void eh_rrect(int x, int y, int w, int h, int r, uint16_t c)
{
    int i, a;

    if (r * 2 > h)
        r = h / 2;
    for (i = 0; i < r; i++) {
        a = inset(r, i);
        gfx_rect(x + a, y + i, w - 2 * a, 1, c);
        gfx_rect(x + a, y + h - 1 - i, w - 2 * a, 1, c);
    }
    gfx_rect(x, y + r, w, h - 2 * r, c);
}

void eh_frame(int x, int y, int w, int h, int r, int t, uint16_t c, uint16_t fill)
{
    eh_rrect(x, y, w, h, r, c);
    eh_rrect(x + t, y + t, w - 2 * t, h - 2 * t, r > t ? r - t : 0, fill);
}
