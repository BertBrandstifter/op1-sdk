/* Host tests for the FXPACK effect core (plain cc, built with fxpack_core.c by tools/test_all.py). */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "fxpack_core.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define SR      44100
#define BLOCK   64
#define PI      3.14159265358979
static fp_state st, st2;
static int16_t buf[SR * 2], ref[SR * 2];

static void run(fp_state *s, int16_t *b, int n, const int16_t *p)
{
    int i;

    for (i = 0; i < n; i += BLOCK)
        fp_process(s, b + i, n - i < BLOCK ? n - i : BLOCK, p);
}

static void sine(int16_t *b, int n, double hz, double amp)
{
    int i;

    for (i = 0; i < n; i++)
        b[i] = (int16_t)(amp * sin(2 * PI * hz * i / SR));
}

static void noise(int16_t *b, int n, int amp, uint32_t seed)
{
    int i;

    for (i = 0; i < n; i++) {
        seed = seed * 1664525u + 1013904223u;
        b[i] = (int16_t)(((int32_t)(seed >> 16) - 32768) * amp / 32768);
    }
}

static double rms(const int16_t *b, int n)
{
    double a = 0;
    int i;

    for (i = 0; i < n; i++)
        a += (double)b[i] * b[i];
    return sqrt(a / n);
}

/* amplitude of hz in b (Goertzel) */
static double tone(const int16_t *b, int n, double hz)
{
    double c = 2 * cos(2 * PI * hz / SR), s0, s1 = 0, s2 = 0;
    int i;

    for (i = 0; i < n; i++) {
        s0 = b[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return 2 * sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / n;
}

/* gain of the effect at hz: sine through a fresh state, measured after settling */
static double gain_at(int type, const int16_t *p, double hz)
{
    fp_reset(&st, type, 0);
    sine(buf, SR / 2, hz, 8000);
    run(&st, buf, SR / 2, p);
    return tone(buf + SR / 4, SR / 4, hz) / 8000;
}

static void set(int16_t *p, int a, int b, int c, int d, int e, int f, int g)
{
    p[0] = (int16_t)a; p[1] = (int16_t)b; p[2] = (int16_t)c; p[3] = (int16_t)d;
    p[4] = 0; p[5] = (int16_t)e; p[6] = (int16_t)f; p[7] = (int16_t)g;
}

int main(void)
{
    int16_t p[8];
    int t, i, ok;

    /* type knob, names, choices */
    CHECK(fp_type_of(0) == FP_PHASER && fp_type_of(32767) == FP_TEXTURE && fp_type_of(-1) == 0);
    CHECK(strcmp(fp_name(FP_DJ), "DJ FILTER") == 0 && strcmp(fp_param_name(FP_FUZZ, 2), "CHAR") == 0);
    CHECK(strcmp(fp_choice(FP_FUZZ, 2, 32767), "CHEBY") == 0 && strcmp(fp_choice(FP_TEXTURE, 0, 0), "VINYL") == 0
          && fp_choice(FP_PHASER, 0, 100) == 0);

    /* dry: MIX 0 (TEXTURE: LEVEL 0) leaves the signal bit-exact, every effect */
    for (t = 0; t < FP_TYPES; t++) {
        set(p, 20000, 20000, 20000, 0, 20000, 20000, 20000);
        if (t == FP_TEXTURE)
            p[1] = 0, p[3] = 20000;
        noise(buf, 8192, 20000, 3);
        memcpy(ref, buf, 8192 * 2);
        fp_reset(&st, t, 0);
        run(&st, buf, 8192, p);
        CHECK(memcmp(buf, ref, 8192 * 2) == 0);
    }

    /* stability: full-scale noise with all knobs at 0, at full and at a few mixes, then silence:
     * nothing sticks (except TEXTURE, which makes its own sound) */
    for (t = 0; t < FP_TYPES; t++) {
        static const int sets[4][7] = {
            { 0, 0, 0, 32767, 0, 0, 0 }, { 32767, 32767, 32767, 32767, 32767, 32767, 32767 },
            { 9000, 31000, 32767, 32767, 3000, 32767, 32767 }, { 32767, 0, 32767, 32767, 32767, 0, 0 },
        };
        int k, late = 0;

        for (k = 0; k < 4; k++) {
            set(p, sets[k][0], sets[k][1], sets[k][2], sets[k][3], sets[k][4], sets[k][5], sets[k][6]);
            fp_reset(&st, t, 0);
            noise(buf, SR, 32767, 9 + k);
            run(&st, buf, SR, p);
            memset(buf, 0, SR * 2);
            run(&st, buf, SR, p);
            for (i = SR / 2; i < SR; i++)
                late = buf[i] > late ? buf[i] : -buf[i] > late ? -buf[i] : late;
        }
        printf("%-9s peak 0.5 s after full-scale noise: %d\n", fp_name(t), late);
        if (t != FP_TEXTURE)
            CHECK(late < 300);
    }

    /* PHASER, fixed sweep (DEPTH 0), no feedback, full mix: deep notches */
    {
        double lo = 1e9, hi = 0, g, f;

        set(p, 0, 0, 0, 32767, 16384, 0, 0);    /* 4 stages, centre ~900 Hz */
        for (f = 100; f < 8000; f *= 1.06) {
            g = gain_at(FP_PHASER, p, f);
            lo = g < lo ? g : lo;
            hi = g > hi ? g : hi;
        }
        printf("phaser: gain min %.3f max %.3f\n", lo, hi);
        CHECK(lo < 0.1 && hi > 0.9);
    }

    /* PHASER right channel with SPREAD differs from left */
    set(p, 20000, 32767, 10000, 32767, 16384, 0, 32767);
    fp_reset(&st, FP_PHASER, 0);
    fp_reset(&st2, FP_PHASER, 1);
    noise(buf, SR / 2, 8000, 4);
    memcpy(ref, buf, SR);
    run(&st, buf, SR / 2, p);
    run(&st2, ref, SR / 2, p);
    for (i = 0, ok = 0; i < SR / 2; i++)
        ok += buf[i] != ref[i];
    CHECK(ok > SR / 4);

    /* FLANGER, static 5 ms (DELAY knob ~ 0.5, DEPTH 0), no feedback: first notch at 1/(2 D) */
    {
        double d = (4 + ((16384 * 437) >> 15)) / (double)SR, notch = 1 / (2 * d), gn, gp;

        set(p, 0, 0, 16384, 32767, 16384, 0, 0);
        gn = gain_at(FP_FLANGER, p, notch);
        gp = gain_at(FP_FLANGER, p, 2 * notch);
        printf("flanger: notch %.0f Hz gain %.3f, peak %.0f Hz gain %.3f\n", notch, gn, 2 * notch, gp);
        CHECK(gn < 0.1 && gp > 0.9);
    }

    /* FOLD: more DRIVE, more 3rd harmonic */
    {
        double h0, h1;

        set(p, 0, 16384, 32767, 32767, 0, 16384, 0);
        fp_reset(&st, FP_FOLD, 0);
        sine(buf, SR / 2, 220, 16000);
        run(&st, buf, SR / 2, p);
        h0 = tone(buf + SR / 4, SR / 4, 660) / tone(buf + SR / 4, SR / 4, 220);
        p[0] = 32767;
        fp_reset(&st, FP_FOLD, 0);
        sine(buf, SR / 2, 220, 16000);
        run(&st, buf, SR / 2, p);
        h1 = tone(buf + SR / 4, SR / 4, 660) / (tone(buf + SR / 4, SR / 4, 220) + 1);
        printf("fold: 3rd/1st clean %.3f, driven %.3f\n", h0, h1);
        CHECK(h0 < 0.05 && h1 > 0.3);
    }

    /* FUZZ: every curve distorts a driven sine; GATE silences quiet input */
    for (t = 0; t < 5; t++) {
        double h;

        set(p, 32767, 32767, t * 6554 + 100, 32767, 16384, 16384, 0);
        fp_reset(&st, FP_FUZZ, 0);
        sine(buf, SR / 2, 220, 8000);
        run(&st, buf, SR / 2, p);
        h = (tone(buf + SR / 4, SR / 4, 660) + tone(buf + SR / 4, SR / 4, 440)) / tone(buf + SR / 4, SR / 4, 220);
        printf("fuzz %s: (2nd+3rd)/1st %.2f, rms %.0f\n", fp_choice(FP_FUZZ, 2, p[2]), h, rms(buf + SR / 4, SR / 4));
        CHECK(h > 0.1);
    }
    set(p, 32767, 32767, 0, 32767, 16384, 16384, 32767);
    fp_reset(&st, FP_FUZZ, 0);
    noise(buf, SR / 2, 1000, 5);                /* -30 dB hiss: below the gate */
    run(&st, buf, SR / 2, p);
    printf("fuzz gate: hiss in, rms out %.1f\n", rms(buf + SR / 4, SR / 4));
    CHECK(rms(buf + SR / 4, SR / 4) < 20);

    /* DJ FILTER: left = low-pass (5 kHz gone), middle = open, right = high-pass (100 Hz gone);
     * 24 dB cuts more than 12 dB */
    {
        double lp5k, mid1k, mid5k, hp100, lp5k24;

        set(p, 4000, 0, 0, 32767, 0, 0, 0);
        lp5k = gain_at(FP_DJ, p, 5000);
        p[5] = 32767;
        lp5k24 = gain_at(FP_DJ, p, 5000);
        set(p, 16300, 0, 0, 32767, 0, 0, 0);
        mid1k = gain_at(FP_DJ, p, 1000);
        mid5k = gain_at(FP_DJ, p, 5000);
        set(p, 30000, 0, 0, 32767, 0, 0, 0);
        hp100 = gain_at(FP_DJ, p, 100);
        printf("dj: LP 5k %.3f (24 dB %.4f), middle 1k %.3f 5k %.3f, HP 100 Hz %.3f\n", lp5k, lp5k24, mid1k, mid5k, hp100);
        CHECK(lp5k < 0.05 && lp5k24 < lp5k / 5 && mid1k > 0.9 && mid1k < 1.1 && mid5k > 0.85 && hp100 < 0.05);
    }
    /* DJ resonance peaks at the cutoff */
    {
        double flat, res;

        double f, g;

        flat = res = 0;
        for (f = 300; f < 12000; f *= 1.08) {
            set(p, 12000, 0, 0, 32767, 0, 0, 0);
            g = gain_at(FP_DJ, p, f);
            flat = g > flat ? g : flat;
            p[1] = 32767;
            g = gain_at(FP_DJ, p, f);
            res = g > res ? g : res;
        }
        printf("dj: largest gain, no reso %.2f, full reso %.2f\n", flat, res);
        CHECK(flat < 1.1 && res > 3);
    }

    /* TRANSIENT: ATTACK up boosts the first ms of hits, SUSTAIN up their tails */
    {
        double a0, a1, s0, s1;
        int h;

        for (h = 0; h < 4; h++) {
            set(p, h & 1 ? 32767 : 16384, h & 2 ? 32767 : 16384, 16384, 32767, 16384, 0, 0);
            fp_reset(&st, FP_TRANSIENT, 0);
            memset(buf, 0, SR * 2);
            for (i = 0; i < 8; i++) {           /* 8 hits, 100 ms apart: 220 Hz, 30 ms decay */
                int j;

                for (j = 0; j < SR / 10; j++)
                    buf[i * (SR / 10) + j] = (int16_t)(12000 * exp(-j / (0.03 * SR)) * sin(2 * PI * 220 * j / SR));
            }
            run(&st, buf, SR, p);
            if (h == 0) {
                a0 = rms(buf + 7 * (SR / 10), 200);
                s0 = rms(buf + 7 * (SR / 10) + 2000, 1500);
            } else if (h == 1) {
                a1 = rms(buf + 7 * (SR / 10), 200);
            } else if (h == 2) {
                s1 = rms(buf + 7 * (SR / 10) + 2000, 1500);
            }
        }
        printf("transient: attack window %.0f -> %.0f, sustain window %.0f -> %.0f\n", a0, a1, s0, s1);
        CHECK(a1 > a0 * 1.3 && s1 > s0 * 1.3);
    }

    /* TEXTURE: every kind sounds on silence; DUCK lowers it under loud input */
    for (t = 0; t < FP_KINDS; t++) {
        double r;

        set(p, t * 5462 + 100, 32767, 32767, 0, 16384, 16384, 0);
        fp_reset(&st, FP_TEXTURE, 0);
        memset(buf, 0, SR * 2);
        run(&st, buf, SR, p);
        r = rms(buf + SR / 2, SR / 2);
        printf("texture %-5s rms %.0f\n", fp_choice(FP_TEXTURE, 0, p[0]), r);
        CHECK(r > 20 && r < 9000);
    }
    {
        double free_, ducked;

        set(p, 5462 * 3 + 100, 32767, 32767, 0, 16384, 16384, 32767);   /* WIND, DUCK full */
        fp_reset(&st, FP_TEXTURE, 0);
        memset(buf, 0, SR * 2);
        run(&st, buf, SR / 2, p);
        free_ = rms(buf + SR / 4, SR / 4);
        sine(buf, SR / 2, 100, 20000);
        memcpy(ref, buf, SR);
        run(&st, buf, SR / 2, p);
        for (i = 0; i < SR / 2; i++)
            buf[i] = (int16_t)(buf[i] - ref[i]);
        ducked = rms(buf + SR / 4, SR / 4);
        printf("texture duck: alone %.0f, under loud input %.0f\n", free_, ducked);
        CHECK(ducked < free_ / 3);
    }

    printf("%d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
