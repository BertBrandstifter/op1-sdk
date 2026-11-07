/* Engine host UI helpers (eh_host.h): antialiased shapes for the engine screens. Each pixel near
 * an edge gets a coverage (0..256) from its distance to the shape, blended into the frame TE
 * rendered (RGB565). Pixel centres sit on whole numbers (as gfx_rect / gfx_disc); eh_aa_line and
 * eh_aa_ellipse take 1/16 px. Lines and ellipses are kept (their pixels and coverages) and played
 * back while the same one is drawn again: most of a screen repeats every frame. First used by
 * MALLET's instruments (features/mallet), then PARTY's knob and buttons. */
#include <op1/gfx.h>
#include "eh_host.h"

static uint16_t *frame_buf;
int eh_aa_dy;

void eh_aa_begin(uint16_t *buf) { frame_buf = buf; }

static void rect(int x, int y, int w, int h, uint16_t c) { gfx_rect(x, y + eh_aa_dy, w, h, c); }

/* the shapes' pixels, kept: x | y << 9 | coverage << 17 (see kept_play) */
#define KEPT_SHAPES 256
#define KEPT_PIXELS 16384
static uint32_t kept_px[KEPT_PIXELS];
static int kept_n, keeping;                     /* pixels used; recording one (1) or overflowed (-1) */

void eh_aa_plot(int x, int y, uint16_t c, int a)
{
    uint16_t *p;
    uint32_t f, b;

    y += eh_aa_dy;
    if (a <= 0 || (unsigned int)x >= (unsigned int)gfx_w || (unsigned int)y >= (unsigned int)gfx_h)
        return;
    if (keeping > 0) {
        if (kept_n < KEPT_PIXELS)
            kept_px[kept_n++] = (uint32_t)x | (uint32_t)y << 9 | (uint32_t)(a > 256 ? 256 : a) << 17;
        else
            keeping = -1;
    }
    p = &frame_buf[(gfx_w - 1 - x) * GFX_STRIDE + y];
    if (a >= 248) {
        *p = c;
        return;
    }
    a = (a + 4) >> 3;                           /* 0..31; the channels apart in one word (g, r, b) */
    f = (c | (uint32_t)c << 16) & 0x07e0f81f;
    b = (*p | (uint32_t)*p << 16) & 0x07e0f81f;
    b = ((f * a + b * (32 - a)) >> 5) & 0x07e0f81f;
    *p = (uint16_t)(b | b >> 16);
}

/* coverage of a pixel whose centre is d (1/16 px) outside an edge */
static int cover(int d) { return d <= -8 ? 256 : d >= 8 ? 0 : (8 - d) * 16; }

static int iabs(int v) { return v < 0 ? -v : v; }

/* n / d for d > 0, |n| < 2^23, within 0.4 %, without a divide (per pixel; the divide is a
 * library call): d's top 8 bits through a table of reciprocals, 2^22 / (128 + i + 1/2) */
static uint16_t rcp[128];

static inline int qdiv(int n, int d)
{
    int e = 30 - __builtin_clrsb(d), i, s = 1;      /* clrsb: Blackfin's SIGNBITS */

    if (!rcp[0])
        for (i = 0; i < 128; i++)
            rcp[i] = (uint16_t)((1 << 23) / (2 * (128 + i) + 1));
    i = rcp[(e >= 7 ? d >> (e - 7) : d << (7 - e)) - 128];
    if (n < 0) {
        n = -n;
        s = -1;
    }
    return s * (((n >> 8) * i >> (e + 7)) + ((n & 255) * i >> (e + 15)));
}

static int isqrt(uint32_t v)
{
    uint32_t r = 0, b = 1u << 30;

    while (b > v)
        b >>= 2;
    for (; b; b >>= 2)
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
    return (int)r;
}

/* a line from (x0, y0) to (x1, y1) with round ends, half width hw (eh_aa_line: kept) */
static void line_draw(int x0, int y0, int x1, int y1, int hw, uint16_t c)
{
    int dx = x1 - x0, dy = y1 - y0, len = isqrt((uint32_t)(dx * dx + dy * dy)), r = hw + 8;
    int ux = len ? dx * 4096 / len : 4096, uy = len ? dy * 4096 / len : 0;
    int xl = (x0 < x1 ? x0 : x1) - r, xh = (x0 < x1 ? x1 : x0) + r, py, px;

    for (py = ((y0 < y1 ? y0 : y1) - r + 15) >> 4; py <= ((y0 < y1 ? y1 : y0) + r) >> 4; py++) {
        int Y = py * 16 - y0, lo = xl, hi = xh;

        if (dy) {                               /* only the pixels near the line on this row */
            int xm = x0 + (dy > 0 ? qdiv(Y * dx, dy) : qdiv(-Y * dx, -dy));
            int half = qdiv((r + 16) * len, iabs(dy)) + 1;

            lo = xm - half > xl ? xm - half : xl;
            hi = xm + half < xh ? xm + half : xh;
        }
        for (px = (lo + 15) >> 4; px <= hi >> 4; px++) {
            int X = px * 16 - x0, along = (X * ux + Y * uy) >> 12, d;

            if (along < 0)
                d = isqrt((uint32_t)(X * X + Y * Y));
            else if (along > len)
                d = isqrt((uint32_t)((X - dx) * (X - dx) + (Y - dy) * (Y - dy)));
            else
                d = iabs((Y * ux - X * uy) >> 12);
            eh_aa_plot(px, py, c, cover(d - hw));
        }
    }
}

/* the angle of (x, y) in 1/32768 turn (0 = right, 8192 = down) */
static int angle(int x, int y)
{
    int ax = iabs(x), ay = iabs(y), r, t;

    if (ax >= ay) {
        r = ax ? qdiv(ay * 1024, ax) : 0;
        t = (4096 * r >> 10) + (1424 * r * (1024 - r) >> 20);   /* atan, within 0.1 step */
    } else {
        r = qdiv(ax * 1024, ay);
        t = 8192 - (4096 * r >> 10) - (1424 * r * (1024 - r) >> 20);
    }
    if (x < 0)
        t = 16384 - t;
    return y < 0 ? (32768 - t) & 32767 : t;
}

/* an ellipse round (cx, cy) with radii a, b: its outline as a ring of half width hw, or filled
 * (hw < 0), from step s0 to s1 (128 a turn, 0 = right, 32 = down: s1 - s0 <= 64, or a whole turn
 * from s1 - s0 >= 128), dashed when dash (on from step 2 to 6 of every 8, as the old polyline's).
 * Drawn in two scan passes: rows take the pixels where the curve runs steeper than 45 degrees,
 * columns the others (each pixel once); per scan line the curve's crossing and normal, per pixel
 * its distance along the scan line times the normal's part in it. */
typedef struct {
    int cx, cy, a, b, h, fill, arc, dash, s0, s1, a2, b2;
    uint16_t c;
} ellipse;

/* the pass along rows (sw 0) or columns (sw 1: x and y swapped throughout) */
static void ell_pass(const ellipse *e, int sw)
{
    int ca = sw ? e->b : e->a, cb = sw ? e->a : e->b, cc = sw ? e->cy : e->cx, cs = sw ? e->cx : e->cy;
    int win = (e->h + 8) * 3 / 2 + 8, p;

    for (p = (cs - cb + 16) >> 4; p <= (cs + cb - 1) >> 4; p++) {
        int Y = p * 16 - cs, xc, tx, ty, mag, nx, lo, hi, x, side;
        int t0[2] = { 0, 0 }, kt = 0, md = 0;

        if (iabs(Y) >= cb)
            continue;
        xc = qdiv(ca * isqrt((uint32_t)(cb * cb - Y * Y)), cb);
        tx = qdiv(ca * iabs(Y), cb);            /* the tangent, d(point) / d(step angle) */
        ty = qdiv(cb * xc, ca);
        mag = isqrt((uint32_t)(tx * tx + ty * ty));
        if (!mag)
            continue;
        nx = qdiv(ty * 4096, mag);              /* the normal's part along the scan line (1/4096) */
        if (e->arc) {                           /* the angle at both crossings, its change along x */
            int u = qdiv(xc * 4096, ca), v = qdiv(Y * 4096, cb);

            t0[0] = sw ? angle(v, -u) : angle(-u, v);
            t0[1] = sw ? angle(v, u) : angle(u, v);
            kt = qdiv(v * 1304, ca);            /* 1304 = 32768 / 2 pi / 4 */
            kt = sw ? kt : -kt;
            md = qdiv(mag * 4096, 5215);        /* 1/16 px per 1/256 step */
        }
        for (side = 0; side < 2; side++) {
            if (e->fill || xc <= win) {         /* one span across */
                if (side)
                    break;
                lo = (-xc - win + cc + 15) >> 4;
                hi = (xc + win + cc) >> 4;
            } else if (!side) {
                lo = (-xc - win + cc + 15) >> 4;
                hi = (-xc + win + cc) >> 4;
            } else {
                lo = (xc - win + cc + 15) >> 4;
                hi = (xc + win + cc) >> 4;
            }
            for (x = lo; x <= hi; x++) {
                int X = x * 16 - cc, ax = iabs(X), d, cov, in = 256;

                if (sw ? iabs(Y) * e->b2 >= ax * e->a2 : ax * e->b2 < iabs(Y) * e->a2)
                    continue;                   /* the other pass's pixel */
                d = (ax - xc) * nx >> 12;
                cov = e->fill ? cover(d) : cover(iabs(d) - e->h);
                if (!cov)
                    continue;
                if (e->arc) {                   /* how far inside the arc / dash, along it */
                    int t = X < 0 ? t0[0] + ((X + xc) * kt >> 10) : t0[1] + ((X - xc) * kt >> 10);
                    int din = 1 << 20, ph;

                    t = e->fill ? (sw ? angle(Y, X) : angle(X, Y)) : t;   /* filled: the pixel's own angle */
                    if (e->s1 - e->s0 < 128) {
                        ph = (t - e->s0 * 256) & 32767;
                        if (ph > 16384 + (e->s1 - e->s0) * 128)
                            ph -= 32768;        /* before the start */
                        din = ph < (e->s1 - e->s0) * 128 ? ph : (e->s1 - e->s0) * 256 - ph;
                    }
                    if (e->dash) {
                        ph = (t - 512) & 2047;
                        ph = ph < 1024 ? (ph < 512 ? ph : 1024 - ph) : -(ph < 1536 ? ph - 1024 : 2048 - ph);
                        din = ph < din ? ph : din;
                    }
                    if (din <= -1024)
                        continue;
                    if (din < 1024 && !(in = cover(-(din * md >> 12))))
                        continue;
                }
                eh_aa_plot(sw ? p : x, sw ? x : p, e->c, cov * in >> 8);
            }
        }
    }
}

/* An ellipse's or a line's pixels and coverages are kept and played back while the same one
 * (place, size, arc, eh_aa_dy) is drawn again, in any colour: most of a screen repeats every
 * frame (a MALLET instrument's shapes: some 3000 .. 9000 pixels). eh_aa_forget (MALLET: another
 * instrument) or a full table starts it over. */
static struct {
    int16_t cx, cy, a, b, hw, dy;
    int16_t s0, s1;
    int8_t dash, used;
    uint16_t start, n;
} kept[KEPT_SHAPES];

void eh_aa_forget(void)
{
    int i;

    for (i = 0; i < KEPT_SHAPES; i++)
        kept[i].used = 0;
    kept_n = 0;
}

static int kept_slot(int cx, int cy, int a, int b, int hw, int s0)
{
    return (int)(((uint32_t)cx * 31 + (uint32_t)cy * 17 + (uint32_t)a * 7 + (uint32_t)b * 3 + (uint32_t)hw
                  + (uint32_t)s0 * 5) % KEPT_SHAPES);
}

/* plays the shape back (1) or starts keeping it (0: draw it, then kept_end) */
static int kept_k;

static int kept_play(int cx, int cy, int a, int b, int hw, int s0, int s1, int dash, uint16_t c)
{
    int k = kept_slot(cx, cy, a, b, hw, s0), i;

    for (i = 0; i < 16; i++, k = (k + 1) % KEPT_SHAPES) {
        if (!kept[k].used)
            break;
        if (kept[k].cx == cx && kept[k].cy == cy && kept[k].a == a && kept[k].b == b && kept[k].hw == hw
            && kept[k].dy == eh_aa_dy && kept[k].s0 == s0 && kept[k].s1 == s1 && kept[k].dash == dash) {
            const uint32_t *q = &kept_px[kept[k].start];
            int o = eh_aa_dy, j;

            eh_aa_dy = 0;                       /* kept with the offset applied */
            for (j = kept[k].n; j; j--, q++)
                eh_aa_plot((int)(*q & 511), (int)(*q >> 9 & 255), c, (int)(*q >> 17));
            eh_aa_dy = o;
            return 1;
        }
    }
    if (i == 16 || kept_n > KEPT_PIXELS - 1024) {  /* full: start over */
        eh_aa_forget();
        k = kept_slot(cx, cy, a, b, hw, s0);
    }
    kept[k].cx = (int16_t)cx;
    kept[k].cy = (int16_t)cy;
    kept[k].a = (int16_t)a;
    kept[k].b = (int16_t)b;
    kept[k].hw = (int16_t)hw;
    kept[k].dy = (int16_t)eh_aa_dy;
    kept[k].s0 = (int16_t)s0;
    kept[k].s1 = (int16_t)s1;
    kept[k].dash = (int8_t)dash;
    kept[k].start = (uint16_t)kept_n;
    kept_k = k;
    keeping = 1;
    return 0;
}

static void kept_end(void)
{
    if (keeping > 0) {
        kept[kept_k].n = (uint16_t)(kept_n - kept[kept_k].start);
        kept[kept_k].used = 1;
    } else {
        kept_n = kept[kept_k].start;            /* did not fit: drawn, not kept */
    }
    keeping = 0;
}

void eh_aa_ellipse(int cx, int cy, int a, int b, int hw, int s0, int s1, int dash, uint16_t c)
{
    ellipse e;

    if (kept_play(cx, cy, a, b, hw, s0, s1, dash, c))
        return;
    e.cx = cx;
    e.cy = cy;
    e.a = a < 8 ? 8 : a;
    e.b = b < 8 ? 8 : b;
    e.fill = hw < 0;
    e.h = e.fill ? 0 : hw;
    e.dash = dash;
    e.arc = dash || s1 - s0 < 128;
    e.s0 = s0;
    e.s1 = s1;
    e.a2 = e.a * e.a >> 6;                      /* who takes a pixel: |x| b^2 against |y| a^2 */
    e.b2 = e.b * e.b >> 6;
    e.c = c;
    ell_pass(&e, 0);
    ell_pass(&e, 1);
    kept_end();
}

void eh_aa_line(int x0, int y0, int x1, int y1, int hw, uint16_t c)
{
    if (!kept_play(x0, y0, x1, y1, hw, -1, -1, 0, c)) {      /* s0 = -1: a line */
        line_draw(x0, y0, x1, y1, hw, c);
        kept_end();
    }
}

/* rounded rectangles: eh_aa_frame = an outline t px wide (t <= r), eh_aa_box = filled; corners of
 * radius r */
static int corner(int w, int h, int r)
{
    r = r > (w - 1) / 2 ? (w - 1) / 2 : r;
    return r > (h - 1) / 2 ? (h - 1) / 2 : r;
}

/* the four corners: quarter circles round the corner centres (x0, y0) .. (x1, y1), pixels off
 * the axes (the straight edges cover those); radius r px, a ring t px wide or filled (t = 0).
 * Coverage tables are made once per shape (a bar's corners are drawn some 150 times a frame). */
#define CORNER_MAX  10
static void corners(int x0, int y0, int x1, int y1, int r, int t, uint16_t c)
{
    static struct { uint8_t r, t, n, cov[CORNER_MAX][CORNER_MAX]; } tab[8];
    static int used;
    int e, i, j, n;

    for (e = 0; e < used && (tab[e].r != r || tab[e].t != t); e++)
        ;
    if (e == used) {
        int cr = t ? r * 16 - (t - 1) * 8 : r * 16 + 8;

        if (used == 8)                          /* full (never with these instruments): reuse the last */
            e = 7;
        else
            used++;
        n = (r * 16 + 8 + 15) >> 4;
        n = n >= CORNER_MAX ? CORNER_MAX - 1 : n;
        tab[e].r = (uint8_t)r;
        tab[e].t = (uint8_t)t;
        tab[e].n = (uint8_t)n;
        for (i = 1; i <= n; i++)
            for (j = 1; j <= n; j++) {
                int d = isqrt((uint32_t)(i * i + j * j) << 8) - cr;
                int a = t ? cover(iabs(d) - t * 8) : cover(d);

                tab[e].cov[i][j] = (uint8_t)(a > 255 ? 255 : a);
            }
    }
    n = tab[e].n;
    for (i = 1; i <= n; i++)
        for (j = 1; j <= n; j++) {
            int a = tab[e].cov[i][j] == 255 ? 256 : tab[e].cov[i][j];

            if (!a)
                continue;
            eh_aa_plot(x0 - i, y0 - j, c, a);
            eh_aa_plot(x1 + i, y0 - j, c, a);
            eh_aa_plot(x0 - i, y1 + j, c, a);
            eh_aa_plot(x1 + i, y1 + j, c, a);
        }
}

void eh_aa_frame(int x, int y, int w, int h, int r, int t, uint16_t c)
{
    r = corner(w, h, r);
    rect(x + r, y, w - 2 * r, t, c);
    rect(x + r, y + h - t, w - 2 * r, t, c);
    rect(x, y + r, t, h - 2 * r, c);
    rect(x + w - t, y + r, t, h - 2 * r, c);
    if (r)
        corners(x + r, y + r, x + w - 1 - r, y + h - 1 - r, r, t, c);
}

void eh_aa_box(int x, int y, int w, int h, int r, uint16_t c)
{
    r = corner(w, h, r);
    rect(x, y + r, w, h - 2 * r, c);
    rect(x + r, y, w - 2 * r, r, c);
    rect(x + r, y + h - r, w - 2 * r, r, c);
    if (r)
        corners(x + r, y + r, x + w - 1 - r, y + h - 1 - r, r, 0, c);
}
