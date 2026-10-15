/* Host tests for the WHEEL voice core (plain cc, built with wheel_core.c by tools/test_all.py). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wheel_core.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define SR      WH_SR
#define N       SR                              /* one second */
static int16_t buf[N];
static uint32_t incs[128];

static uint32_t inc_hz(double hz) { return (uint32_t)(hz / SR * 4294967296.0); }

/* knob value in the middle of zone z of n */
static int16_t zone_knob(int z, int n) { return (int16_t)(((2 * z + 1) * 32768L) / (2 * n)); }

static void params(int16_t *p, int reg, int perc, int rot)
{
    p[WH_REG] = zone_knob(reg, WH_REGS);
    p[WH_PERC] = zone_knob(perc, WH_PERCS);
    p[WH_DRIVE] = 0;
    p[WH_ROTARY] = zone_knob(rot, WH_ROTS);
    p[WH_BASS] = p[WH_BODY] = p[WH_TOP] = 16384;   /* offsets 0 */
    p[WH_CLICK] = 0;
}

/* render n samples of one note in 128-sample blocks, the rotor advanced once per block */
static void render(wh_voice *v, wh_rotor *r, double hz, const int16_t *p, int n)
{
    int i, k;

    memset(v, 0, sizeof(*v));
    wh_note_on(v);
    for (k = 0; k < 128; k++)
        incs[k] = inc_hz(hz);
    for (i = 0; i < n; i += 128) {
        wh_rotor_block(r, p, 128);
        wh_render(v, r, buf + i, n - i < 128 ? n - i : 128, incs, p);
    }
}

/* amplitude of the hz component in buf[from, from + len) (Goertzel, Hann window) */
static double amp(double hz, int from, int len)
{
    double w = 2 * M_PI * hz / SR, c = 2 * cos(w), s0, s1 = 0, s2 = 0, re, im;
    int i;

    for (i = 0; i < len; i++) {
        double win = 0.5 - 0.5 * cos(2 * M_PI * i / (len - 1));

        s0 = buf[from + i] * win + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    re = s1 - s2 * cos(w);
    im = s2 * sin(w);
    return sqrt(re * re + im * im) / (len * 0.25);
}

static int peak(int from, int to)
{
    int i, m = 0;

    for (i = from; i < to; i++)
        if (abs(buf[i]) > m)
            m = abs(buf[i]);
    return m;
}

static double rms(int from, int len)
{
    double s = 0;
    int i;

    for (i = from; i < from + len; i++)
        s += (double)buf[i] * buf[i];
    return sqrt(s / len);
}

static double db(double a, double b) { return 20 * log10((a + 1e-9) / (b + 1e-9)); }

enum { FLUTE = 0, JAZZ = 3, CLARINET = 6, FULL = 15 };

int main(void)
{
    static int16_t copy[N];
    static const double ratio[WH_BARS] = { 0.5, 1.5, 1, 2, 3, 4, 5, 6, 8 };
    wh_voice v;
    wh_rotor r;
    int16_t p[WH_PARAMS];
    int i, b, ok;
    double a1, a2, a3, lo, hi;

    /* knob zones, names, offsets */
    CHECK(wh_reg(0) == 0 && wh_reg(32767) == WH_REGS - 1 && wh_reg(-5) == 0);
    CHECK(wh_perc(0) == 0 && wh_perc(32767) == WH_PERCS - 1);
    CHECK(wh_rot(0) == 0 && wh_rot(16384) == 1 && wh_rot(32767) == 2);
    CHECK(wh_offset(16384) == 0 && wh_offset(0) == -8 && wh_offset(32767) == 8);
    CHECK(!strcmp(wh_reg_name(JAZZ), "JAZZ") && !strcmp(wh_perc_name(2), "3RD")
          && !strcmp(wh_rot_name(2), "FAST"));
    params(p, JAZZ, 0, 0);
    CHECK(wh_bar(p, 0) == 8 && wh_bar(p, 2) == 8 && wh_bar(p, 3) == 0);
    p[WH_BASS] = 0;                             /* -8: 16' and 5 1/3' off */
    p[WH_TOP] = 32767;                          /* +8: 2 2/3' .. 1' full */
    CHECK(wh_bar(p, 0) == 0 && wh_bar(p, 1) == 0 && wh_bar(p, 2) == 8 && wh_bar(p, 8) == 8);

    /* FLUTE (8' only), nothing else: a clean sine at the note, peak ~WH_LEVEL */
    memset(&r, 0, sizeof(r));
    params(p, FLUTE, 0, 0);
    render(&v, &r, 440, p, N);
    a1 = amp(440, 8820, 8192);
    a2 = amp(880, 8820, 8192);
    a3 = amp(1320, 8820, 8192);
    printf("flute 440: amp %.0f, peak %d, h2 %.1f dB, h3 %.1f dB\n", a1, peak(8820, N),
           db(a2, a1), db(a3, a1));
    CHECK(fabs(a1 - WH_LEVEL) < WH_LEVEL * 0.05);
    CHECK(peak(8820, N) > WH_LEVEL * 0.95 && peak(8820, N) < WH_LEVEL * 1.05);
    CHECK(db(a2, a1) < -50 && db(a3, a1) < -50);   /* the fast sine is clean enough */
    CHECK(amp(400, 8820, 8192) < a1 * 0.01);        /* and it is at 440, not near it */

    /* FULL: all nine bars at the drawbar ratios, equal levels */
    params(p, FULL, 0, 0);
    render(&v, &r, 220, p, N);
    lo = 1e9;
    hi = 0;
    for (b = 0; b < WH_BARS; b++) {
        a1 = amp(220 * ratio[b], 8820, 16384);
        lo = a1 < lo ? a1 : lo;
        hi = a1 > hi ? a1 : hi;
    }
    printf("full 220: bars %.0f .. %.0f, peak %d\n", lo, hi, peak(0, N));
    CHECK(lo > 2000 && hi < lo * 1.12);
    CHECK(amp(220 * 7, 8820, 16384) < lo * 0.01);  /* no 7th harmonic in a tonewheel organ */
    CHECK(peak(0, N) <= 32767 && peak(0, N) > 12000);

    /* CLARINET 008080800: odd harmonics of the 8' only */
    params(p, CLARINET, 0, 0);
    render(&v, &r, 220, p, N);
    a1 = amp(220, 8820, 16384);
    CHECK(amp(660, 8820, 16384) > a1 * 0.9 && amp(1100, 8820, 16384) > a1 * 0.9);
    CHECK(amp(440, 8820, 16384) < a1 * 0.01 && amp(110, 8820, 16384) < a1 * 0.01);

    /* foldback: at 2 kHz the 1' bar (16 kHz) folds down to 4 kHz */
    params(p, FULL, 0, 0);
    render(&v, &r, 2000, p, N);
    CHECK(amp(16000, 8820, 16384) < 50);
    CHECK(amp(8000, 8820, 16384) < 50);         /* 2' (8 kHz) folds too */
    CHECK(amp(4000, 8820, 16384) > 5000);       /* 2', 1' land on 4 kHz (and 4' is there) */
    printf("fold 2k: 16k %.0f, 8k %.0f, 4k %.0f\n", amp(16000, 8820, 16384),
           amp(8000, 8820, 16384), amp(4000, 8820, 16384));

    /* glide: the voice follows TE's per-sample increments */
    memset(&v, 0, sizeof(v));
    wh_note_on(&v);
    params(p, FLUTE, 0, 0);
    for (i = 0; i < 128; i++)
        incs[i] = inc_hz(330);
    for (i = 0; i < N; i += 128)
        wh_render(&v, &r, buf + i, 128 < N - i ? 128 : N - i, incs, p);
    CHECK(amp(330, 8820, 8192) > WH_LEVEL * 0.9);

    /* percussion 2ND: the 4' component decays fast; 3RD: 2 2/3'; SLOW lasts longer */
    params(p, FLUTE, 1, 0);
    render(&v, &r, 220, p, N);
    a1 = amp(440, 0, 2048);
    a2 = amp(440, 44100, 2048);
    printf("perc 2nd fast: %.0f -> %.0f after 0.5 s; 8' %.0f\n", a1, a2, amp(220, 0, 2048));
    CHECK(a1 > 8000 && a2 < a1 * 0.05);
    CHECK(amp(220, 44100, 2048) > WH_LEVEL * 0.6 && amp(220, 44100, 2048) < WH_LEVEL * 0.8);  /* -3 dB */
    params(p, FLUTE, 6, 0);
    render(&v, &r, 220, p, N);
    a1 = amp(660, 0, 2048);
    a2 = amp(660, 44100, 2048);
    CHECK(a1 > 8000 && a2 > a1 * 0.2 && amp(440, 0, 2048) < 100);
    params(p, FLUTE, 3, 0);                     /* soft: half the level, bars not cut */
    render(&v, &r, 220, p, N);
    CHECK(amp(440, 0, 2048) < a1 * 0.6 && amp(220, 44100, 2048) > WH_LEVEL * 0.95);
    params(p, FULL, 1, 0);                      /* percussion silences the 1' bar */
    render(&v, &r, 220, p, N);
    CHECK(amp(1760, 44100, 8192) < 50 && amp(1320, 44100, 8192) > 1000);

    /* click: only the first few ms differ */
    params(p, JAZZ, 0, 0);
    render(&v, &r, 220, p, N);
    memcpy(copy, buf, sizeof(buf));
    p[WH_CLICK] = 32767;
    render(&v, &r, 220, p, N);
    ok = 1;
    for (i = 8820; i < N; i++)
        ok &= buf[i] == copy[i];
    a1 = 0;
    for (i = 0; i < 441; i++)
        a1 += fabs((double)buf[i] - copy[i]);
    printf("click: mean |diff| first 5 ms %.0f\n", a1 / 441);
    CHECK(ok && a1 / 441 > 400);

    /* rotary FAST: level swings (horn), after the spin-up; OFF: steady */
    memset(&r, 0, sizeof(r));
    params(p, FLUTE, 0, 2);                     /* 880 Hz: horn only; windows = 10 periods */
    render(&v, &r, 880, p, N);
    lo = 1e9;
    hi = 0;
    for (i = 44100; i + 1002 <= N; i += 1002) {
        a1 = rms(i, 1002);
        lo = a1 < lo ? a1 : lo;
        hi = a1 > hi ? a1 : hi;
    }
    printf("rotary fast: rms %.0f .. %.0f, horn %.2f Hz, drum %.2f Hz\n", lo, hi,
           r.horn_hz / 65536.0, r.drum_hz / 65536.0);
    CHECK(hi > lo * 1.25);
    CHECK(r.horn_hz / 65536.0 > 5 && r.horn_hz / 65536.0 < 6.75 && r.drum_hz / 65536.0 > 1.5);  /* spinning up */
    params(p, FLUTE, 0, 0);
    render(&v, &r, 880, p, N);
    lo = 1e9;
    hi = 0;
    for (i = 44100; i + 1002 <= N; i += 1002) {
        a1 = rms(i, 1002);
        lo = a1 < lo ? a1 : lo;
        hi = a1 > hi ? a1 : hi;
    }
    CHECK(hi < lo * 1.05);

    /* drive: more odd harmonics, still bounded */
    params(p, FLUTE, 0, 0);
    p[WH_DRIVE] = 32767;
    render(&v, &r, 440, p, N);
    a1 = amp(440, 8820, 8192);
    a3 = amp(1320, 8820, 8192);
    printf("drive full: h3 %.1f dB\n", db(a3, a1));
    CHECK(db(a3, a1) > -20 && peak(0, N) <= 32768);

    /* repeatable notes, extremes stay in range */
    params(p, JAZZ, 1, 2);
    p[WH_CLICK] = 20000;
    memset(&r, 0, sizeof(r));
    render(&v, &r, 330, p, 4096);
    memcpy(copy, buf, 4096 * sizeof(int16_t));
    memset(&r, 0, sizeof(r));
    render(&v, &r, 330, p, 4096);
    CHECK(!memcmp(copy, buf, 4096 * sizeof(int16_t)));
    for (i = 0; i < WH_PARAMS; i++)
        p[i] = 32767;
    render(&v, &r, 8000, p, N);
    render(&v, &r, 20, p, N);
    params(p, FLUTE, 0, 0);
    for (i = 0; i < 4; i++)
        p[i] = (int16_t)-32768;                 /* TE's LFO can push slots 1-4 negative */
    render(&v, &r, 440, p, N);
    CHECK(peak(0, N) > 1000);

    printf("%d/%d checks passed\n", checks - fails, checks);
    return fails != 0;
}
