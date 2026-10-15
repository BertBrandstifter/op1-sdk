/* Host tests for the SHIMMER effect core (plain cc, built with shimmer_core.c by tools/test_all.py). */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "shimmer_core.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define SR      44100
#define BLOCK   64                              /* TE's FX block */
#define LEN     (SR * 6)
static sh_state st;
static int16_t buf[LEN], bl[LEN], br[LEN];

static void run(int16_t *b, int n, const int16_t *p)
{
    int i;

    for (i = 0; i < n; i += BLOCK)
        sh_process(&st, b + i, n - i < BLOCK ? n - i : BLOCK, p);
}

static void run_st(int16_t *l, int16_t *r, int n, const int16_t *p)
{
    int i;

    for (i = 0; i < n; i += BLOCK)
        sh_process_stereo(&st, l + i, r + i, n - i < BLOCK ? n - i : BLOCK, p);
}

static double rms(const int16_t *b, int n)
{
    double acc = 0;
    int i;

    for (i = 0; i < n; i++)
        acc += (double)b[i] * b[i];
    return sqrt(acc / n);
}

static int peak(const int16_t *b, int n)
{
    int i, m = 0;

    for (i = 0; i < n; i++)
        m = b[i] > m ? b[i] : -b[i] > m ? -b[i] : m;
    return m;
}

/* energy at hz: Goertzel over 1024-sample frames (~43 Hz wide: a shifted tone restarts its
 * phase every grain, which spreads it by ~20 Hz), averaged */
static double tone1(const int16_t *b, int n, double hz)
{
    double c = 2 * cos(2 * 3.14159265358979 * hz / SR), s0, s1 = 0, s2 = 0;
    int i;

    for (i = 0; i < n; i++) {
        s0 = b[i] + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return (s1 * s1 + s2 * s2 - c * s1 * s2) / ((double)n * n);
}

static double tone(const int16_t *b, int n, double hz)
{
    double acc = 0;
    int i, k = 0;

    for (i = 0; i + 1024 <= n; i += 1024, k++)
        acc += tone1(b + i, 1024, hz);
    return acc / k;
}

/* buf = a sine of hz for n samples, then silence */
static void note(double hz, int n)
{
    int i;

    for (i = 0; i < LEN; i++)
        buf[i] = (int16_t)(i < n ? 8000 * sin(2 * 3.14159265358979 * hz * i / SR) : 0);
}

static void noise(int16_t *b, int n, int amp, uint32_t seed)
{
    int i;

    for (i = 0; i < n; i++) {
        seed = seed * 1664525u + 1013904223u;
        b[i] = (int16_t)(((int32_t)(seed >> 16) - 32768) * amp / 32768);
    }
}

static void defaults(int16_t *p)
{
    p[SH_DECAY] = 20000;
    p[SH_TONE] = 20000;
    p[SH_SHIMMER] = 0;
    p[SH_MIX] = 32767;
    p[SH_INTERVAL] = 0;                         /* +12 */
    p[SH_SIZE] = 22000;
    p[SH_MOD] = 0;
    p[SH_LOWCUT] = 8000;
}

int main(void)
{
    int16_t p[SH_PARAMS];
    int i, ok;

    /* interval knob covers all five; semitones */
    CHECK(sh_interval(0) == 0 && sh_interval(32767) == SH_INTERVALS - 1 && sh_interval(-5) == 0);
    CHECK(sh_interval_semis(0) == 12 && sh_interval_semis(1) == 7 && sh_interval_semis(4) == -12);

    /* MIX 0: dry, bit-exact (mono and stereo) */
    defaults(p);
    p[SH_MIX] = 0;
    for (i = 0; i < 4096; i++)
        buf[i] = bl[i] = br[i] = (int16_t)(i * 37 % 20000 - 10000);
    sh_reset(&st);
    run(buf, 4096, p);
    run_st(bl, br, 4096, p);
    for (i = 0, ok = 1; i < 4096; i++)
        ok &= buf[i] == (int16_t)(i * 37 % 20000 - 10000) && bl[i] == buf[i] && br[i] == buf[i];
    CHECK(ok);

    /* reverb tail: a noise burst, then silence; still sounding 1 s later with long DECAY, gone
     * with short DECAY; level of the wet in a sane range */
    {
        double t_short, t_long, wet;

        defaults(p);
        p[SH_DECAY] = 0;
        sh_reset(&st);
        memset(buf, 0, sizeof(buf));
        noise(buf, SR / 4, 8000, 1);
        run(buf, SR * 2, p);
        t_short = rms(buf + SR + SR / 4, SR / 4);
        p[SH_DECAY] = 26000;
        sh_reset(&st);
        memset(buf, 0, sizeof(buf));
        noise(buf, SR / 4, 8000, 1);
        run(buf, SR * 2, p);
        t_long = rms(buf + SR + SR / 4, SR / 4);
        wet = rms(buf + SR / 8, SR / 8);
        printf("tail rms short %.1f long %.1f, wet during burst %.0f (dry %.0f)\n", t_short, t_long, wet, 8000 / sqrt(3));
        CHECK(t_short < 5 && t_long > 300);
        CHECK(wet > 1000 && wet < 12000);
    }

    /* RT60 at DECAY 0.5: 2^(-3.03) * ... the tail falls by ~60 dB in about RT; measure the slope
     * between 0.3 s and 0.8 s after a burst and compare with the formula (within a factor 1.6) */
    {
        double a, b, rt, want;

        defaults(p);
        p[SH_DECAY] = 16384;
        p[SH_TONE] = 32767;
        sh_reset(&st);
        memset(buf, 0, sizeof(buf));
        noise(buf, SR / 10, 8000, 3);
        run(buf, SR * 2, p);
        a = rms(buf + SR / 10 + SR * 3 / 10, SR / 20);
        b = rms(buf + SR / 10 + SR * 8 / 10, SR / 20);
        rt = 0.5 * 60 / (20 * log10(a / b));
        want = 0.3 * pow(2, 6.06 * 0.5);
        printf("RT60 measured %.2f s, want %.2f s\n", rt, want);
        CHECK(rt > want / 1.6 && rt < want * 1.6);
    }

    /* the shimmer: a 440 Hz note into the tank; with SHIMMER the tail carries 880 and 1760 Hz,
     * without it hardly any; -12 brings 220 Hz */
    {
        double e880_off, e880_on, e1760_on, e220_dn, e220_off;

        defaults(p);
        p[SH_DECAY] = 26000;
        p[SH_TONE] = 30000;
        sh_reset(&st);
        note(440, SR / 3);
        run(buf, SR * 3, p);
        e880_off = tone(buf + SR / 2, SR / 2, 880);
        e220_off = tone(buf + SR / 2, SR / 2, 220);
        p[SH_SHIMMER] = 26000;
        sh_reset(&st);
        note(440, SR / 3);
        run(buf, SR * 3, p);
        e880_on = tone(buf + SR / 2, SR / 2, 880);
        e1760_on = tone(buf + SR / 2, SR / 2, 1760);
        p[SH_INTERVAL] = 32767;                 /* -12 */
        sh_reset(&st);
        note(440, SR / 3);
        run(buf, SR * 3, p);
        e220_dn = tone(buf + SR / 2, SR / 2, 220);
        printf("880 Hz off %.3g on %.3g, 1760 on %.3g; 220 Hz off %.3g down %.3g\n",
               e880_off, e880_on, e1760_on, e220_off, e220_dn);
        CHECK(e880_on > 30 * e880_off && e1760_on > 5 * e880_off);
        CHECK(e220_dn > 30 * e220_off);
    }

    /* +7: 440 -> 659 Hz */
    {
        double on, off;

        defaults(p);
        p[SH_DECAY] = 26000;
        p[SH_INTERVAL] = 9000;                  /* +7 */
        sh_reset(&st);
        note(440, SR / 3);
        run(buf, SR * 3, p);
        off = tone(buf + SR / 2, SR / 2, 659.26);
        p[SH_SHIMMER] = 26000;
        sh_reset(&st);
        note(440, SR / 3);
        run(buf, SR * 3, p);
        on = tone(buf + SR / 2, SR / 2, 659.26);
        printf("659 Hz off %.3g on %.3g\n", off, on);
        CHECK(on > 30 * off);
    }

    /* stability: 3 s full-scale noise with every knob at full (longest, brightest, most shimmer,
     * all intervals) stays bounded and then dies out: the shifted energy climbs out of the band */
    for (i = 0; i < SH_INTERVALS; i++) {
        int k, late;

        for (k = 0; k < SH_PARAMS; k++)
            p[k] = 32767;
        p[SH_INTERVAL] = (int16_t)(i * 6554 + 100);
        p[SH_LOWCUT] = 0;
        sh_reset(&st);
        noise(buf, SR * 3, 32767, 7 + i);
        run(buf, SR * 3, p);
        for (k = 0; k < 20; k++) {              /* 20 s at the longest decay */
            memset(buf, 0, sizeof(int16_t) * SR);
            run(buf, SR, p);
        }
        late = peak(buf, SR);
        printf("interval %+d: peak 20 s after full-scale noise %d\n", sh_interval_semis(i), late);
        CHECK(late < 2500);                     /* plain tank at RT 20 s: -60 dB per 20 s */
        p[SH_DECAY] = 0;
        p[SH_SHIMMER] = 0;
        for (k = 0; k < 2; k++) {
            memset(buf, 0, sizeof(int16_t) * SR);
            run(buf, SR, p);
        }
        CHECK(peak(buf + SR / 2, SR / 2) < 40);
    }

    /* DC in: nothing builds up (wet only: after the step's own tail the output is quiet) */
    defaults(p);
    p[SH_DECAY] = 16384;
    p[SH_SHIMMER] = 32767;
    sh_reset(&st);
    for (i = 0; i < SR * 4; i++)
        buf[i] = 12000;
    run(buf, SR * 4, p);
    printf("DC in: largest output in the 4th second %d\n", peak(buf + SR * 3, SR));
    CHECK(peak(buf + SR * 3, SR) < 300);

    /* stereo: both sides carry the tail, and they differ (wide) */
    {
        double c = 0, el = 0, er = 0;

        defaults(p);
        p[SH_DECAY] = 26000;
        sh_reset(&st);
        memset(bl, 0, sizeof(bl));
        noise(bl, SR / 4, 8000, 5);
        memcpy(br, bl, sizeof(bl));
        run_st(bl, br, SR * 2, p);
        for (i = SR; i < SR + SR / 2; i++) {
            c += (double)bl[i] * br[i];
            el += (double)bl[i] * bl[i];
            er += (double)br[i] * br[i];
        }
        printf("stereo tail rms L %.0f R %.0f, correlation %.2f\n", sqrt(el / (SR / 2)), sqrt(er / (SR / 2)), c / sqrt(el * er));
        CHECK(el > 0 && er > 0 && sqrt(el / (SR / 2)) > 100 && sqrt(er / (SR / 2)) > 100);
        CHECK(fabs(c / sqrt(el * er)) < 0.5);
    }

    /* SIZE moved during a tail: lengths slew, no jumps (largest step between samples stays
     * near what the tail has anyway) */
    {
        int d, worst = 0, before = 0;

        defaults(p);
        p[SH_DECAY] = 26000;
        sh_reset(&st);
        memset(buf, 0, sizeof(buf));
        note(300, SR / 4);
        run(buf, SR / 2, p);
        for (i = SR / 4 + 1; i < SR / 2; i++) {
            d = buf[i] - buf[i - 1];
            before = d > before ? d : -d > before ? -d : before;
        }
        p[SH_SIZE] = 0;
        p[SH_MOD] = 32767;
        run(buf + SR / 2, SR / 2, p);
        for (i = SR / 2 + 1; i < SR; i++) {
            d = buf[i] - buf[i - 1];
            worst = d > worst ? d : -d > worst ? -d : worst;
        }
        printf("max step before %d, while SIZE moves %d\n", before, worst);
        CHECK(worst < before * 2 + 200);
    }

    printf("%d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
