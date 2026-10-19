/* DRUM ROLL punch-in effects (dr_punch.h). */
#include "dr_punch.h"

const char *const dr_px_name[DR_PX_KEYS] = {
    "FILL", "RETRIG", "RETRIG FAST", "GLITCH", "RISE",
    "LP SWEEP", "HP SWEEP", "BLINDS", "SOLO 1", "SOLO 1+2",
    "8-BIT", "LASER", "FALL", "COIN", "RND PITCH",
    "OCT UP", "OCT DOWN", "ECHO", "REVERSE", "SHUFFLE",
    "HALF TIME", "DOUBLE TIME", "TRIPLET", "LOOP BEAT", "BUILD",
    "SOLO 1-4", "SOLO 5-8", "NO KICK", "FADE", "CUT",
};
const uint8_t dr_px_icon[DR_PX_KEYS][8] = {
    { 0x00, 0x44, 0x00, 0x11, 0x00, 0x88, 0x22, 0x00 },     /* FILL: scattered hits */
    { 0x00, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x00 },     /* RETRIG: the same hit again */
    { 0x00, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0x00 },     /* RETRIG FAST */
    { 0x00, 0x60, 0x66, 0x06, 0xc6, 0xd8, 0x18, 0x00 },     /* GLITCH: cut-up blocks */
    { 0x01, 0x03, 0x07, 0x0f, 0x1f, 0x3f, 0x7f, 0xff },     /* RISE: a rising roll */
    { 0xf8, 0x04, 0x02, 0x02, 0x01, 0x01, 0x01, 0x00 },     /* LP SWEEP: the low-pass curve */
    { 0x1f, 0x20, 0x40, 0x40, 0x80, 0x80, 0x80, 0x00 },     /* HP SWEEP: the high-pass curve */
    { 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00 },     /* BLINDS: slats */
    { 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00 },     /* SOLO 1: one lane */
    { 0x00, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0x00 },     /* SOLO 1+2 */
    { 0xf0, 0xf0, 0x0f, 0x0f, 0xf0, 0xf0, 0x0f, 0x0f },     /* 8-BIT: big pixels */
    { 0xc0, 0x30, 0x0c, 0x03, 0xc0, 0x30, 0x0c, 0x03 },     /* LASER: zaps */
    { 0x80, 0xc0, 0xe0, 0xf0, 0xf8, 0xfc, 0xfe, 0xff },     /* FALL: a falling roll */
    { 0x3c, 0x42, 0x99, 0xa5, 0xa5, 0x99, 0x42, 0x3c },     /* COIN */
    { 0x20, 0x02, 0x80, 0x10, 0x01, 0x40, 0x08, 0x00 },     /* RND PITCH */
    { 0x18, 0x3c, 0x7e, 0xff, 0x18, 0x18, 0x18, 0x18 },     /* OCT UP */
    { 0x18, 0x18, 0x18, 0x18, 0xff, 0x7e, 0x3c, 0x18 },     /* OCT DOWN */
    { 0xff, 0x00, 0x7e, 0x00, 0x3c, 0x00, 0x18, 0x00 },     /* ECHO: fading repeats */
    { 0x10, 0x30, 0x7f, 0xff, 0x7f, 0x30, 0x10, 0x00 },     /* REVERSE: arrow left */
    { 0xc0, 0xc0, 0x00, 0x30, 0x30, 0x00, 0x0c, 0x0c },     /* SHUFFLE: swung steps */
    { 0x00, 0xf0, 0xf0, 0x00, 0x00, 0xf0, 0xf0, 0x00 },     /* HALF TIME */
    { 0x00, 0xcc, 0xcc, 0x00, 0x00, 0xcc, 0xcc, 0x00 },     /* DOUBLE TIME */
    { 0x00, 0x92, 0x92, 0x92, 0x92, 0x92, 0x92, 0x00 },     /* TRIPLET */
    { 0xff, 0x81, 0x81, 0x99, 0x99, 0x81, 0x81, 0xff },     /* LOOP BEAT */
    { 0x80, 0x80, 0xa0, 0xa0, 0xa8, 0xa8, 0xaa, 0xaa },     /* BUILD: faster and faster */
    { 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0xff },     /* SOLO 1-4 */
    { 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0xff, 0xff },     /* SOLO 5-8 */
    { 0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81 },     /* NO KICK */
    { 0xff, 0xfe, 0xfc, 0xf8, 0xf0, 0xe0, 0xc0, 0x80 },     /* FADE */
    { 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x00 },     /* CUT */
};

int dr_px_repeat(uint32_t a, uint32_t n, uint32_t t0, uint32_t len, dr_px_seg seg[3])
{
    int k = 0;
    uint32_t end = a + n, at, m;

    if ((int32_t)(end - t0) <= 0 || !len) {     /* the slice has not started: as it is */
        seg[0].src = a;
        seg[0].n = n;
        seg[0].out = 0;
        return 1;
    }
    if ((int32_t)(t0 - a) > 0) {                /* up to t0 as it is */
        seg[k].src = a;
        seg[k].n = t0 - a;
        seg[k].out = 0;
        k++;
        a = t0;
    }
    while (a < end) {                           /* the slice, wrapping */
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

int dr_px_glitch(uint32_t a, uint32_t n, uint32_t g, uint32_t span, uint32_t seed, dr_px_seg seg[2])
{
    uint32_t end = a + n, k = 0, places = g && span >= g ? span / g : 1;

    while (a < end && g) {
        uint32_t b = a / g, at = a - b * g, m = g - at < end - a ? g - at : end - a, r = mix(seed ^ b * 0x9e3779b1u);

        seg[k].src = (r & 3) == 0 || places < 2 ? a : (a - a % span) + ((r >> 2) % places) * g + at;
        seg[k].n = m;
        seg[k].out = a - (end - n);
        k++;
        a += m;
    }
    if (!g) {
        seg[0].src = a;
        seg[0].n = n;
        seg[0].out = 0;
        k = 1;
    }
    return (int)k;
}

int dr_px_reverse(uint32_t a, uint32_t n, uint32_t t0, uint32_t step, uint32_t steps, dr_px_seg seg[3])
{
    uint32_t end = a + n, k = 0, span = step * steps;

    if ((int32_t)(end - t0) <= 0 || !step || !steps) {
        seg[0].src = a;
        seg[0].n = n;
        seg[0].out = 0;
        return 1;
    }
    if ((int32_t)(t0 - a) > 0) {                /* up to t0 as it is */
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

void dr_px_start(dr_px_voice *v)
{
    dr_voice_start(&v->lp, 0, DR_UNITY, -1);
    dr_voice_start(&v->hp, 0, DR_UNITY, -1);
    v->gg = 32767;
    v->hold = 0;
    v->hn = 0;
}

void dr_px_process(dr_px_voice *v, int16_t *out, int n, const dr_px_ctl *c)
{
    int i;

    if (c->lp.type != DR_F_OFF)
        dr_process(&v->lp, out, n, &c->lp);
    else
        v->lp.ic1 = v->lp.ic2 = 0;              /* fresh when it comes back */
    if (c->hp.type != DR_F_OFF)
        dr_process(&v->hp, out, n, &c->hp);
    else
        v->hp.ic1 = v->hp.ic2 = 0;
    if (c->crush) {                             /* 8-BIT: 5 bits, every value held 8 samples */
        int16_t hv = v->hold;
        int hn = v->hn;

        for (i = 0; i < n; i++) {
            if (!hn)
                hv = (int16_t)(out[i] & DR_PX_CRUSH_MASK);
            hn = hn + 1 == DR_PX_CRUSH_HOLD ? 0 : hn + 1;
            out[i] = hv;
        }
        v->hold = hv;
        v->hn = (uint8_t)hn;
    }
    if (c->gate || v->gg != 32767) {
        int32_t g = v->gg;
        uint32_t ph = c->gper ? c->gpos % c->gper : 0, half = c->gper / 2;

        for (i = 0; i < n; i++) {
            int32_t want = !c->gate || ph < half ? 32767 : 0;

            g = g < want ? (g + DR_PX_GATE_SLEW > want ? want : g + DR_PX_GATE_SLEW)
                         : (g - DR_PX_GATE_SLEW < want ? want : g - DR_PX_GATE_SLEW);
            out[i] = (int16_t)((out[i] * g) >> 15);
            if (++ph >= c->gper)
                ph = 0;
        }
        v->gg = g;
    }
}
