/* NOTE ROLL punch-in effects (nr_punch.h). */
#include "nr_punch.h"

const char *const nr_px_name[NR_PX_KEYS] = {
    "FILL", "RETRIG", "RETRIG FAST", "GLITCH", "RISING ARP",
    "OCT UP", "OCT DOWN", "STACCATO", "BLINDS", "RANDOM",
    "CHIP ARP UP", "CHIP ARP DN", "POWER UP", "POWER DOWN", "COIN",
    "TRILL", "ECHO", "REVERSE", "OCT ARP", "LASER",
    "HALF TIME", "DOUBLE TIME", "TRIPLET", "LOOP BEAT", "BUILD",
    "SOLO CHORDS", "SOLO LEAD", "SOLO BASS", "SOLO MELODY", "FADE",
};
const uint8_t nr_px_icon[NR_PX_KEYS][8] = {
    { 0x00, 0x44, 0x00, 0x11, 0x00, 0x88, 0x22, 0x00 },     /* FILL */
    { 0x00, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x00 },     /* RETRIG */
    { 0x00, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0x00 },     /* RETRIG FAST */
    { 0x00, 0x60, 0x66, 0x06, 0xc6, 0xd8, 0x18, 0x00 },     /* GLITCH */
    { 0x01, 0x03, 0x07, 0x0f, 0x1f, 0x3f, 0x7f, 0xff },     /* RISING ARP */
    { 0x18, 0x3c, 0x7e, 0xff, 0x18, 0x18, 0x18, 0x18 },     /* OCT UP */
    { 0x18, 0x18, 0x18, 0x18, 0xff, 0x7e, 0x3c, 0x18 },     /* OCT DOWN */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x66, 0x66, 0x00 },     /* STACCATO */
    { 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00 },     /* BLINDS */
    { 0x20, 0x02, 0x80, 0x10, 0x01, 0x40, 0x08, 0x00 },     /* RANDOM */
    { 0x03, 0x03, 0x0c, 0x0c, 0x30, 0x30, 0xc0, 0xc0 },     /* CHIP ARP UP */
    { 0xc0, 0xc0, 0x30, 0x30, 0x0c, 0x0c, 0x03, 0x03 },     /* CHIP ARP DN */
    { 0x01, 0x03, 0x07, 0x0f, 0x1f, 0x3f, 0x7f, 0xff },     /* POWER UP */
    { 0x80, 0xc0, 0xe0, 0xf0, 0xf8, 0xfc, 0xfe, 0xff },     /* POWER DOWN */
    { 0x3c, 0x42, 0x99, 0xa5, 0xa5, 0x99, 0x42, 0x3c },     /* COIN */
    { 0xcc, 0xcc, 0x33, 0x33, 0xcc, 0xcc, 0x33, 0x33 },     /* TRILL */
    { 0xff, 0x00, 0x7e, 0x00, 0x3c, 0x00, 0x18, 0x00 },     /* ECHO */
    { 0x10, 0x30, 0x7f, 0xff, 0x7f, 0x30, 0x10, 0x00 },     /* REVERSE */
    { 0xf0, 0xf0, 0x00, 0x00, 0x0f, 0x0f, 0x00, 0x00 },     /* OCT ARP */
    { 0xc0, 0x30, 0x0c, 0x03, 0xc0, 0x30, 0x0c, 0x03 },     /* LASER */
    { 0x00, 0xf0, 0xf0, 0x00, 0x00, 0xf0, 0xf0, 0x00 },     /* HALF TIME */
    { 0x00, 0xcc, 0xcc, 0x00, 0x00, 0xcc, 0xcc, 0x00 },     /* DOUBLE TIME */
    { 0x00, 0x92, 0x92, 0x92, 0x92, 0x92, 0x92, 0x00 },     /* TRIPLET */
    { 0xff, 0x81, 0x81, 0x99, 0x99, 0x81, 0x81, 0xff },     /* LOOP BEAT */
    { 0x80, 0x80, 0xa0, 0xa0, 0xa8, 0xa8, 0xaa, 0xaa },     /* BUILD */
    { 0x00, 0xee, 0x00, 0xee, 0x00, 0xee, 0x00, 0x00 },     /* SOLO CHORDS */
    { 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },     /* SOLO LEAD */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff },     /* SOLO BASS */
    { 0x00, 0x00, 0x30, 0x00, 0x0c, 0x00, 0x03, 0x00 },     /* SOLO MELODY */
    { 0xff, 0xfe, 0xfc, 0xf8, 0xf0, 0xe0, 0xc0, 0x80 },     /* FADE */
};

int nr_px_repeat(uint32_t a, uint32_t n, uint32_t t0, uint32_t len, nr_px_seg seg[3])
{
    int k = 0;
    uint32_t end = a + n, at, m;

    if ((int32_t)(end - t0) <= 0 || !len) {
        seg[0].src = a;
        seg[0].n = n;
        seg[0].out = 0;
        return 1;
    }
    if ((int32_t)(t0 - a) > 0) {
        seg[k].src = a;
        seg[k].n = t0 - a;
        seg[k].out = 0;
        k++;
        a = t0;
    }
    while (a < end) {
        at = (a - t0) % len;
        m = len - at < end - a ? len - at : end - a;
        seg[k].src = t0 + at;
        seg[k].n = m;
        seg[k].out = a - (end - n);
        k++;
        a += m;
    }
    return k;
}

static uint32_t mix(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

int nr_px_glitch(uint32_t a, uint32_t n, uint32_t g, uint32_t span, uint32_t seed, nr_px_seg seg[2])
{
    uint32_t end = a + n, k = 0, places = g && span >= g ? span / g : 1;

    if (!g) {
        seg[0].src = a;
        seg[0].n = n;
        seg[0].out = 0;
        return 1;
    }
    while (a < end) {
        uint32_t b = a / g, at = a - b * g, m = g - at < end - a ? g - at : end - a, r = mix(seed ^ b * 0x9e3779b1u);

        seg[k].src = (r & 3) == 0 || places < 2 ? a : (a - a % span) + ((r >> 2) % places) * g + at;
        seg[k].n = m;
        seg[k].out = a - (end - n);
        k++;
        a += m;
    }
    return (int)k;
}

int nr_px_reverse(uint32_t a, uint32_t n, uint32_t t0, uint32_t step, uint32_t steps, nr_px_seg seg[3])
{
    uint32_t end = a + n, k = 0, span = step * steps;

    if ((int32_t)(end - t0) <= 0 || !step || !steps) {
        seg[0].src = a;
        seg[0].n = n;
        seg[0].out = 0;
        return 1;
    }
    if ((int32_t)(t0 - a) > 0) {
        seg[k].src = a;
        seg[k].n = t0 - a;
        seg[k].out = 0;
        k++;
        a = t0;
    }
    while (a < end) {
        uint32_t rel = a - t0, bar = rel / span, j = rel % span / step, at = rel % step;
        uint32_t m = step - at < end - a ? step - at : end - a;

        seg[k].src = t0 + bar * span + (steps - 1 - j) * step + at;
        seg[k].n = m;
        seg[k].out = a - (end - n);
        k++;
        a += m;
    }
    return (int)k;
}

static int shift(int key, int d)
{
    return key + d < 0 || key + d > 127 ? key : key + d;
}

static int put(nr_px_pulse *o, int n, int max, int key, int vel, uint32_t delay, uint32_t hold)
{
    if (n >= max)
        return n;
    o[n].key = key < 0 ? 0 : key > 127 ? 127 : key;
    o[n].vel = vel < 1 ? 1 : vel;
    o[n].delay = delay;
    o[n].hold = hold < 64 ? 64 : hold;
    return n + 1;
}

#define HELD(fx)    ((held >> (fx)) & 1)

int nr_px_notes(uint32_t held, int key, int vel, int voice, int voices, uint32_t span, uint32_t hold,
                uint32_t r, int fade, nr_px_pulse *o, int max)
{
    uint32_t solo = held & (1u << NR_PX_SCHORDS | 1u << NR_PX_SLEAD | 1u << NR_PX_SBASS | 1u << NR_PX_SMELODY);
    int up = HELD(NR_PX_OCTUP), dn = HELD(NR_PX_OCTDN), n = 0, k, m, first;
    uint32_t half = span / 2, total;

    if (solo && !((HELD(NR_PX_SCHORDS) && voices > 1) || (HELD(NR_PX_SLEAD) && voice == voices - 1)
                  || (HELD(NR_PX_SBASS) && voice == 0) || (HELD(NR_PX_SMELODY) && voices == 1)))
        return 0;
    if (up != dn)
        key = shift(key, up ? 12 : -12);
    if (HELD(NR_PX_RAND)) {
        uint32_t x = r % 10;

        key = shift(key, x < 3 ? 12 : x < 5 ? -12 : x < 7 ? 7 : 0);
    }
    vel = vel * fade / 256;
    if (vel < 1 || max < 1)
        return 0;
    total = hold > span ? hold : span;
    if ((HELD(NR_PX_CHIPUP) || HELD(NR_PX_CHIPDN)) && half) {
        int turn = HELD(NR_PX_CHIPUP) ? voice : voices - 1 - voice;   /* the chord's notes in turn */

        m = (int)(total / half);
        m = m < 1 ? 1 : m > 16 ? 16 : m;
        for (k = 0; k < m; k++)
            if (voices < 2)
                n = put(o, n, max, k & 1 ? shift(key, 12) : key, vel, (uint32_t)k * half, half - half / 8);
            else if (k % voices == turn)
                n = put(o, n, max, key, vel, (uint32_t)k * half, half - half / 8);
    } else if (HELD(NR_PX_TRILL) && half) {
        m = (int)(total / half);
        m = m < 1 ? 1 : m > 16 ? 16 : m;
        for (k = 0; k < m; k++)
            n = put(o, n, max, k & 1 ? shift(key, 2) : key, vel, (uint32_t)k * half, half - half / 8);
    } else if (HELD(NR_PX_ARP) && voices > 1) {
        n = put(o, n, max, key, vel, span * (uint32_t)voice / (uint32_t)voices, span / (uint32_t)voices);
    } else {
        if (HELD(NR_PX_STAC) && hold > span / 4)
            hold = span / 4;
        if (HELD(NR_PX_BLINDS) && span >= 4) {  /* pulses while the note lasts, at most 4 */
            m = (int)((hold + span / 4 - 1) / (span / 4));
            m = m < 1 ? 1 : m > 4 ? 4 : m;
            for (k = 0; k < m; k++)
                n = put(o, n, max, key, vel, (uint32_t)k * (span / 4), span / 8);
        } else {
            n = put(o, n, max, key, vel, 0, hold);
        }
    }
    if (!n)
        return 0;
    first = 0;
    if (HELD(NR_PX_COIN))
        n = put(o, n, max, shift(o[first].key, 12), vel, o[first].delay + span / 4, span / 4);
    if (HELD(NR_PX_OCTARP))
        n = put(o, n, max, shift(o[first].key, 12), vel, o[first].delay + span / 2, span / 2);
    if (HELD(NR_PX_ECHO)) {
        n = put(o, n, max, o[first].key, vel * 6 / 10, o[first].delay + 3 * span, o[first].hold);
        n = put(o, n, max, o[first].key, vel * 3 / 10, o[first].delay + 6 * span, o[first].hold);
    }
    return n;
}
