/* py_draw: PLAY's screens from 0.17 (py_draw.h, SCREENS.md). GUI thread; no TE addresses.
 *
 *   type      labels in the 3x5 font, values in the 5x7 font (both tracked), primary numbers
 *             (levels, BPM, BAR, the chain number) as thin numerals: strokes of the antialiased
 *             lines and arcs of enginehost's eh_aa.c, which keeps the pixels of a shape drawn
 *             again in the same place (most of the screen, every frame).
 *   colours   a track's colour is its list position's (the encoders' order: blue, green, white,
 *             red, then again); the type label keeps its own (SYN white, DRM red, AUD amber).
 *   meters    -48 .. +6 dB, linear in dB, from the peak of each frame: instant attack, ~0.8 dB a
 *             frame release, a white hold falling ~0.2 dB a frame; red from 0 dB up. */
#include <op1/gfx.h>
#include "py_draw.h"
#include "../enginehost/eh_host.h"

#define S16(v)      ((v) * 16)

static const uint16_t track_col[4] = { D_BLUE, D_GREEN, D_TEXT, D_RED };

uint16_t py_d_track_col(int i)
{
    return track_col[i & 3];
}

uint16_t py_d_scale(uint16_t c, int a)
{
    uint32_t r = (uint32_t)(c >> 11) * (uint32_t)a >> 8, g = (uint32_t)(c >> 5 & 63) * (uint32_t)a >> 8,
             b = (uint32_t)(c & 31) * (uint32_t)a >> 8;

    return (uint16_t)(r << 11 | g << 5 | b);
}

static uint16_t type_col(int type)
{
    return type == PS_AUDIO ? D_AMBER : type == PS_DRUM ? D_RED : D_TEXT;
}

static const char *type_tag(int type)
{
    return type == PS_AUDIO ? "AUD" : type == PS_SYNTH ? "SYN" : "DRM";
}

static const char *type_word(int type)
{
    return type == PS_AUDIO ? "AUDIO" : type == PS_SYNTH ? "SYNTH" : "DRUM";
}

/* ------------------------------------------------------------------ text */

/* 3x5 glyphs gfx_text lacks: 3 bits a row, top row in bits 14..12 */
static uint16_t extra3(char ch)
{
    switch (ch) {
    case '%': return 0x52a5;
    case '<': return 0x1511;
    case '>': return 0x4454;
    case '\1': return 0x0080;                   /* a middle dot */
    case '|': return 0x2492;
    case '(': return 0x2922;
    case ')': return 0x224a;
    case '!': return 0x2482;
    default: return 0;
    }
}

int py_d_text3(int x, int y, const char *s, uint16_t c, int sp)
{
    char one[2] = { 0, 0 };

    for (; *s; s++) {
        uint16_t g = extra3(*s);

        if (g) {
            int i;

            for (i = 0; i < 15; i++)
                if (g >> (14 - i) & 1)
                    gfx_pixel(x + i % 3, y + i / 3, c);
        } else if (*s != ' ') {
            one[0] = *s;
            gfx_text(x, y, one, c, 1);
        }
        x += 4 + sp;
    }
    return x;
}

int py_d_w3(const char *s, int sp)
{
    int n = 0;

    while (s[n])
        n++;
    return n ? n * (4 + sp) - 1 - sp : 0;
}

int py_d_text7(int x, int y, const char *s, uint16_t c, int sp)
{
    char one[2] = { 0, 0 };

    for (; *s; s++) {
        if (*s != ' ') {
            one[0] = *s;
            gfx_text7(x, y, one, c);
        }
        x += 6 + sp;
    }
    return x;
}

int py_d_w7(const char *s, int sp)
{
    int n = 0;

    while (s[n])
        n++;
    return n ? n * (6 + sp) - 1 - sp : 0;
}

void py_d_fmt(char *out, int v)
{
    char t[8];
    int n = 0, i = 0;

    if (v < 0) {
        out[i++] = '-';
        v = -v;
    }
    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < 7);
    while (n)
        out[i++] = t[--n];
    out[i] = 0;
}

void py_d_hex2(char *out, int v)
{
    static const char hex[] = "0123456789ABCDEF";

    out[0] = hex[(v >> 4) & 15];
    out[1] = hex[v & 15];
    out[2] = 0;
}

/* ------------------------------------------------------------------ thin numerals
 *
 * Glyphs on a grid 100 high: the advance, then strokes: 'L' x0 y0 x1 y1 (a line), 'E' cx cy a b
 * s0 s1 (an elliptic arc from step s0 to s1, 128 steps a turn, 0 = right, 32 = down; s1 - s0 >= 128
 * = the whole ellipse, else at most 64), 'D' cx cy r (a dot), 0 = the end. */
static const uint8_t g_0[] = { 56, 'E', 28, 50, 27, 49, 0, 128, 0 };
static const uint8_t g_1[] = { 32, 'L', 6, 18, 22, 1, 'L', 22, 1, 22, 100, 0 };
static const uint8_t g_2[] = { 54, 'E', 27, 26, 24, 25, 68, 128, 'E', 27, 26, 24, 25, 128, 150,
                               'L', 38, 48, 3, 100, 'L', 3, 100, 54, 100, 0 };
static const uint8_t g_3[] = { 52, 'E', 26, 24, 22, 23, 72, 128, 'E', 26, 24, 22, 23, 128, 160,
                               'E', 26, 73, 25, 26, 96, 160, 'E', 26, 73, 25, 26, 160, 186, 0 };
static const uint8_t g_4[] = { 56, 'L', 40, 100, 40, 1, 'L', 40, 1, 2, 70, 'L', 2, 70, 56, 70, 0 };
static const uint8_t g_5[] = { 54, 'L', 48, 1, 12, 1, 'L', 12, 1, 12, 45, 'E', 27, 70, 26, 30, 84, 128,
                               'E', 27, 70, 26, 30, 128, 186, 0 };
static const uint8_t g_6[] = { 54, 'E', 27, 70, 26, 29, 0, 128, 'L', 9, 50, 38, 1, 0 };
static const uint8_t g_7[] = { 52, 'L', 1, 1, 52, 1, 'L', 52, 1, 16, 100, 0 };
static const uint8_t g_8[] = { 52, 'E', 26, 24, 21, 23, 0, 128, 'E', 26, 73, 25, 27, 0, 128, 0 };
static const uint8_t g_9[] = { 54, 'E', 27, 30, 26, 29, 0, 128, 'L', 45, 50, 16, 100, 0 };
static const uint8_t g_A[] = { 58, 'L', 2, 100, 29, 1, 'L', 29, 1, 56, 100, 'L', 11, 68, 47, 68, 0 };
static const uint8_t g_B[] = { 52, 'L', 4, 1, 4, 100, 'L', 4, 1, 24, 1, 'E', 24, 24, 22, 23, 96, 160,
                               'L', 24, 47, 4, 47, 'L', 4, 47, 26, 47, 'E', 26, 73, 24, 27, 96, 160,
                               'L', 26, 100, 4, 100, 0 };
static const uint8_t g_C[] = { 56, 'E', 30, 50, 28, 49, 16, 64, 'E', 30, 50, 28, 49, 64, 112, 0 };
static const uint8_t g_D[] = { 56, 'L', 4, 1, 4, 100, 'L', 4, 1, 18, 1, 'E', 18, 50, 34, 49, 96, 160,
                               'L', 18, 100, 4, 100, 0 };
static const uint8_t g_E[] = { 44, 'L', 4, 1, 4, 100, 'L', 4, 1, 42, 1, 'L', 4, 50, 38, 50,
                               'L', 4, 100, 42, 100, 0 };
static const uint8_t g_F[] = { 42, 'L', 4, 1, 4, 100, 'L', 4, 1, 42, 1, 'L', 4, 50, 38, 50, 0 };
static const uint8_t g_N[] = { 54, 'L', 4, 100, 4, 1, 'L', 4, 1, 50, 100, 'L', 50, 100, 50, 1, 0 };
static const uint8_t g_O[] = { 60, 'E', 30, 50, 29, 49, 0, 128, 0 };
static const uint8_t g_minus[] = { 34, 'L', 4, 60, 30, 60, 0 };
static const uint8_t g_dot[] = { 22, 'D', 11, 95, 5, 0 };
static const uint8_t g_colon[] = { 22, 'D', 11, 38, 5, 'D', 11, 95, 5, 0 };
static const uint8_t g_slash[] = { 36, 'L', 33, 1, 3, 100, 0 };
static const uint8_t g_plus[] = { 50, 'L', 25, 34, 25, 84, 'L', 2, 59, 48, 59, 0 };
static const uint8_t g_space[] = { 24, 0 };

static const uint8_t *num_glyph(char ch)
{
    static const uint8_t *const dig[10] = { g_0, g_1, g_2, g_3, g_4, g_5, g_6, g_7, g_8, g_9 };
    static const uint8_t *const hex[6] = { g_A, g_B, g_C, g_D, g_E, g_F };

    if (ch >= '0' && ch <= '9')
        return dig[ch - '0'];
    if (ch >= 'A' && ch <= 'F')
        return hex[ch - 'A'];
    switch (ch) {
    case 'N': return g_N;
    case 'O': return g_O;
    case '-': return g_minus;
    case '.': return g_dot;
    case ':': return g_colon;
    case '/': return g_slash;
    case '+': return g_plus;
    default: return g_space;
    }
}

#define GAP_U       14                          /* between glyphs, grid units */

static int adv(int h, const uint8_t *g)
{
    return ((g[0] + GAP_U) * h + 50) / 100;
}

int py_d_wnum(int h, const char *s)
{
    int w = 0;

    for (; *s; s++)
        w += adv(h, num_glyph(*s));
    return w ? w - GAP_U * h / 100 : 0;
}

/* grid units to whole pixels from the origin */
static int px(int o, int u, int h)
{
    return o + (u * h + 50) / 100;
}

/* grid units to 1/16 px from the origin (whole px) */
static int p16(int o, int u, int h)
{
    return o * 16 + (u * h * 16 + 50) / 100;
}

int py_d_num(int x, int y, int h, const char *s, uint16_t c)
{
    int hw = h >= 18 ? 10 : 9, fine = h < 18;

    for (; *s; s++) {
        const uint8_t *g = num_glyph(*s), *p = g + 1;

        while (*p) {
            if (*p == 'L' && fine) {            /* small: where the strokes are; straight ones on
                                                 * pixel centres (crisp), slanted ones as they fall */
                int x0 = p16(x, p[1], h), y0 = p16(y, p[2], h), x1 = p16(x, p[3], h), y1 = p16(y, p[4], h);

                if (p[1] == p[3])
                    x0 = x1 = (x0 + 8) & ~15;
                if (p[2] == p[4])
                    y0 = y1 = (y0 + 8) & ~15;
                eh_aa_line(x0, y0, x1, y1, hw, c);
                p += 5;
            } else if (*p == 'L') {
                eh_aa_line(S16(px(x, p[1], h)), S16(px(y, p[2], h)), S16(px(x, p[3], h)),
                           S16(px(y, p[4], h)), hw, c);
                p += 5;
            } else if (*p == 'E' && fine) {
                eh_aa_ellipse(p16(x, p[1], h), p16(y, p[2], h), p[3] * h * 16 / 100, p[4] * h * 16 / 100, hw,
                              p[5], p[6], 0, c);
                p += 7;
            } else if (*p == 'E') {             /* extremes on pixel centres: crisp edges */
                int l = px(x, p[1] - p[3], h), r = px(x, p[1] + p[3], h);
                int t = px(y, p[2] - p[4], h), b = px(y, p[2] + p[4], h);

                eh_aa_ellipse((l + r) * 8, (t + b) * 8, (r - l) * 8, (b - t) * 8, hw, p[5], p[6], 0, c);
                p += 7;
            } else if (h < 24) {                /* 'D', small: a pixel or 2 x 2 */
                int d = h < 14 ? 1 : 2;

                gfx_rect(px(x, p[1], h) - d / 2, px(y, p[2], h) - d + 1, d, d, c);
                p += 4;
            } else {                            /* 'D' */
                int r = p[3] * h * 16 / 100;

                eh_aa_ellipse(px(x, p[1], h) * 16, px(y, p[2], h) * 16 - 8, r, r, -1, 0, 128, 0, c);
                p += 4;
            }
        }
        x += adv(h, g);
    }
    return x - GAP_U * h / 100;
}

/* ------------------------------------------------------------------ shapes */

void py_d_ring(int cx, int cy, int r, uint16_t c)
{
    eh_aa_ellipse(S16(cx), S16(cy), S16(r), S16(r), 8, 0, 128, 0, c);
}

void py_d_dial(int cx, int cy, int r, int num, uint16_t c)
{
    char t[4];

    py_d_ring(cx, cy, r, c);
    py_d_fmt(t, num);
    py_d_text3(cx - (t[1] ? 3 : 1), cy - 2, t, c, 0);
    gfx_rect(cx, cy - r - 2, 1, r < 5 ? 3 : 4, D_TEXT);    /* the pointer at 12 o'clock */
}

void py_d_button(int cx, int cy, char letter, int on, uint16_t col)
{
    char t[2] = { letter, 0 };

    if (on)
        eh_aa_ellipse(S16(cx), S16(cy), S16(4) + 8, S16(4) + 8, -1, 0, 128, 0, col);
    else
        py_d_ring(cx, cy, 4, py_d_scale(col, 110));     /* off: the same colour, dimmer */
    gfx_text(cx - 1, cy - 2, t, on ? GFX_BLACK : py_d_scale(col, 150), 1);
}

void py_d_frame(int x, int y, int w, int h, uint16_t c)
{
    gfx_rect(x, y, w, 1, c);
    gfx_rect(x, y + h - 1, w, 1, c);
    gfx_rect(x, y, 1, h, c);
    gfx_rect(x + w - 1, y, 1, h, c);
}

static void dashed_frame(int x, int y, int w, int h, uint16_t c)
{
    int i;

    for (i = 0; i < w; i += 4) {
        gfx_rect(x + i, y, i + 2 > w ? 1 : 2, 1, c);
        gfx_rect(x + i, y + h - 1, i + 2 > w ? 1 : 2, 1, c);
    }
    for (i = 0; i < h; i += 4) {
        gfx_rect(x, y + i, 1, i + 2 > h ? 1 : 2, c);
        gfx_rect(x + w - 1, y + i, 1, i + 2 > h ? 1 : 2, c);
    }
}

int py_d_chip3(int x, int y, const char *s, uint16_t c)
{
    int w = py_d_w3(s, 1) + 6;

    py_d_frame(x, y, w, 9, c);
    py_d_text3(x + 3, y + 2, s, c, 1);
    return w;
}

int py_d_chip(int x, int y, const char *s, uint16_t c)
{
    int w = py_d_w7(s, 0) + 6;

    py_d_frame(x, y, w, 11, c);
    gfx_text7(x + 3, y + 2, s, c);
    return w;
}

/* small triangles: right (dir 0), up (1), down (2), left (3); n = rows */
static void tri(int x, int y, int n, int dir, uint16_t c)
{
    int i;

    for (i = 0; i < n; i++) {
        int w = 2 * (dir == 2 ? n - 1 - i : i) + 1;

        if (dir == 0)
            gfx_rect(x + i, y + i, 1, 2 * (n - 1 - i) + 1, c);
        else if (dir == 3)
            gfx_rect(x + i, y + n - 1 - i, 1, 2 * i + 1, c);
        else
            gfx_rect(x + n - 1 - (w - 1) / 2, y + i, w, 1, c);
    }
}

/* ------------------------------------------------------------------ meters */

#define DB_FLOOR    (-60 * 256)                 /* levels in 1/256 dB */
#define DB_REL      11                          /* release a block (64 frames): ~30 dB/s */
#define DB_HOLD_REL 2                           /* the hold's fall a block: ~5.5 dB/s */

static int32_t m_db[PS_TRACKS] = { DB_FLOOR, DB_FLOOR, DB_FLOOR, DB_FLOOR, DB_FLOOR, DB_FLOOR, DB_FLOOR, DB_FLOOR };
static int32_t m_hold[PS_TRACKS] = { DB_FLOOR, DB_FLOOR, DB_FLOOR, DB_FLOOR, DB_FLOOR, DB_FLOOR, DB_FLOOR, DB_FLOOR };

/* 20 log10(pk / 32768) in 1/256 dB (log2 from the top bit and a parabola, within 0.05 dB) */
static int32_t lin_db(uint32_t pk)
{
    int e;
    int32_t db;
    uint32_t f, l;

    if (pk < 4)
        return DB_FLOOR;
    e = 31 - __builtin_clz(pk);
    f = e >= 16 ? (pk >> (e - 16)) & 0xffff : (pk << (16 - e)) & 0xffff;
    l = f + ((f * (65536 - f) >> 16) * 22713 >> 16);
    db = ((e - 15) * 65536 + (int32_t)l) / 16 * 963 / 2560;    /* x 6.0206 dB, in 1/256 dB */
    return db < DB_FLOOR ? DB_FLOOR : db;
}

/* every frame: the peak since the last one and the audio blocks since then (the fall follows
 * time, not the frame rate) */
void py_d_meter_feed(int k, uint32_t peak, uint32_t blocks)
{
    int32_t db = lin_db(peak), m;

    if (k < 0 || k >= PS_TRACKS)
        return;
    if (blocks > 400)
        blocks = 400;
    m = m_db[k] - DB_REL * (int32_t)blocks;
    m_db[k] = db > m ? db : m < DB_FLOOR ? DB_FLOOR : m;
    m = m_hold[k] - DB_HOLD_REL * (int32_t)blocks;
    m_hold[k] = m_db[k] > m ? m_db[k] : m < DB_FLOOR ? DB_FLOOR : m;
}

void py_d_meter_set(int k, int db16, int hold16)
{
    m_db[k] = db16 * 16;
    m_hold[k] = hold16 * 16;
}

/* segments lit for a level (1/256 dB), in 1/256 segment */
static int32_t lit256(int32_t db, int segs)
{
    int32_t v = (db + 48 * 256) * segs / 54;

    return v < 0 ? 0 : v > segs * 256 ? segs * 256 : v;
}

int py_d_meter_lit(int k, int segs)
{
    return (lit256(m_db[k], segs) + 128) >> 8;
}

void py_d_meter(int x, int y, int seg, int gap, int segs, int h, int k, int level_db, uint16_t col,
                int a, int marks)
{
    int32_t l = lit256(m_db[k], segs);
    int lit = l >> 8, part = l & 255, hold = (lit256(m_hold[k], segs) + 128) >> 8;
    int zero = (48 * segs + 27) / 54, i;
    int w = segs * (seg + gap) - gap, mx;

    for (i = 0; i < segs; i++) {
        uint16_t on = i >= zero ? D_RED : col, c;

        if (i < lit)
            c = on;
        else if (i == lit && part > 24)         /* the top segment partly: smooth movement */
            c = py_d_scale(on, part);
        else
            c = D_SEG_OFF;
        if (i == hold - 1 && hold > lit)
            c = D_TEXT;
        if (a < 256 && c != D_SEG_OFF)
            c = py_d_scale(c, a);
        gfx_rect(x + i * (seg + gap), y, seg, h, c);
    }
    if (!marks)
        return;
    mx = x + ((level_db + 48) * (w - 1) + 27) / 54;
    tri(mx - 1, y + h + 1, 2, 1, a < 256 ? py_d_scale(col, a) : col);
    if (marks > 1)
        gfx_rect(x + (48 * (w - 1) + 27) / 54, y + h + 1, 1, 2, D_DISABLED);
}

/* ------------------------------------------------------------------ header, footer */

void py_d_header(const py_hm *h)
{
    char t[16];
    int x, y = 6;

    x = py_d_text7(10, y, h->title, h->title_col, 2);
    if (h->num >= 0) {
        py_d_hex2(t, h->num);
        x = py_d_text7(x + 4, y, t, D_TEXT, 2);
    }
    if (h->ctx && !h->bare)
        py_d_text3(x + 6, y + 2, h->ctx, D_MUTED, 1);
    if (h->bare)
        goto right;
    x = h->row >= 0 ? 94 : 140;                 /* centre: BPM, (ROW,) BAR */
    py_d_text3(x, 9, "BPM", D_MUTED, 1);
    py_d_fmt(t, h->bpm);
    py_d_num(x + 18, 3, 10, t, D_TEXT);
    if (h->row >= 0) {
        py_d_text3(136, 9, "ROW", D_MUTED, 1);
        py_d_hex2(t, h->row);
        py_d_num(154, 3, 10, t, D_TEXT);
    }
    if (h->nobar)
        goto right;
    x = h->row >= 0 ? 174 : 186;
    py_d_text3(x, 9, "BAR", D_MUTED, 1);
    t[0] = (char)('0' + h->bar / 10 % 10);
    py_d_fmt(t + 1, h->bar % 10);
    if (h->bar >= 100)
        py_d_fmt(t, h->bar);
    {
        int k = 0;

        while (t[k])
            k++;
        t[k++] = '.';
        py_d_fmt(t + k, h->beat);
    }
    py_d_num(x + 18, 3, 10, t, D_TEXT);
right:
    if (h->loop_n >= 2) {                       /* the loop section, red: 04-07 X2 */
        int lx = 234;

        py_d_hex2(t, h->loop_a);
        t[2] = '-';
        py_d_hex2(t + 3, h->loop_b);
        lx = py_d_text3(lx, 7, t, D_RED, 1);
        t[0] = 'X';
        py_d_fmt(t + 1, h->loop_n);
        py_d_text3(lx + 3, 7, t, D_RED, 1);
    }
    if (h->state == 2) {                        /* rendering */
        eh_aa_ellipse(S16(281), S16(9), 56, 56, -1, 0, 128, 0, D_RED);
        py_d_text7(288, y, "REC", D_RED, 1);
    } else if (h->state == 1) {
        eh_aa_line(S16(279), S16(4), S16(279), S16(14), 8, D_GREEN);
        eh_aa_line(S16(279), S16(4), S16(286), S16(9), 8, D_GREEN);
        eh_aa_line(S16(279), S16(14), S16(286), S16(9), 8, D_GREEN);
        py_d_text7(290, y, "PLAY", D_GREEN, 1);
    } else {
        py_d_frame(278, 5, 9, 9, D_TEXT);
        py_d_text7(290, y, "STOP", D_TEXT, 1);
    }
    gfx_rect(0, D_HEAD_LINE, gfx_w, 1, D_LINE);
}

static int master(int db)
{
    char t[8];
    int x;

    py_d_ring(14, D_FOOT_Y + 2, 3, D_TEXT);
    gfx_rect(14, D_FOOT_Y - 2, 1, 3, D_TEXT);
    x = py_d_text3(21, D_FOOT_Y, "MASTER", D_TEXT, 1);
    py_d_fmt(t, db);
    x = py_d_text3(x + 3, D_FOOT_Y, t, D_TEXT, 1);
    return py_d_text3(x + 3, D_FOOT_Y, "DB", D_MUTED, 1);
}

static int hint_w(const py_hint *h)
{
    return (h->ring ? 9 : py_d_w3(h->key, 0) + 4) + py_d_w3(h->act, 0);
}

void py_d_footer(const py_hint *h, int n, int master_db, int with_master)
{
    int i, w = 0, x;

    gfx_rect(0, D_FOOT_LINE, gfx_w, 1, D_LINE);
    if (with_master)
        master(master_db);
    for (i = 0; i < n; i++)
        w += hint_w(&h[i]) + (i ? 6 : 0);
    x = 310 - w;
    for (i = 0; i < n; i++) {
        if (h[i].ring) {
            py_d_ring(x + 3, D_FOOT_Y + 2, 3, h[i].ring);
            x += 9;
        } else {
            x = py_d_text3(x, D_FOOT_Y, h[i].key, D_TEXT, 0) + 3;
        }
        x = py_d_text3(x, D_FOOT_Y, h[i].act, D_MUTED, 0) + 5;
    }
}

void py_d_footer_text(int x, const char *s, uint16_t c)
{
    gfx_rect(0, D_FOOT_LINE, gfx_w, 1, D_LINE);
    py_d_text3(x, D_FOOT_Y, s, c, 0);
}

void py_d_footer_render(const char *name, int pc)
{
    char t[8];
    int x;

    gfx_rect(0, D_FOOT_LINE, gfx_w, gfx_h - D_FOOT_LINE, GFX_BLACK);
    gfx_rect(0, D_FOOT_LINE, gfx_w, 1, D_LINE);
    x = py_d_text3(10, D_FOOT_Y, "RENDERING", D_RED, 1);
    x = py_d_text3(x + 4, D_FOOT_Y, name, D_TEXT, 1);
    py_d_fmt(t, pc);
    x = py_d_text3(x + 6, D_FOOT_Y, t, D_TEXT, 1);
    py_d_text3(x, D_FOOT_Y, "%", D_TEXT, 1);
    gfx_rect(150, D_FOOT_Y + 1, 100, 3, D_SEG_OFF);
    gfx_rect(150, D_FOOT_Y + 1, pc > 100 ? 100 : pc, 3, D_RED);
    py_d_text3(262, D_FOOT_Y, "STOP", D_TEXT, 0);
    py_d_text3(281, D_FOOT_Y, "CANCEL", D_MUTED, 0);
}

/* ------------------------------------------------------------------ the track list */

#define VIS         4                           /* lanes / rows on screen */
static uint8_t l_top;                           /* the first track shown */

static int window(const py_lm *m)
{
    int c = m->cursor >= m->n ? (m->n ? m->n - 1 : 0) : m->cursor, top = l_top;

    if (c < top)
        top = c;
    if (c >= top + VIS)
        top = c - VIS + 1;
    if (top > m->n - VIS)
        top = m->n - VIS;
    if (top < 0)
        top = 0;
    l_top = (uint8_t)top;
    return top;
}

/* the track is not heard: muted, or soloing elsewhere (it is drawn dimmed, as muted) */
static int quiet(const py_lm *m, const py_lt *t)
{
    int i;

    if (t->mute)
        return 1;
    if (t->solo)
        return 0;
    for (i = 0; i < m->n; i++)
        if (m->t[i].solo)
            return 1;
    return 0;
}

static uint16_t sel_bg(const py_lm *m, int i)
{
    return m->cut == i ? D_CUT_BG : m->insert ? D_INS_BG : D_SEL;
}

/* the 5x7 versions: M / S rings of 11 px, V2, ECO underlined */
static void button7(int cx, int cy, char letter, int on, uint16_t col)
{
    char t[2] = { letter, 0 };

    if (on)
        eh_aa_ellipse(S16(cx), S16(cy), S16(5) + 8, S16(5) + 8, -1, 0, 128, 0, col);
    else
        py_d_ring(cx, cy, 5, py_d_scale(col, 110));
    gfx_text7(cx - 2, cy - 3, t, on ? GFX_BLACK : py_d_scale(col, 150));
}

static void voices7(int x, int y, const py_lt *t, uint16_t c)
{
    char v[3] = { 'V', (char)('0' + t->voices), 0 };

    gfx_text7(x, y, v, c);
}

static void eco7(int x, int y, uint16_t c)      /* ECO: black on a green box, easy to spot */
{
    (void)c;
    gfx_rect(x - 1, y - 1, 19, 9, D_GREEN);
    gfx_text7(x, y, "ECO", GFX_BLACK);
}

static void cpu(int xr, int y, const py_lt *t, uint16_t c)
{
    char s[8];
    int n;

    if (t->cpu == 0xffff)
        return;
    py_d_fmt(s, (t->cpu + 5) / 10);
    for (n = 0; s[n]; n++)
        ;
    s[n] = '%';
    s[n + 1] = 0;
    py_d_text3(xr - py_d_w3(s, 0), y, s, t->cpu > 300 ? D_RED : c, 0);
}

/* the file name without its extension, at most n characters */
static void short_name(char *out, const char *s, int n)
{
    int i;

    for (i = 0; i < n && s[i] && s[i] != '.'; i++)
        out[i] = s[i];
    out[i] = 0;
}

/* a lane, top to bottom: dial + type, the engine (AUDIO: the file), the level and its meter,
 * SND n  PAT n (AUDIO: LOOP / ONCE / SONG), voices + ECO, M / S */
static void lane(const py_lm *m, int i, int x)
{
    const py_lt *t = &m->t[i];
    int q = quiet(m, t);
    uint16_t col = py_d_track_col(i), c = q ? py_d_scale(col, 90) : col;
    uint16_t half = py_d_scale(c, 128);         /* a pattern chosen, not playing yet; a chain row's sound */
    char s[16];
    int sel = m->cursor == i, w;

    if (sel) {
        gfx_rect(x + 1, 18, 63, D_FOOT_LINE - 18, sel_bg(m, i));
        gfx_rect(x + 1, 18, 63, 1, col);
    }
    py_d_fmt(s, i + 1);                         /* the number, then the type: one colour per lane */
    w = gfx_text7(x + 6, 23, s, c);
    py_d_text7(w + 4, 23, type_tag(t->type), c, 1);
    cpu(x + 58, 24, t, c);
    if (t->err) {
        py_d_text3(x + 6, 39, t->err, D_RED, 0);
    } else if (t->name) {
        short_name(s, t->name, 13);
        if (t->type != PS_AUDIO)                /* engine names keep their dots */
            for (w = 0; w < 13 && t->name[w]; w++)
                s[w] = t->name[w], s[w + 1] = 0;
        if (py_d_w7(s, 0) <= 56)
            gfx_text7(x + 6, 38, s, c);
        else
            py_d_text3(x + 6, 39, s, c, 0);
        if (t->under)
            py_d_text3(x + 56, 39, "!", D_RED, 0);
    }
    gfx_rect(x + 6, 49, 52, 1, D_LINE);
    py_d_fmt(s, t->level);
    w = py_d_num(x + 6, 55, 15, s, c);
    py_d_text3(w + 4, 66, "DB", c, 1);
    py_d_meter(x + 6, 76, 1, 1, 26, 4, t->k, t->level, col, q ? 90 : 256, 2);
    if (t->type == PS_AUDIO) {
        gfx_text7(x + 6, 94, t->amode == 2 ? "SONG" : t->amode ? "LOOP" : "ONCE", c);
    } else {                                    /* SND2 SNG5 */
        w = gfx_text7(x + 6, 94, t->type == PS_DRUM ? "KIT" : "SND", c);
        py_d_fmt(s, t->slot);
        w = gfx_text7(w + 1, 94, t->slot ? s : "-", t->slot_alt ? half : c);
        w = gfx_text7(w + 5, 94, t->seq_song ? "SNG" : "PAT", t->pend ? half : c);
        py_d_fmt(s, t->seq);
        gfx_text7(w + 1, 94, t->seq ? s : "--", t->pend ? half : c);
    }
    button7(x + 10, 115, 'M', t->mute, col);   /* M S V2 ECO */
    button7(x + 22, 115, 'S', t->solo, col);
    if (t->busy)
        gfx_text7(x + 30, 112, "BUSY", D_RED);
    else if (t->voices)
        voices7(x + 30, 112, t, c);
    if (t->eco && !t->busy)
        eco7(x + 44, 112, c);
}

static void add_lane(const py_lm *m, int x, int w)
{
    uint16_t tc = m->add_ok ? py_d_scale(type_col(m->add_type) == D_TEXT ? D_BLUE : type_col(m->add_type), 256)
                            : D_DISABLED;

    if (m->cursor == m->n) {
        gfx_rect(x + 1, 18, w - 1, D_FOOT_LINE - 18, m->insert ? D_INS_BG : D_SEL);
        gfx_rect(x + 1, 18, w - 1, 1, D_MUTED);
    }
    eh_aa_ellipse(S16(x + 10), S16(27), S16(4), S16(4), 8, 0, 128, 1, D_CTRL_OFF);
    py_d_text3(x + 9, 25, "+", D_MUTED, 0);
    py_d_text3(x + 19, 25, "ADD", D_MUTED, 1);
    py_d_text3(x + 6, 39, type_word(m->add_type), tc, 1);
    dashed_frame(x + 6, 49, w - 12, D_FOOT_LINE - 54, D_CTRL_OFF);
}

static void lanes(const py_lm *m)
{
    int top = window(m), i, x = 10;

    for (i = 0; i <= VIS; i++)
        gfx_rect(10 + i * 64, 18, 1, D_FOOT_LINE - 18, D_LANE);
    for (i = top; i < m->n && i < top + VIS; i++, x += 64)
        lane(m, i, x);
    if (m->n < VIS) {                           /* room: the ADD lane next to the tracks */
        if (m->add_room)
            add_lane(m, x, 64);
    } else if (top + VIS >= m->n && m->add_room) {
        add_lane(m, 10 + VIS * 64, 44);
    } else if (top + VIS < m->n) {              /* the next track, cut off at the edge */
        lane(m, top + VIS, 10 + VIS * 64);
    }
    if (top > 0)                                /* more tracks left of the window */
        tri(4, 23, 3, 3, D_MUTED);
}

/* ROWS: the column grid */
#define R_SRC       20                          /* the type */
#define R_SEQ       43                          /* the engine over the pattern */
#define R_VOI       101
#define R_MET       134
#define R_SEGS      58
#define R_LVL       254
#define R_Y0        20
#define R_H         24

static void row(const py_lm *m, int i, int yr)
{
    const py_lt *t = &m->t[i];
    int q = quiet(m, t);
    uint16_t col = py_d_track_col(i), c = q ? py_d_scale(col, 90) : col, half = py_d_scale(c, 128);
    int ym = yr + 11, x;
    char s[16];

    if (m->cursor == i) {
        gfx_rect(0, yr, gfx_w, R_H - 1, sel_bg(m, i));
        gfx_rect(0, yr, 2, R_H - 1, col);
    }
    gfx_rect(10, yr + R_H - 1, 300, 1, D_LANE);
    py_d_fmt(s, i + 1);                         /* the number: one colour per row */
    gfx_text7(10, ym - 3, s, c);
    gfx_text7(R_SRC, ym - 3, type_tag(t->type), c);    /* the type, as big as the number */
    if (t->type == PS_AUDIO) {                  /* the file over LOOP / ONCE / SONG */
        short_name(s, t->name ? t->name : "", 9);
        gfx_text7(R_SEQ, ym - 8, s, c);
        gfx_text7(R_SEQ, ym + 1, t->amode == 2 ? "SONG" : t->amode ? "LOOP" : "ONCE", c);
    } else {                                    /* the engine (DRUM: KIT n) over SNG n / PAT n */
        if (t->type == PS_DRUM) {
            x = gfx_text7(R_SEQ, ym - 8, "KIT", c);
            py_d_fmt(s, t->slot);
            gfx_text7(x + 1, ym - 8, t->slot ? s : "-", t->slot_alt ? half : c);
        } else if (t->name && py_d_w7(t->name, 0) <= 56) {
            gfx_text7(R_SEQ, ym - 8, t->name, t->slot_alt ? half : c);
        } else if (t->name) {
            py_d_text3(R_SEQ, ym - 7, t->name, t->slot_alt ? half : c, 0);
        }
        x = gfx_text7(R_SEQ, ym + 1, t->seq_song ? "SNG" : "PAT", t->pend ? half : c);
        py_d_fmt(s, t->seq);
        gfx_text7(x + 1, ym + 1, t->seq ? s : "--", t->pend ? half : c);
    }
    if (t->busy)
        py_d_text3(R_VOI, ym - 3, "BUSY", D_RED, 1);
    else if (t->voices)                         /* V2 over ECO */
        voices7(R_VOI, ym - 8, t, c);
    else
        gfx_rect(R_VOI, ym - 1, 5, 1, c);
    if (t->eco && !t->busy)
        eco7(R_VOI, ym + 1, c);
    cpu(R_MET - 5, ym - 7, t, c);
    if (t->err)
        py_d_text3(R_MET, ym - 2, t->err, D_RED, 1);
    else
        py_d_meter(R_MET, ym - 3, 1, 1, R_SEGS, 5, t->k, t->level, col, q ? 90 : 256, 1);
    if (t->under)
        py_d_text3(R_MET - 4, ym - 2, "!", D_RED, 0);
    py_d_fmt(s, t->level);
    x = py_d_num(R_LVL, ym - 5, 11, s, c);
    py_d_text3(x + 3, ym + 2, "DB", c, 1);
    py_d_button(294, ym, 'M', t->mute, col);
    py_d_button(306, ym, 'S', t->solo, col);
}

static void rows(const py_lm *m)
{
    int top = window(m), i, y = R_Y0;

    for (i = top; i < m->n && i < top + VIS; i++, y += R_H)
        row(m, i, y);
    if (top > 0)
        tri(312, R_Y0 + 1, 3, 1, D_MUTED);
    if (top + VIS < m->n)
        tri(312, y - 4, 3, 2, D_MUTED);
    if (top + VIS >= m->n && m->add_room) {     /* the ADD row */
        uint16_t tc = m->add_ok ? (m->add_type == PS_SYNTH ? D_BLUE : type_col(m->add_type)) : D_DISABLED;
        int x;

        if (m->cursor == m->n) {
            gfx_rect(0, y, gfx_w, 12, m->insert ? D_INS_BG : D_SEL);
            gfx_rect(0, y, 2, 12, D_MUTED);
        }
        dashed_frame(10, y + 2, 11, 8, D_CTRL_OFF);
        py_d_text3(14, y + 4, "+", D_MUTED, 0);
        x = py_d_text3(27, y + 4, "ADD", D_MUTED, 2);
        py_d_text3(x + 4, y + 4, type_word(m->add_type), tc, 2);
    }
}

/* the list's bottom line: only a note (an armed key, a short message), else nothing (v0.17: the
 * hints went, the panel's edge hides that part of the screen) */
static void list_footer(const py_lm *m)
{
    int x;

    if (!m->note)
        return;
    x = py_d_text3(10, D_FOOT_Y, m->note, m->note_col, 0);
    if (m->note2)
        py_d_text3(x + 4, D_FOOT_Y, m->note2, D_TEXT, 0);
}

void py_d_list(const py_lm *m)
{
    if (m->layout)
        rows(m);
    else
        lanes(m);
    list_footer(m);
}

/* ------------------------------------------------------------------ the song view */

#define S_X0        24                          /* the first column */
#define S_COLS      8                           /* all of them: up to 4 wide, 5..8 narrow */
#define S_RPT       312                         /* the RPT column's right edge */
#define S_Y0        32
#define S_RH        12
#define S_ROWS      8
#define S_NUM       9                           /* the cells' thin numerals (px) */

static int s_cw = 68;                           /* column width (py_d_song) */
static int s_solo;                              /* a column is soloed (py_d_song) */

static int col_quiet(const py_sc *c)
{
    return c->mute || (s_solo && !c->solo);
}

static void song_cell(const py_sm *m, int i, int ci, int sr, int x, int y)
{
    const py_sc *c = &m->c[ci];
    uint8_t v = m->s->cell[sr][c->k];
    uint16_t col = py_d_track_col(ci), ink;
    int here = c->head == sr, tx = x + 9;
    char s[3];

    if (col_quiet(c))
        col = py_d_scale(col, 90);
    if (here) {
        gfx_rect(x + 2, y + 1, s_cw - 7, S_RH - 2, py_d_scale(col, 64));
        tri(x + 4, y + 3, 3, 0, col);
    }
    ink = col;
    if (v == PS_EMPTY) {
        gfx_rect(tx, y + 5, 1, 1, D_EMPTY);
        gfx_rect(tx + 4, y + 5, 1, 1, D_EMPTY);
        gfx_rect(tx + 8, y + 5, 1, 1, D_EMPTY);
    } else if (c->type == PS_AUDIO) {
        py_d_num(tx, y, S_NUM, v == PS_ON ? "ON" : "OF", v == PS_ON ? ink : D_MUTED);
    } else if (v == PS_REST) {
        py_d_num(tx, y, S_NUM, "OF", D_MUTED);
    } else {
        py_d_hex2(s, v);
        py_d_num(tx, y, S_NUM, s, v < PS_CHAINS && py_song_chain_empty(m->s, v) ? D_DISABLED : ink);
    }
    if (m->playing && c->cue == sr && (m->blink & 8))     /* cued: it jumps here */
        py_d_frame(x + 1, y, s_cw - 5, S_RH, py_d_track_col(ci));
    if (sr == m->row && ci == m->col)
        py_d_frame(x + 1, y - 1, s_cw - 5, S_RH + 1, D_TEXT);
    (void)i;
}

static int song_window(const py_sm *m)
{
    int top = *m->top, left = *m->left;

    top = m->row < top ? m->row : m->row >= top + S_ROWS ? m->row - S_ROWS + 1 : top;
    *m->top = (uint8_t)top;
    left = m->col < left ? m->col : m->col >= left + S_COLS ? m->col - S_COLS + 1 : left;
    if (left > m->n - S_COLS)
        left = m->n - S_COLS;
    if (left < 0)
        left = 0;
    *m->left = (uint8_t)left;
    return top;
}

void py_d_song(const py_sm *m)
{
    int top, left, i, r;

    s_cw = m->n ? (S_RPT - 16 - S_X0) / m->n : 68;     /* the columns share the width */
    s_cw = s_cw > 136 ? 136 : s_cw;
    for (s_solo = 0, i = 0; i < m->n; i++)
        s_solo |= m->c[i].solo;
    top = song_window(m);
    left = *m->left;

    for (i = left; i < m->n && i < left + S_COLS; i++) {  /* the columns' names */
        const py_sc *c = &m->c[i];
        int x = S_X0 + (i - left) * s_cw;
        uint16_t col = py_d_track_col(i);
        char s[10];

        if (col_quiet(c))
            col = py_d_scale(col, 90);
        py_d_fmt(s, i + 1);                     /* the track's number alone (red: BUSY) */
        gfx_text7(x + 9, 21, s, c->starved ? D_RED : col);
        if (i == m->col)                        /* the cursor's column */
            gfx_rect(x + 9, 29, 5, 1, col);
        if (c->solo)
            py_d_text3(x + 17, 23, "S", col, 0);
    }
    py_d_text3(6, 23, "ROW", D_MUTED, 0);
    py_d_text3(S_RPT - py_d_w3("RPT", 1), 23, "RPT", D_MUTED, 1);
    if (left > 0)
        tri(S_X0 - 4, 23, 3, 3, D_MUTED);
    if (left + S_COLS < m->n)
        tri(S_RPT - 18, 23, 3, 0, D_MUTED);
    gfx_rect(2, 30, S_RPT - 2, 1, D_LINE);
    for (r = 0; r < S_ROWS; r++) {
        int y = S_Y0 + r * S_RH, sr = top + r;
        char s[4];

        if (sr >= PS_ROWS)
            break;
        if (sr == m->row) {
            gfx_rect(2, y - 1, S_RPT - 2, S_RH, D_PLAYROW);
            tri(5, y + 2, 3, 0, D_TEXT);
        }
        py_d_hex2(s, sr);
        gfx_text7(10, y + 1, s, sr == m->row ? D_TEXT : D_DISABLED);
        if (m->loop_n >= 2 && sr >= m->loop_a && sr <= m->loop_b) {      /* the loop: a red bracket */
            gfx_rect(2, y - 1, 1, S_RH, D_RED);
            if (sr == m->loop_a) {
                gfx_rect(2, y - 1, 3, 1, D_RED);
                s[0] = 'X';
                py_d_fmt(s + 1, m->loop_n);
                gfx_text7(S_RPT - py_d_w7(s, 0), y + 1, s, D_ORANGE);
            }
            if (sr == m->loop_b)
                gfx_rect(2, y + S_RH - 2, 3, 1, D_RED);
        }
        for (i = left; i < m->n && i < left + S_COLS; i++)
            song_cell(m, r, i, sr, S_X0 + (i - left) * s_cw, y);
    }
}
