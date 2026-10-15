/* Host tests for the LOFI voice core (plain cc, built with lofi_core.c by tools/test_all.py). */
#include <stdio.h>
#include <string.h>
#include "lofi_core.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define SR      LF_SR                           /* TE's voice rate, 88.2 kHz */
#define N       SR                              /* one second */
static int16_t buf[N];

static uint32_t inc_hz(double hz) { return (uint32_t)(hz / SR * 4294967296.0); }

/* knob value in the middle of wave w's range */
static int16_t wave_knob(int w) { return (int16_t)(((2 * w + 1) * 32768L) / (2 * LF_WAVES)); }

static uint32_t incs[128];

static void fill(uint32_t inc)
{
    int i;

    for (i = 0; i < 128; i++)
        incs[i] = inc;
}

static void render(lf_voice *v, uint32_t inc, const int16_t *p, int n)
{
    int i;

    memset(v, 0, sizeof(*v));
    lf_note_on(v);
    fill(inc);
    for (i = 0; i < n; i += 128)                /* TE renders 128-sample blocks */
        lf_render(v, buf + i, n - i < 128 ? n - i : 128, incs, p);
}

/* rising zero crossings (with a little hysteresis) */
static int crossings(int from, int to)
{
    int i, n = 0, low = 1;

    for (i = from; i < to; i++) {
        if (low && buf[i] > 200) {
            n++;
            low = 0;
        } else if (buf[i] < -200) {
            low = 1;
        }
    }
    return n;
}

static int near(int a, int b, int tol) { return a >= b - tol && a <= b + tol; }

int main(void)
{
    lf_voice v;
    int16_t p[LF_PARAMS];
    int i, w, n, ok, distinct;

    /* WAVE knob covers every wave, ends included */
    CHECK(lf_wave(0) == LF_W_PULSE && lf_wave(32767) == LF_WAVES - 1);
    for (w = 0, ok = 1; w < LF_WAVES; w++)
        ok &= lf_wave(wave_knob(w)) == w;
    CHECK(ok);

    /* pitch: periodic waves play the note (441 Hz -> 441 cycles in one second) */
    memset(p, 0, sizeof(p));
    for (w = LF_W_PULSE; w <= LF_W_TABLE + LF_TABLES - 1; w++) {
        if (w == LF_W_NOISE)
            continue;
        p[LF_WAVE] = wave_knob(w);
        render(&v, inc_hz(441), p, N);
        n = crossings(0, N);
        if (w == LF_W_METAL) {
            /* the 93-step register loop repeats once per note period (200 samples at 441 Hz) */
            int same = 0, other = 0;

            for (i = 1000; i < N - 400; i++) {
                same += buf[i] != buf[i + SR / 441];
                other += buf[i] != buf[i + 37];
            }
            ok = same < (N - 1400) / 10 && other > (N - 1400) / 4;
            n = same;
        } else if (w >= LF_W_TABLE)
            ok = n >= 441 && n % 441 <= 2;              /* a table may cross more than once */
        else
            ok = near(n, 441, 1);
        if (!ok)
            printf("  wave %d: %d crossings\n", w, n);
        CHECK(ok);
    }

    /* per-sample increments are followed (TE's glide/bend/LFO): half the block an octave up */
    p[LF_WAVE] = wave_knob(LF_W_PULSE);
    memset(&v, 0, sizeof(v));
    lf_note_on(&v);
    for (i = 0; i < 128; i++)
        incs[i] = inc_hz(i < 64 ? 441 : 882);
    for (i = 0; i < N; i += 128)
        lf_render(&v, buf + i, N - i < 128 ? N - i : 128, incs, p);
    CHECK(near(crossings(0, N), (441 + 882) / 2, 2));

    /* level: chip waves stay within LF_LEVEL (+ rounding) */
    p[LF_WAVE] = wave_knob(LF_W_PULSE);
    render(&v, inc_hz(200), p, 4096);
    for (i = 0, ok = 1; i < 4096; i++)
        ok &= buf[i] >= -LF_LEVEL - 16 && buf[i] <= LF_LEVEL + 16;
    CHECK(ok);

    /* pulse duty: SHAPE narrows the pulse */
    p[LF_SHAPE] = 32767;
    render(&v, inc_hz(441), p, N);
    for (i = n = 0; i < N; i++)
        n += buf[i] > 0;
    CHECK(n < N / 16 && n > N / 64);
    p[LF_SHAPE] = 0;

    /* stepped triangle: 16 levels (NES); SHAPE max -> 2 levels */
    p[LF_WAVE] = wave_knob(LF_W_TRI);
    render(&v, inc_hz(100), p, N);
    {
        int16_t seen[64];
        int ns = 0, j;

        for (i = 0; i < N; i++) {
            for (j = 0; j < ns && seen[j] != buf[i]; j++)
                ;
            if (j == ns && ns < 64)
                seen[ns++] = buf[i];
        }
        CHECK(ns == 16);
    }

    /* bytebeat time runs at inc >> 11: C4 -> ~8.4 kHz, so "t & 255" is the key 3 octaves down */
    p[LF_WAVE] = wave_knob(LF_W_BYTE);
    render(&v, inc_hz(261.6256), p, N);
    CHECK(near((int)v.t, 8372, 2));
    /* one octave up: t twice as fast */
    render(&v, inc_hz(523.2511), p, N);
    CHECK(near((int)v.t, 16744, 3));

    /* every bytebeat formula makes sound (not silence) within 2 s */
    for (w = LF_W_BYTE, ok = 1; w < LF_WAVES; w++) {
        int lo = 0, hi = 0;

        p[LF_WAVE] = wave_knob(w);
        render(&v, inc_hz(261.6256), p, N);
        for (i = 0; i < N; i++) {
            if (buf[i] < lo) lo = buf[i];
            if (buf[i] > hi) hi = buf[i];
        }
        if (hi - lo < 4000) {
            printf("  byte %d: range %d\n", w - LF_W_BYTE, hi - lo);
            ok = 0;
        }
    }
    CHECK(ok);

    /* LOOP on bytebeat: the shortest window (2^5 t steps) repeats -> periodic output */
    p[LF_WAVE] = wave_knob(LF_W_BYTE);
    p[LF_LOOP] = 32767;
    render(&v, inc_hz(261.6256), p, N);
    {
        /* period = 32 t steps = 32 / 8372 s = ~168.6 samples: compare two periods far apart */
        int d = 0;

        for (i = 0; i < 1000; i++)
            d += buf[20000 + i] != buf[20000 + i + 1];
        CHECK(d > 50);                          /* still moving */
    }
    p[LF_LOOP] = 0;

    /* BITS max: one bit -> at most 3 distinct values (+-, 0 from rounding / smoothing) */
    p[LF_WAVE] = wave_knob(LF_W_SAW);
    p[LF_BITS] = 32767;
    render(&v, inc_hz(300), p, N);
    {
        int16_t a = buf[0], b = buf[0], c = buf[0];

        for (i = 0, distinct = 1; i < N; i++)
            if (buf[i] != a && buf[i] != b && buf[i] != c) {
                if (distinct == 1) b = buf[i]; else c = buf[i];
                distinct++;
            }
        CHECK(distinct <= 3);
    }
    p[LF_BITS] = 0;

    /* CRUSH max: runs of 128 equal samples (64 at 44.1 kHz) */
    p[LF_CRUSH] = 32767;
    render(&v, inc_hz(300), p, 1024);
    for (i = 1, ok = 1; i < 1024; i++)
        if (i % 128)
            ok &= buf[i] == buf[i - 1];
    CHECK(ok);
    p[LF_CRUSH] = 0;

    /* SWEEP: the pitch falls over the note */
    p[LF_WAVE] = wave_knob(LF_W_PULSE);
    p[LF_SWEEP] = 16384;
    render(&v, inc_hz(2000), p, N);
    CHECK(crossings(0, 2205) > 2 * crossings(N - 2205, N));
    p[LF_SWEEP] = 0;

    /* ARP (LOOP on chip waves) pattern 0/+12: two pitches alternate */
    p[LF_LOOP] = (int16_t)(32768 / 8 + 100);    /* pattern 1 */
    render(&v, inc_hz(441), p, N);
    n = crossings(0, N);
    CHECK(near(n, (441 + 882) / 2, 20));
    p[LF_LOOP] = 0;

    /* TONE darkens: fewer large steps */
    p[LF_WAVE] = wave_knob(LF_W_PULSE);
    p[LF_TONE] = 32767;
    render(&v, inc_hz(441), p, 4096);
    for (i = 1, ok = 1; i < 4096; i++)
        ok &= buf[i] - buf[i - 1] < 8000 && buf[i - 1] - buf[i] < 8000;
    CHECK(ok);
    p[LF_TONE] = 0;

    /* BEND changes a sine-like table, stays pitched */
    p[LF_WAVE] = wave_knob(LF_W_TABLE);
    p[LF_BEND] = 32767;
    render(&v, inc_hz(441), p, N);
    n = crossings(0, N);
    CHECK(n >= 441 && n % 441 <= 2);
    p[LF_BEND] = 0;

    /* note on restarts the voice: two notes render identically */
    p[LF_WAVE] = wave_knob(LF_W_BYTE + 3);
    p[LF_SWEEP] = 9000;
    {
        static int16_t first[4096];

        render(&v, inc_hz(300), p, 4096);
        memcpy(first, buf, sizeof(first));
        lf_note_on(&v);
        fill(inc_hz(300));
        for (i = 0; i < 4096; i += 128)
            lf_render(&v, buf + i, 128, incs, p);
        CHECK(memcmp(first, buf, sizeof(first)) == 0);
        p[LF_SWEEP] = 0;
    }

    /* extremes: every knob at both ends, very low/high pitch, odd block sizes: no crash, sane */
    {
        static const uint32_t ext[] = { 0, 1, 97391, 0x7fffffff, 0xffffffffu };
        int k, j;

        for (k = 0; k < 5; k++)
            for (j = 0; j < 64; j++) {
                for (i = 0; i < LF_PARAMS; i++)
                    p[i] = (int16_t)(((j >> (i % 6)) & 1) ? 32767 : (j & 1 ? -5 : 0));
                p[LF_WAVE] = wave_knob(j % LF_WAVES);
                memset(&v, 0, sizeof(v));
                lf_note_on(&v);
                fill(ext[k]);
                lf_render(&v, buf, 77, incs, p);
                lf_render(&v, buf, 128, incs, p);
            }
        CHECK(1);
    }

    printf("%d/%d checks passed\n%s\n", checks - fails, checks, fails ? "FAIL" : "PASS");
    return fails != 0;
}
