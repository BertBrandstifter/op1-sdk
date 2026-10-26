/* Host tests for the PSS voice core (plain cc, built with pss_core.c by tools/test_all.py). Run with
 * the built-in waves, and with build/pss_data.inc when gen_pss.py --sf2 made one (extra checks). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pss_core.h"
#include "pss_testvec.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define SR      PSS_SR
#define N       (SR * 2)
static int16_t buf[N];
static uint32_t incs[128];

static uint32_t inc_hz(double hz) { return (uint32_t)(hz / SR * 4294967296.0); }
static int16_t zk(int z, int n) { return (int16_t)(((2 * z + 1) * 32768L) / (2 * n)); }
static int16_t field(int v) { return (int16_t)(v * 520); }

static int find(const char *a, const char *b)  /* multisound by name, -1 if none */
{
    int w;

    for (w = 0; w < pss_nwaves(); w++)
        if (!strcmp(pss_wave_name(w), a) || (b && !strcmp(pss_wave_name(w), b)))
            return w;
    return -1;
}

/* osc A = w, B off, filter open, nothing else */
static void params(int16_t *p, int w)
{
    memset(p, 0, PSS_PARAMS * sizeof(*p));
    p[PSS_WAVE_A] = zk(w, pss_nwaves());
    p[PSS_WAVE_B] = zk(0, pss_nwaves() + 1);      /* off */
    p[PSS_CUTOFF] = 32767;
    p[PSS_BPITCH] = zk(24, 49);
}

static void render(pss_voice *v, uint32_t inc, const int16_t *p, int n)
{
    int i, k;

    memset(v, 0, sizeof(*v));
    for (k = 0; k < 128; k++)
        incs[k] = inc;
    pss_note_on(v, inc, p);
    for (i = 0; i < n; i += 128)
        pss_render(v, buf + i, n - i < 128 ? n - i : 128, incs, p);
}

static double amp(double hz, int from, int len)  /* Goertzel, Hann window */
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
    return sqrt(re * re + im * im) / len;
}

static double rms(int from, int len)
{
    double s = 0;
    int i;

    for (i = from; i < from + len; i++)
        s += (double)buf[i] * buf[i];
    return sqrt(s / len);
}

static int peak(int from, int len)
{
    int i, pk = 0;

    for (i = from; i < from + len; i++) {
        int a = buf[i] < 0 ? -buf[i] : buf[i];

        if (a > pk)
            pk = a;
    }
    return pk;
}

static int crossings(int from, int len)
{
    int i, n = 0, low = 1;

    for (i = from; i < from + len; i++) {
        if (low && buf[i] > 3000) { n++; low = 0; }
        else if (buf[i] < -3000) low = 1;
    }
    return n;
}

/* fundamental from rising zero crossings (interpolated; one counts after the wave was below -2000,
 * so ripple around zero does not), for waves with one such crossing per cycle */

static void test_decoder(void)
{
    static pss_sample s;
    pss_osc o;
    int i, ok = 1;

    s.off = 0;
    s.len = TV_LEN;
    s.loop = TV_LOOP;
    s.h1 = TV_H1;
    s.h2 = TV_H2;
    s.fmt = 0;
    memset(&o, 0, sizeof(o));
    o.s = &s;
    o.on = 1;
    for (i = 0; i < TV_REF_LEN; i++)
        ok &= pss_osc_next(&o, tv_data) == tv_ref[i];
    CHECK(ok);                                  /* bit-exact with gen_pss.py, two loop wraps */
    s.loop = PSS_NOLOOP;                         /* one-shot: silent after the end */
    memset(&o, 0, sizeof(o));
    o.s = &s;
    o.on = 1;
    for (i = 0; i < TV_LEN; i++)
        pss_osc_next(&o, tv_data);
    CHECK(pss_osc_next(&o, tv_data) == 0 && !o.on);
}

static void test_keys(void)
{
    int k, ok = 1;

    for (k = 0; k < 128; k++)
        ok &= pss_key_of_inc(pss_inc_of_key(k)) == k;
    CHECK(ok);
    CHECK(pss_key_of_inc(inc_hz(440)) == 69 && pss_key_of_inc(inc_hz(452)) == 69);
    CHECK(pss_key_of_inc(inc_hz(466.2)) == 70 && pss_key_of_inc(inc_hz(261.6)) == 60);
    CHECK(labs((long)pss_inc_of_key(69) - (long)inc_hz(440)) < 2 && labs((long)pss_inc_of_key(57) - (long)inc_hz(220)) < 2);
}

static void test_pitch(void)
{
    int16_t p[PSS_PARAMS];
    pss_voice v;
    int w = find("SAW", "SAW WAVE"), n;

    if (w < 0)                                  /* a ROM library has no SAW: the built-in run checks */
        return;
    params(p, w);
    render(&v, inc_hz(440), p, SR);             /* 1 s */
    n = crossings(0, SR);
    CHECK(n >= 437 && n <= 443);
    render(&v, pss_inc_of_key(96), p, SR);       /* C7 and C8: octave-down copies keep the pitch */
    n = crossings(0, SR);
    CHECK(n >= 2093 - 21 && n <= 2093 + 21);
    render(&v, pss_inc_of_key(108), p, SR);
    n = crossings(0, SR);
    CHECK(n >= 4186 - 42 && n <= 4186 + 42);
    p[PSS_WAVE_B] = zk(w + 1, pss_nwaves() + 1);  /* B = the same wave an octave up, A silent */
    p[PSS_BPITCH] = zk(24 + 12, 49);
    p[PSS_MIX] = 32767;
    render(&v, inc_hz(220), p, SR);
    n = crossings(0, SR);
    CHECK(n >= 437 && n <= 443);
    p[PSS_BPITCH] = zk(24 - 12, 49);             /* and an octave down: B only at 16' */
    render(&v, inc_hz(440), p, SR);
    n = crossings(0, SR);
    CHECK(n >= 218 && n <= 222);
}

static void test_all_waves(void)
{
    static const int keys[] = { 24, 48, 60, 72, 96, 108 };
    int16_t p[PSS_PARAMS];
    pss_voice v;
    int w, k, i, bad = 0, silent = 0, dead_loops = 0;

    for (w = 0; w < pss_nwaves(); w++)
        for (k = 0; k < (int)(sizeof(keys) / sizeof(keys[0])); k++) {
            const pss_sample *s = pss_pick(w, keys[k]);
            int clip = 0;

            params(p, w);
            render(&v, pss_inc_of_key(keys[k]), p, SR);
            for (i = 0; i < SR; i++)
                clip += buf[i] == 32767 || buf[i] == -32768;
            bad += clip > 8 || peak(0, SR) > 18000;    /* headroom for TE's voice mix */
            if (s->fmt < 2 && !s->eg[0]) {      /* without an AWM envelope: sounds at once, loops sustain */
                silent += rms(0, SR / 10) < 30;       /* BREATH at the lowest key: ~100 */
                if (s->loop != PSS_NOLOOP && rms(SR - 4410, 4410) < 30)
                    dead_loops++;
            } else if (s->eg[0] >> 4) {         /* AWM: sounds within the note (attack 0 stays at -63 dB) */
                silent += rms(0, SR) < 30;
            }
        }
    CHECK(bad == 0);
    CHECK(silent == 0);
    CHECK(dead_loops == 0);                     /* looped sounds keep sounding */
}

static void test_filter_levels(void)
{
    int16_t p[PSS_PARAMS];
    pss_voice v;
    int w = find("SAW", "SAW WAVE");
    double h10_open, h10_closed, a0, a1;

    if (w < 0)
        return;
    params(p, w);
    render(&v, inc_hz(220), p, SR / 2);
    h10_open = amp(2200, 8192, 8192);
    p[PSS_CUTOFF] = 12000;                       /* ~300 Hz */
    render(&v, inc_hz(220), p, SR / 2);
    h10_closed = amp(2200, 8192, 8192);
    CHECK(h10_closed < h10_open * 0.1);
    p[PSS_FENV] = field(63);                     /* envelope opens it at the start */
    p[PSS_FDECAY] = field(40);
    render(&v, inc_hz(220), p, SR / 2);
    CHECK(amp(2200, 0, 4096) > h10_closed * 3);
    p[PSS_RES] = 32767;                          /* resonance stays bounded */
    render(&v, inc_hz(220), p, SR / 2);
    CHECK(rms(0, SR / 2) < 20000);

    params(p, w);                               /* A DECAY: quieter after 1 s (amount 50: ~0.15 s) */
    p[PSS_ADECAY] = field(50);
    render(&v, inc_hz(220), p, SR + SR / 2);
    a0 = rms(0, 4096);
    a1 = rms(SR, 4096);
    CHECK(a1 < a0 * 0.3);
    w = find("SINE", "DWGS SINE");             /* B FADE: B (an octave up) rises, A steady */
    CHECK(w >= 0);
    params(p, w);
    p[PSS_WAVE_B] = zk(w + 1, pss_nwaves() + 1);
    p[PSS_BPITCH] = zk(24 + 12, 49);
    p[PSS_MIX] = 16384;
    p[PSS_BFADE] = field(40);
    render(&v, inc_hz(220), p, SR + SR / 2);
    CHECK(amp(440, 0, 4096) < amp(440, SR, 4096) * 0.5);
    CHECK(fabs(amp(220, 0, 4096) - amp(220, SR, 4096)) < amp(220, SR, 4096) * 0.2);
    p[PSS_MIX] = 0;                              /* MIX: all A */
    p[PSS_BFADE] = 0;
    render(&v, inc_hz(220), p, SR / 2);
    CHECK(amp(440, 8192, 8192) < amp(220, 8192, 8192) * 0.05);
}

/* MIX A..B is a crossfade, not two layers at full level: A = B at the centre is as loud as A alone
 * (the old law gave +6 dB there, which overloaded TE's voice mix in chords) */
static void test_mix_law(void)
{
    int16_t p[PSS_PARAMS];
    pss_voice v;
    int w = find("SAW", "SAW WAVE");
    double one, both;

    if (w < 0)
        return;
    params(p, w);
    render(&v, inc_hz(440), p, SR / 2);
    one = rms(4096, 16384);
    p[PSS_WAVE_B] = zk(w + 1, pss_nwaves() + 1);  /* the same wave in B: correlated, so peaks add */
    p[PSS_MIX] = 16384;
    render(&v, inc_hz(440), p, SR / 2);
    both = rms(4096, 16384);
    CHECK(both > one * 0.9 && both < one * 1.1);
    CHECK(peak(4096, 16384) < 18000);           /* a voice stays well short of full scale */
    p[PSS_MIX] = 0;                              /* the ends are one wave at full level */
    render(&v, inc_hz(440), p, SR / 2);
    CHECK(fabs(rms(4096, 16384) - one) < one * 0.05);
}

/* every page 2 / 3 parameter and MIX / CUTOFF stepped through its range: the measured effect only
 * moves one way (knob, bar and sound agree); a SAW at 220 Hz, B = SAW */
static double effect(int q, int v)
{
    int16_t p[PSS_PARAMS];
    pss_voice vc;
    int w = find("SAW", "SAW WAVE");

    params(p, w);
    p[q] = (int16_t)v;
    switch (q) {
    case PSS_CUTOFF:                             /* brightness: the 10th harmonic */
        render(&vc, inc_hz(220), p, SR / 4);
        return amp(2200, 8192, 8192);
    case PSS_RES:                                /* resonance: the peak at a low cutoff */
        p[PSS_CUTOFF] = 14000;
        render(&vc, inc_hz(110), p, SR / 4);
        return rms(8192, 8192);
    case PSS_FENV:                               /* brightness at the start */
        p[PSS_CUTOFF] = 8000;
        p[PSS_FDECAY] = field(30);
        render(&vc, inc_hz(220), p, SR / 4);
        return amp(2200, 0, 2048);
    case PSS_FDECAY:                             /* brightness later in the note */
        p[PSS_CUTOFF] = 8000;
        p[PSS_FENV] = field(63);
        render(&vc, inc_hz(220), p, SR / 2);
        return amp(2200, 16384, 4096);
    case PSS_KEYTRK:                             /* brightness of a high note */
        p[PSS_CUTOFF] = 12000;
        render(&vc, pss_inc_of_key(96), p, SR / 4);
        return amp(2093.0 * 2, 8192, 8192);
    case PSS_ADECAY:                             /* level after 0.5 s: less with more decay */
        render(&vc, inc_hz(220), p, SR);
        return -rms(SR / 2, 4096);
    /* B a fifth up (330 Hz): not one of A's harmonics, which would add to / cancel B by phase */
    case PSS_BFADE:                              /* B at the start: less with more fade */
        p[PSS_WAVE_B] = zk(w + 1, pss_nwaves() + 1);
        p[PSS_BPITCH] = zk(24 + 7, 49);
        p[PSS_MIX] = 16384;
        render(&vc, inc_hz(220), p, SR / 4);
        return -amp(220 * 1.4983, 0, 4096);
    case PSS_MIX:                                /* B against A */
        p[PSS_WAVE_B] = zk(w + 1, pss_nwaves() + 1);
        p[PSS_BPITCH] = zk(24 + 7, 49);
        render(&vc, inc_hz(220), p, SR / 4);
        return amp(220 * 1.4983, 8192, 8192) / (amp(220, 8192, 8192) + 1);
    case PSS_DETUNE:                             /* beating of A + B: the level's spread over 1 s */
        {
            double lo = 1e9, hi = 0;
            int i;

            p[PSS_WAVE_B] = zk(w + 1, pss_nwaves() + 1);
            p[PSS_MIX] = 16384;
            render(&vc, inc_hz(220), p, SR);
            for (i = 0; i < SR - 4096; i += 4096) {
                double r = amp(220, i, 4096);

                lo = r < lo ? r : lo;
                hi = r > hi ? r : hi;
            }
            return hi - lo;
        }
    }
    return 0;
}

static void test_monotonic(void)
{
    static const struct { int q, from; const char *name; } par[] = {
        { PSS_CUTOFF, 8000, "CUTOFF" }, { PSS_RES, 0, "RES" }, { PSS_FENV, 0, "F.ENV" },
        { PSS_FDECAY, 0, "F.DECAY" }, { PSS_KEYTRK, 0, "KEY TRK" }, { PSS_ADECAY, 520, "A DECAY" },
        { PSS_BFADE, 0, "B FADE" }, { PSS_MIX, 0, "MIX" }, { PSS_DETUNE, 520, "DETUNE" },
    };
    unsigned int i;

    if (find("SAW", "SAW WAVE") < 0)
        return;
    for (i = 0; i < sizeof(par) / sizeof(par[0]); i++) {
        double prev = effect(par[i].q, par[i].from), first = prev, e;
        int v, bad = 0;

        for (v = par[i].from; v <= 32767; v += (32767 - par[i].from) / 8) {
            e = effect(par[i].q, v);
            bad += e < prev - 0.02 * (fabs(prev) + 1);   /* never back down (2 % tolerance) */
            prev = e;
        }
        e = effect(par[i].q, 32767);
        if (bad || !(e > first))
            printf("  %s: not monotonic (%d steps back, %.1f -> %.1f)\n", par[i].name, bad, first, e);
        CHECK(!bad && e > first);
    }
}

static void test_restart(void)
{
    static int16_t first[128];
    int16_t p[PSS_PARAMS];
    pss_voice v;

    params(p, 0);
    render(&v, inc_hz(330), p, 128);
    memcpy(first, buf, sizeof(first));
    render(&v, inc_hz(330), p, 128);
    CHECK(!memcmp(first, buf, sizeof(first)));
}

static void test_rom(void)
{
    int16_t p[PSS_PARAMS];
    pss_voice v;
    int w, i, sounding = 0, named = 1;
    static int16_t a[4096];

    if (!pss_have_rom()) {
        printf("(built-in waves only: ROM checks skipped)\n");
        return;
    }
    CHECK(pss_nwaves() >= 1 && pss_rom_bytes() <= 1600000);     /* TE's sample memory, as STATION 1 */
    for (w = 0; w < pss_nwaves(); w++) {
        long e = 0;

        params(p, w);
        p[PSS_CUTOFF] = 20000;
        render(&v, pss_inc_of_key(60), p, 4096);
        for (i = 0; i < 4096; i++)
            e += buf[i] < 0 ? -buf[i] : buf[i];
        if (e > 0)
            sounding++;
        if (!pss_wave_name(w)[0])
            named = 0;
        memcpy(a, buf, sizeof(a));
    }
    CHECK(named && sounding == pss_nwaves());
    CHECK(pss_key_of_inc(pss_inc_of_key(69)) == 69 && pss_inc_of_key(69) > 0);
}

/* GEW7 envelope against MAME's per-chip-sample code (gen_pss11.py gew_eg): every rate, decay level
 * and key scaling, level index within the chip's own at +-1 step of 32 samples (+-2 index steps) */
static void test_gew_eg(void)
{
    static const uint8_t nolfo[2] = { 0, 0 };
    int c, j, bad = 0;

    for (c = 0; c < TV_EG_N; c++) {
        pss_eg e;
        uint32_t k = 0;
        int idx = 0, ok = 1;

        memset(&e, 0, sizeof(e));
        pss_eg_start(&e, tv_eg[c].eg, nolfo, 1, tv_eg[c].st);
        for (j = 0; j < TV_EG_K && ok; j++) {
            while (k < tv_eg_checks[j]) {
                idx = pss_eg_next(&e);
                k++;
            }
            if (idx < tv_eg[c].lohi[2 * j] || idx > tv_eg[c].lohi[2 * j + 1]) {
                printf("  GEW EG %02x %02x %02x st %u: after %u x 32 samples %d, chip %d .. %d\n", tv_eg[c].eg[0],
                       tv_eg[c].eg[1], tv_eg[c].eg[2], tv_eg[c].st, k, idx, tv_eg[c].lohi[2 * j],
                       tv_eg[c].lohi[2 * j + 1]);
                ok = 0;
            }
        }
        bad += !ok;
    }
    CHECK(bad == 0);
}

/* GEW 8- and 12-bit samples as gew.cpp reads them, through end -> loop wraps, reversed first pass */
static void test_gew_decode(void)
{
    int t, i, ok = 1;

    for (t = 0; t < 2; t++) {
        pss_sample smp;
        pss_osc o;

        memset(&smp, 0, sizeof(smp));
        memset(&o, 0, sizeof(o));
        smp.fmt = tv_gew[t].fmt;
        smp.len = tv_gew[t].n;
        smp.loop = tv_gew[t].loop;
        smp.lfo[0] = (uint8_t)(tv_gew[t].rev << 7);
        o.s = &smp;
        o.on = 1;
        o.rev = tv_gew[t].rev;
        for (i = 0; i < 500; i++)
            ok &= pss_osc_next(&o, tv_gew_data) == tv_gew[t].seq[i];
    }
    CHECK(ok);
}

/* a voice with an AWM envelope (built-in E.PIANO: attack 15, decay 1 5 to level 4, decay 2 3, key
 * scaling 2): the rendered level follows the envelope (RMS per 23 ms vs the level index, +-0.7 dB, 2 s) */
static void test_awm_render(void)
{
    int w = find("E.PIANO", 0), i, k, worst = 0, n = N;
    int16_t p[PSS_PARAMS];
    pss_voice v;
    pss_eg ref;
    static int lvl[N / 32 + 1];
    double r0 = 0;

    if (w < 0 || pss_have_rom())
        return;
    params(p, w);
    memset(&v, 0, sizeof(v));
    for (k = 0; k < 128; k++)
        incs[k] = pss_inc_of_key(57);
    pss_note_on(&v, incs[0], p);
    ref = v.a.eg;
    CHECK(ref.st == 0 && ref.ar == 63);
    for (i = 0; i < n / 32; i++)
        lvl[i] = pss_eg_next(&ref);
    for (i = 0; i < n; i += 128)
        pss_render(&v, buf + i, 128, incs, p);
    for (i = 0; i + 2048 <= n; i += 2048) {     /* windows of 2048 = 64 envelope steps */
        double e = 0, want;
        int j;

        for (j = 0; j < 2048; j++)
            e += (double)buf[i + j] * buf[i + j];
        e = 10 * log10(e / 2048 + 1e-9);
        want = 0;
        for (j = 0; j < 64; j++)
            want += pow(10, -(96 - 96.0 * lvl[i / 32 + j] / 1024) / 10);
        want = 10 * log10(want / 64);
        if (i == 0)
            r0 = e - want;
        else if (want > -60 && fabs(e - want - r0) * 10 > worst)
            worst = (int)(fabs(e - want - r0) * 10);
    }
    printf("  AWM render: level vs envelope within %.1f dB over 2 s (E.PIANO, decays %d dB)\n", worst / 10.0,
           (int)(96 - 96.0 * lvl[n / 32 - 1] / 1024));
    CHECK(worst <= 7 && lvl[n / 32 - 1] < lvl[100]);
}

int main(void)
{
    test_decoder();
    test_keys();
    test_pitch();
    test_all_waves();
    test_filter_levels();
    test_mix_law();
    test_restart();
    test_monotonic();
    test_rom();
    test_gew_eg();
    test_gew_decode();
    test_awm_render();
    printf("%s: %d checks, %d failed (%s, %u bytes)\n", fails ? "FAIL" : "OK", checks, fails,
           pss_have_rom() ? "PSS-11 ROM data" : "built-in waves", pss_rom_bytes());
    return fails != 0;
}
