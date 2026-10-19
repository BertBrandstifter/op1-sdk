/* MONO bench (build/apps/op1plus): engine host + MONO glue against stub TE functions and TE's
 * per-sample pitch buffer. Returns failed-check bits (0 = pass). */
#include <op1/te246.h>

extern void wh_te_render(void *self, int16_t *out, int16_t *knobs);    /* the engine host's */
extern void wh_te_note_on(void *self, int a1, int a2);
extern void compose_init(void);
extern void engine_name(te_string *out, void *db, const char *sql);    /* composer's chain */
extern void hook_key(int key, int down);
extern void hook_knob(int knob, int16_t *diff);
extern void hook_frame(uint16_t *buf);
extern char FB[];
extern volatile uint8_t mo_page;
extern volatile int8_t mo_bank;
extern volatile uint8_t mo_title_on;
extern void mo_tick(void);
extern int mo_param(const volatile int16_t *k, int id);
extern void mo_param_set(volatile int16_t *k, int id, int v);
extern void mo_bank_knobs(int e, int16_t *k);

#define INC         ((volatile uint32_t *)0xff801950)
#define VOICE(i)    ((void *)(0xffb00000u + (i) * 0x24))
#define P2(t, v)    ((int16_t)(((t) << 12) | (v)))

static int drwave_renders, drwave_notes, saves, fxsaves, keys_seen, knobs_seen;
static int16_t out[128];
volatile uint32_t cycles_plain, cycles_heavy;   /* read by run_monotest.py */
volatile int fails;

void drwave_render(void *self, int16_t *o, int16_t *k)
{
    int i;
    (void)self; (void)k;
    drwave_renders++;
    for (i = 0; i < 128; i++)
        o[i] = 1234;
}
void drwave_note_on(void *self, int a1, int a2) { (void)self; (void)a1; (void)a2; drwave_notes++; }
void log_save(void *patch, int16_t *k) { (void)patch; (void)k; saves++; }
void log_fxsave(void *patch, int16_t *f) { (void)patch; (void)f; fxsaves++; }
void log_key(int key, int down) { (void)key; (void)down; keys_seen++; }
void log_knob(int knob, int16_t *diff) { (void)knob; (void)diff; knobs_seen++; }

static uint32_t cycles(void)
{
    uint32_t c;
    __asm__ volatile("%0 = CYCLES;" : "=d"(c));
    return c;
}

static void pitch(uint32_t inc)
{
    int i;
    for (i = 0; i < 128; i++)
        INC[i] = inc;
}

static void push(int key, int shift)
{
    *TE_SHIFT_HELD = (uint8_t)shift;
    hook_key(key, 1);
    hook_key(key, 0);
    *TE_SHIFT_HELD = 0;
}

static void turn(int knob, int16_t d) { hook_knob(knob, &d); }

static void name(te_string *s, const char *n)
{
    int i;

    for (i = 0; n[i]; i++)
        s->bx.sso[i] = n[i];
    s->bx.sso[i] = 0;
    s->size = (uint32_t)i;
    s->cap = 15;
}

static int peak(void)
{
    int i, m = 0;

    for (i = 0; i < 128; i++)
        m = out[i] > m ? out[i] : -out[i] > m ? -out[i] : m;
    return m;
}

static int loud(void) { return peak() > 1000; }  /* the last block has sound */

static int level[6];

/* one audio block as TE renders it: voices 1 .. 4 in index order; level[v] = it had sound */
static void block(int16_t *knobs)
{
    int v;

    for (v = 1; v <= 4; v++) {
        wh_te_render(VOICE(v), out, knobs);
        level[v] = loud();
    }
}

#define CHECK(bit, c) do { if (!(c)) fails |= 1 << (bit); } while (0)
#define K(i)        TE_SYNTH_KNOBS[2 * 8 + (i)]  /* sound 3 */
#define HZ(f)       ((uint32_t)((f) / 88200.0 * 4294967296.0))

/* parameter ids (mono.c's page order) */
enum { P_CUTOFF, P_EMPH, P_CONTOUR, P_GLIDE, P_RANGE1, P_WAVE1, P_VOL1, P_NOISE, P_RANGE2, P_FREQ2,
       P_WAVE2, P_VOL2, P_RANGE3, P_FREQ3, P_WAVE3, P_VOL3, P_FATT, P_FDEC, P_FSUS, P_KBD, P_LATT,
       P_LDEC, P_LSUS, P_RELEASE, NP };

/* the 'mono 3' preset (feature.json content_sql) = bank entry 1 */
static const int16_t preset[8] = { 4114, 2573, 9616, 608, 9239, 12229, 8621, 9874 };

/* SAW at 8', filter open, no emphasis / contour / glide, osc 2 / 3 at unison but silent, sustains full */
static void saw_knobs(int16_t *k)
{
    int i;

    for (i = 0; i < 8; i++)
        k[i] = i < 4 ? 0 : P2(2, 0);
    mo_param_set(k, P_CUTOFF, 255);
    mo_param_set(k, P_RANGE1, 3); mo_param_set(k, P_RANGE2, 3); mo_param_set(k, P_RANGE3, 3);
    mo_param_set(k, P_WAVE1, 2);
    mo_param_set(k, P_VOL1, 15);
    mo_param_set(k, P_FREQ2, 15); mo_param_set(k, P_FREQ3, 15);
    mo_param_set(k, P_FSUS, 15); mo_param_set(k, P_LSUS, 15);
}

int main(void)
{
    static const int16_t stock[8] = { 20924, 4313, 8063, 0, 32767, 0, 0, 0 };
    int16_t knobs[8], saw[8];
    int i, n, low, ok;
    uint32_t t0;

    __asm__ volatile("R0 = SYSCFG; BITSET (R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");  /* CCEN */
    compose_init();
    *TE_EDIT_MODE = 0;
    *TE_SYNTH_SUBSCREEN = 0;
    *TE_SYNTH_SLOT = 2;
    TE_SYNTH_TYPE[2] = TE_INSTR_DRWAVE;
    saw_knobs(saw);

    /* 0: plain drwave stays TE's; the preset = bank entry 1, inside drwave's ranges, tag 2 */
    for (i = 0; i < 8; i++)
        knobs[i] = stock[i];
    wh_te_render(VOICE(0), out, knobs);
    CHECK(0, drwave_renders == 1 && out[0] == 1234);
    mo_bank_knobs(0, knobs);
    for (ok = 1, i = 0; i < 8; i++)
        ok &= knobs[i] == preset[i];
    CHECK(0, ok);

    /* 1: tag 2 = MONO: a saw at 441 Hz from TE's pitch buffer (rising zero crossings) */
    for (i = 0; i < 8; i++)
        K(i) = knobs[i] = saw[i];
    pitch(HZ(441.0));
    wh_te_note_on(VOICE(1), 0, 0);
    drwave_renders = 0;
    for (n = 0, low = 1, i = 0; i < 689; i++) {
        int j;
        wh_te_render(VOICE(1), out, knobs);
        for (j = 0; j < 128; j++) {
            if (low && out[j] > 2000) { n++; low = 0; }
            else if (out[j] < -2000) low = 1;
        }
    }
    CHECK(1, drwave_renders == 0 && n >= 439 && n <= 443);

    /* 2: poly, in TE's order (every voice once per block, by index): a new note on voice 2 sounds
     * and voice 1 goes on; held keys A (voice 3) and B (voice 4) sound together, B's key up leaves A
     * (release off: B is gone after its 21 ms) */
    pitch(HZ(330.0));
    wh_te_note_on(VOICE(2), 0, 0);
    block(knobs);
    block(knobs);
    ok = level[1] && level[2];
    hook_key(39, 1);                            /* key A */
    pitch(HZ(220.0));
    wh_te_note_on(VOICE(3), 0, 0);
    block(knobs);
    hook_key(40, 1);                            /* key B */
    pitch(HZ(247.0));
    wh_te_note_on(VOICE(4), 0, 0);
    block(knobs);
    block(knobs);
    ok &= level[3] && level[4];
    hook_key(40, 0);
    for (n = 0; n < 30; n++)
        block(knobs);
    ok &= level[3] && !level[4];
    hook_key(39, 0);
    CHECK(2, ok);

    /* 3: knob push cycles 7 pages */
    i = keys_seen;
    push(59, 0);
    CHECK(3, mo_page == 1 && keys_seen == i);
    for (n = 0; n < 5; n++)
        push(60, 0);
    CHECK(3, mo_page == 6);
    push(59, 0);
    CHECK(3, mo_page == 0);

    /* 4: page 1 CUTOFF clamps at 255 (fields below kept); page 2 RANGE one step per detent (max
     * 2'), WAVE, the tag kept; page 3 FREQ to +7 st; page 5 KEYBOARD to FULL */
    {
        int f2 = mo_param(&K(0), P_FREQ2), rel = mo_param(&K(0), P_RELEASE);

        for (n = 0; n < 200; n++)
            turn(0, 32767);
        CHECK(4, mo_param(&K(0), P_CUTOFF) == 255 && knobs_seen == 0
                 && mo_param(&K(0), P_FREQ2) == f2 && mo_param(&K(0), P_RELEASE) == rel);
    }
    push(59, 0);
    turn(0, 1024);
    CHECK(4, mo_param(&K(0), P_RANGE1) == 4);
    for (n = 0; n < 20; n++)
        turn(0, 1024);
    turn(1, -1024);
    CHECK(4, mo_param(&K(0), P_RANGE1) == 5 && mo_param(&K(0), P_WAVE1) == 1);
    for (i = 4; i < 8; i++)
        CHECK(4, (K(i) >> 12) == 2);
    push(59, 0);
    for (n = 0; n < 40; n++)
        turn(1, 1024);
    CHECK(4, mo_param(&K(0), P_FREQ2) == 30);
    push(59, 0);
    push(59, 0);
    for (n = 0; n < 5; n++)
        turn(3, 1024);
    CHECK(4, mo_param(&K(0), P_KBD) == 3);
    push(59, 0);
    push(59, 0);
    push(59, 0);
    CHECK(4, mo_page == 0);

    /* 5: shift + blue = bank, 16 entries in drwave's ranges with tag 2, then P-- (the sound as it was) */
    {
        static const int16_t hi[8] = { 24575, 16383, 16383, 32767, 32767, 32767, 32767, 32767 };
        int16_t before[8];
        int e, k;

        for (k = 0; k < 8; k++)
            before[k] = K(k);
        *TE_SHIFT_HELD = 1;
        turn(0, 1024);
        ok = mo_bank == 0;
        for (e = 0; e < 16; e++) {
            for (k = 0; k < 8; k++)
                ok &= K(k) >= 0 && K(k) <= hi[k];
            for (k = 4; k < 8; k++)
                ok &= (K(k) >> 12) == 2;
            turn(0, 1024);
        }
        ok &= mo_bank == -1;
        for (k = 0; k < 8; k++)
            ok &= K(k) == before[k];
        turn(0, -1024);
        ok &= mo_bank == 15;
        turn(0, 1024);
        *TE_SHIFT_HELD = 0;
        CHECK(5, ok && knobs_seen == 0);
    }

    /* 6: page 7 = FX knobs */
    {
        volatile int16_t *rt = (volatile int16_t *)0x01de5c50 + TE_FX_SPRING * 24;
        volatile int16_t *fx = (volatile int16_t *)0xff801444 + 2 * 8;

        rt[0] = 1344; rt[1] = 16448;
        fx[0] = 8000;
        ((volatile int32_t *)0xff801734)[2] = TE_FX_SPRING;
        ((volatile uint8_t *)0x01aaac20)[2] = 1;
        mo_page = 6;
        for (n = 0; n < 200; n++)
            turn(0, 32767);
        CHECK(6, fx[0] == 16448 && fxsaves == 200);
        mo_page = 0;
    }

    /* 7: engine list: "mono 3" -> "drwave", "trio" too (the chain), "mono" untouched */
    {
        static te_string a, b, c;

        name(&a, "mono 3"); name(&b, "trio"); name(&c, "mono");
        engine_name(&a, 0, 0);
        engine_name(&b, 0, 0);
        engine_name(&c, 0, 0);
        CHECK(7, a.size == 6 && a.bx.sso[0] == 'd' && b.size == 6 && c.size == 4 && c.bx.sso[0] == 'm');
    }

    /* 8: cost per voice block of the sounding voice: a saw, filter open; + three oscillators, pink
     * noise, emphasis, contour, keyboard control, a glide on the way */
    for (i = 0; i < 8; i++)
        K(i) = knobs[i] = saw[i];
    pitch(HZ(220.0));
    wh_te_note_on(VOICE(5), 0, 0);
    wh_te_render(VOICE(5), out, knobs);
    t0 = cycles();
    wh_te_render(VOICE(5), out, knobs);
    cycles_plain = cycles() - t0;
    mo_param_set(knobs, P_CUTOFF, 60); mo_param_set(knobs, P_EMPH, 200); mo_param_set(knobs, P_CONTOUR, 180);
    mo_param_set(knobs, P_GLIDE, 10); mo_param_set(knobs, P_VOL2, 15); mo_param_set(knobs, P_VOL3, 15);
    mo_param_set(knobs, P_WAVE2, 1); mo_param_set(knobs, P_WAVE3, 5); mo_param_set(knobs, P_FREQ2, 18);
    mo_param_set(knobs, P_NOISE, 25); mo_param_set(knobs, P_KBD, 3); mo_param_set(knobs, P_FDEC, 20);
    for (i = 0; i < 8; i++)
        K(i) = knobs[i];
    pitch(HZ(330.0));
    wh_te_note_on(VOICE(0), 0, 0);
    wh_te_render(VOICE(0), out, knobs);
    t0 = cycles();
    wh_te_render(VOICE(0), out, knobs);
    cycles_heavy = cycles() - t0;
    CHECK(8, cycles_heavy < 40000);             /* 4 sounding: 4 x 40k = TRIO's 6 x 25k */

    /* 9: TE's LFO on slot 3 changes the low bits TE hands over (NOISE, KEYBOARD): the fields come
     * from the stored knobs (the same sound: no noise), only the contour amount moves (by 0.4 %) */
    {
        int16_t lfo[8];
        int a, b;

        for (i = 0; i < 8; i++)
            K(i) = lfo[i] = knobs[i] = saw[i];
        lfo[2] = (int16_t)(knobs[2] ^ 63);      /* white noise at full, keyboard full */
        pitch(HZ(220.0));
        wh_te_note_on(VOICE(4), 0, 0);
        wh_te_note_on(VOICE(5), 0, 0);
        for (n = 0, ok = 1; n < 40; n++) {
            wh_te_render(VOICE(4), out, knobs);
            a = peak();
            wh_te_render(VOICE(5), out, lfo);
            b = peak();
            ok &= n < 4 || (a - b < 300 && b - a < 300);
        }
        CHECK(9, ok);
    }

    /* 10: the engine screen draws (bank entry 2, page 1; a key held and sounding), and page 5 into
     * a second buffer */
    *TE_SHIFT_HELD = 1;
    turn(0, 1024);
    turn(0, 1024);
    *TE_SHIFT_HELD = 0;
    for (i = 0; i < 8; i++)
        knobs[i] = K(i);
    hook_key(41, 1);
    pitch(HZ(440.0));
    wh_te_note_on(VOICE(1), 0, 0);
    wh_te_render(VOICE(1), out, knobs);
    mo_page = 0;
    hook_frame((uint16_t *)FB);
    mo_page = 4;
    hook_frame((uint16_t *)(FB + 0x40000));
    mo_page = 0;
    hook_key(41, 0);
    CHECK(10, mo_bank == 1);

    /* 11: six notes: the 4 newest sound, the 2 oldest fade out (one block) and then render silence */
    for (i = 0; i < 8; i++)
        knobs[i] = saw[i];
    for (i = 0; i < 6; i++) {
        pitch(HZ(220.0 + 30 * i));
        wh_te_note_on(VOICE(i), 0, 0);
    }
    for (n = 0; n < 3; n++) {
        int v;

        ok = 0;
        for (v = 0; v < 6; v++) {
            wh_te_render(VOICE(v), out, knobs);
            ok += loud();
        }
    }
    CHECK(11, ok == 4);

    /* 12: keyboard picture (as STATION 1): a held key (id 41: note 4 = A, TE octave +1 -> A4) lights A4
     * even long after the start, though its voice starts 2 semitones low (TE's glide), and goes dark
     * on release while TE still renders the voice */
    {
        volatile uint16_t *fb = (volatile uint16_t *)FB;
        const uint16_t green = (uint16_t)(((0x3d >> 3) << 11) | ((0xdc >> 2) << 5) | (0x97 >> 3));
        int lit_held, lit_g4, lit_after;

        hook_key(41, 1);
        pitch((uint32_t)(392.0 / 88200 * 4294967296.0));
        wh_te_note_on(VOICE(2), 0, 0);
        for (n = 0; n < 300; n++)               /* > FLASH blocks */
            wh_te_render(VOICE(2), out, knobs);
        hook_frame((uint16_t *)FB);
        lit_held = fb[(319 - 186) * 256 + 76] == green;   /* A4: white key 19, lower part */
        lit_g4 = fb[(319 - 181) * 256 + 76] == green;     /* G4: white key 18 */
        hook_key(41, 0);
        for (n = 0; n < 10; n++)
            wh_te_render(VOICE(2), out, knobs);
        hook_frame((uint16_t *)FB);
        lit_after = fb[(319 - 186) * 256 + 76] == green;
        CHECK(12, lit_held && !lit_g4 && !lit_after);
    }
    /* 13: the sound's name over the panel: shown when a sound is picked, for 1 s (Seq ticks of 64
     * samples), then gone */
    {
        int on_pick, on_late, off;

        *TE_SHIFT_HELD = 1;
        turn(0, 1024);
        *TE_SHIFT_HELD = 0;
        hook_frame((uint16_t *)FB);
        on_pick = mo_title_on;
        for (n = 0; n < 680; n++)               /* 0.99 s */
            mo_tick();
        hook_frame((uint16_t *)FB);
        on_late = mo_title_on;
        for (n = 0; n < 10; n++)                /* 1.004 s */
            mo_tick();
        hook_frame((uint16_t *)FB);
        off = !mo_title_on;
        CHECK(13, on_pick && on_late && off);
    }
    return fails;
}
