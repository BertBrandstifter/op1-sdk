/* Host tests for DRUM ROLL's punch-in effects (plain cc; dr_dsp.c is included below). */
#include <stdio.h>
#include <string.h>
#include "dr_punch.h"
#include "dr_dsp.c"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void)
{
    dr_px_seg s[3];
    dr_px_voice v;
    dr_px_ctl c;
    static int16_t buf[128];
    int i, n, ok, mx;
    uint32_t a;

    /* REPEAT mapping: before t0 as it is; from t0 the slice, wrapping; contiguous over blocks */
    CHECK(dr_px_repeat(1000, 64, 5000, 2756, s) == 1 && s[0].src == 1000 && s[0].n == 64 && s[0].out == 0);
    n = dr_px_repeat(4980, 64, 5000, 2756, s);
    CHECK(n == 2 && s[0].src == 4980 && s[0].n == 20 && s[1].src == 5000 && s[1].n == 44 && s[1].out == 20);
    n = dr_px_repeat(5000 + 2756 - 10, 64, 5000, 2756, s);
    CHECK(n == 2 && s[0].src == 5000 + 2746 && s[0].n == 10 && s[1].src == 5000 && s[1].n == 54 && s[1].out == 10);
    for (a = 5000, ok = 1; a < 5000 + 40000; a += 64) {     /* every block: source times follow on */
        static uint32_t want = 5000;
        int k;

        n = dr_px_repeat(a, 64, 5000, 2756, s);
        for (k = 0; k < n; k++) {
            ok &= s[k].src == want && s[k].out <= 64;
            want = s[k].src + s[k].n == 5000 + 2756 ? 5000 : s[k].src + s[k].n;
        }
    }
    CHECK(ok);
    CHECK(dr_px_repeat(0, 64, 0, 0, s) == 1);   /* no length: as it is */

    /* GLITCH: pieces on the g grid; a piece's source is a whole piece of the bar (or itself), the
     * same for every block of that piece; contiguous inside a piece; some pieces as they are */
    {
        uint32_t g = 5512, span = 16 * 5512, src0[64], same = 0, moved = 0, b;
        int k;

        for (b = 0; b < 64; b++)
            src0[b] = 0xffffffffu;
        for (a = 0, ok = 1; a < 4 * span; a += 64) {
            n = dr_px_glitch(a, 64, g, span, 77, s);
            for (k = 0; k < n; k++) {
                uint32_t pc = (a + s[k].out) / g, at = (a + s[k].out) % g, base = s[k].src - at;

                ok &= base % g == 0 && s[k].n <= 64 && at + s[k].n <= g;
                ok &= (int32_t)(base - ((a + s[k].out) - (a + s[k].out) % span)) >= 0
                      && base - ((a + s[k].out) - (a + s[k].out) % span) < span;
                if (pc < 64) {
                    if (src0[pc] == 0xffffffffu) {
                        src0[pc] = base;
                        same += base == pc * g;
                        moved += base != pc * g;
                    }
                    ok &= src0[pc] == base;
                }
            }
        }
        CHECK(ok && same > 4 && moved > 30);
        CHECK(dr_px_glitch(100, 64, 0, span, 1, s) == 1 && s[0].src == 100);
    }

    /* REVERSE: the steps of a bar backwards from t0, inside a step forward; before t0 as it is */
    {
        int k, m;

        m = dr_px_reverse(100, 64, 5000, 1000, 4, s);
        CHECK(m == 1 && s[0].src == 100);
        m = dr_px_reverse(4980, 64, 5000, 1000, 4, s);      /* steps 0..3 -> 3 2 1 0 */
        CHECK(m == 2 && s[0].src == 4980 && s[0].n == 20 && s[1].src == 5000 + 3000 && s[1].n == 44 && s[1].out == 20);
        m = dr_px_reverse(5000 + 990, 64, 5000, 1000, 4, s);
        CHECK(m == 2 && s[0].src == 5000 + 3990 && s[0].n == 10 && s[1].src == 5000 + 2000 && s[1].n == 54);
        m = dr_px_reverse(5000 + 4000 + 3970, 64, 5000, 1000, 4, s);   /* bar 2, step 3 -> its step 0 */
        CHECK(m == 2 && s[0].src == 5000 + 4000 + 970 && s[1].src == 5000 + 8000 + 3000);
        for (k = 0, ok = 1; k < m; k++)
            ok &= s[k].n <= 64;
        CHECK(ok);
    }

    /* audio: everything off = untouched */
    memset(&c, 0, sizeof(c));
    dr_px_start(&v);
    for (i = 0; i < 128; i++)
        buf[i] = (int16_t)(i & 1 ? 10000 : -10000);
    dr_px_process(&v, buf, 128, &c);
    CHECK(buf[0] == -10000 && buf[1] == 10000 && buf[127] == 10000);
    /* LOW at a low cutoff damps Nyquist */
    dr_coef_set(&c.lp, DR_F_LP, 20, 40);
    for (n = 0, mx = 0; n < 8; n++) {
        for (i = 0; i < 128; i++)
            buf[i] = (int16_t)(i & 1 ? 10000 : -10000);
        dr_px_process(&v, buf, 128, &c);
    }
    for (i = 0; i < 128; i++)
        mx = buf[i] > mx ? buf[i] : -buf[i] > mx ? -buf[i] : mx;
    CHECK(mx < 500);
    c.lp.type = DR_F_OFF;
    dr_px_process(&v, buf, 128, &c);
    CHECK(v.lp.ic1 == 0 && v.lp.ic2 == 0);
    /* HIGH at a high cutoff takes DC away */
    dr_coef_set(&c.hp, DR_F_HP, 100, 20);
    for (n = 0; n < 20; n++) {
        for (i = 0; i < 128; i++)
            buf[i] = 12000;
        dr_px_process(&v, buf, 128, &c);
    }
    CHECK(buf[127] < 300 && buf[127] > -300);
    c.hp.type = DR_F_OFF;
    /* 8-BIT: 5 bits, every value held for 8 samples */
    c.crush = 1;
    for (i = 0; i < 128; i++)
        buf[i] = (int16_t)(i * 200);
    dr_px_process(&v, buf, 128, &c);
    for (i = 0, ok = 1; i < 128; i++)
        ok &= (buf[i] & 0x7ff) == 0 && buf[i] == (int16_t)(((i / 8) * 8 * 200) & DR_PX_CRUSH_MASK);
    CHECK(ok);
    c.crush = 0;
    /* GATE: open for the first half of the period, shut for the second (64-sample slews) */
    dr_px_start(&v);
    c.gate = 1;
    c.gper = 512;
    c.gpos = 0;
    for (n = 0, ok = 1; n < 4; n++) {
        for (i = 0; i < 128; i++)
            buf[i] = 10000;
        dr_px_process(&v, buf, 128, &c);
        c.gpos += 128;
        for (i = 0; i < 128; i++) {
            uint32_t ph = (n * 128 + i) % 512;

            if (ph < 256)
                ok &= ph < 64 || buf[i] > 9990 || n == 0;
            else
                ok &= ph < 256 + 64 || buf[i] == 0;
        }
    }
    CHECK(ok);
    c.gate = 0;                                 /* off: back to unity (slewed), then untouched */
    for (n = 0; n < 2; n++) {
        for (i = 0; i < 128; i++)
            buf[i] = 10000;
        dr_px_process(&v, buf, 128, &c);
    }
    CHECK(v.gg == 32767 && buf[127] == 10000);

    printf(fails ? "dr_punch: %d FAILED\n" : "dr_punch: all passed (%d)\n", fails ? fails : checks);
    return fails != 0;
}
