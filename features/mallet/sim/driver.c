/* MALLET bench (build/apps/op1plus): drwave voice render/note_on wrappers against stub TE functions
 * and TE's per-sample pitch buffer, page pushes, knob edits + TE's save, the bank, the engine-list
 * rename, screens, and the cost per 128-sample voice block in CPU cycles. Returns failed-check
 * bits (0 = pass). */
#include <op1/te246.h>

extern void ml_te_render(void *self, int16_t *out, int16_t *knobs);    /* engine host's (linked as) */
extern void compose_init(void);
extern void ml_te_note_on(void *self, int a1, int a2);
extern void engine_name(te_string *out, void *db, const char *sql);    /* composer's chain */
extern void hook_key(int key, int down);
extern void hook_knob(int knob, int16_t *diff);
extern void hook_frame(uint16_t *buf);
extern char FB[];
extern volatile uint8_t ml_page, ep_page[4];
extern volatile int8_t ml_bank;
extern volatile uint8_t ml_title_on;
extern void ml_tick(void);

#define INC         ((volatile uint32_t *)0xff801950)
#define VOICE(i)    ((void *)(0xffb00000u + (i) * 0x24))
#define MK(v)       (0x7000 | (v))              /* page 2 with MALLET's marker */
#define FBN(i)      (FB + (i) * 0x40000)        /* screens 0..7: one per instrument */
#define FB4         FBN(8)                      /* scratch (not saved) */

static int drwave_renders, drwave_notes, saves, fxsaves, keys_seen, knobs_seen;
static int16_t out[128];
volatile uint32_t cycles_plain, cycles_full, cycles_strike;    /* read by run_mallettest.py */
volatile uint32_t cycles_first[8], cycles_frame[8];             /* per instrument screen: its first frame, the next */
volatile int fails;                                             /* read by run_mallettest.py */

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

static void turn(int knob, int16_t d)
{
    hook_knob(knob, &d);
}

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

/* shift + blue turns from the current bank entry to entry e */
static void bank_to(int e)
{
    *TE_SHIFT_HELD = 1;
    while (ml_bank != e)
        turn(0, 1024);
    *TE_SHIFT_HELD = 0;
}

#define CHECK(bit, c) do { if (!(c)) fails |= 1 << (bit); } while (0)
#define K(i)        TE_SYNTH_KNOBS[2 * 8 + (i)]  /* sound 3 */

int main(void)
{
    /* a stock drwave preset ("fake piano"): slot 5 32767, 6-8 zero; its slot 5 carries tag 7 */
    static const int16_t stock[8] = { 20924, 4313, 8063, 0, 32767, 0, 0, 0 };
    /* VIBES, soft mallet, page 2 MOTOR 0, TUBE 0, DETUNE centre, TONE open */
    static const int16_t vibes[8] = { 3072, 0, 8192, 8192, MK(0), MK(0), MK(2048), MK(4095) };
    int16_t knobs[8];
    int i, n, low, ok, before;
    uint32_t t0;

    __asm__ volatile("R0 = SYSCFG; BITSET (R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");  /* CCEN */
    compose_init();                             /* engines register with the engine host */
    *TE_EDIT_MODE = 0;
    *TE_SYNTH_SUBSCREEN = 0;
    *TE_SYNTH_SLOT = 2;
    TE_SYNTH_TYPE[2] = TE_INSTR_DRWAVE;
    for (i = 0; i < 8; i++)
        K(i) = knobs[i] = stock[i];

    /* 0: plain drwave sound -> TE's drwave render, untouched; its knob turn and push are TE's */
    ml_te_render(VOICE(0), out, knobs);
    CHECK(0, drwave_renders == 1 && out[0] == 1234 && out[127] == 1234);
    turn(0, 100);
    CHECK(0, knobs_seen == 1 && saves == 0);
    push(59, 0);
    CHECK(0, ep_page[0] == 1 && ml_page == 0);
    push(59, 0);

    /* 1: note_on goes to TE's drwave note_on */
    ml_te_note_on(VOICE(1), 0, 0);
    CHECK(1, drwave_notes == 1);

    /* 2: MALLET (VIBES, soft mallet) renders from TE's pitch buffer: 441 Hz -> ~220 cycles in
     * 0.5 s (345 blocks) */
    for (i = 0; i < 8; i++)
        K(i) = knobs[i] = vibes[i];
    pitch((uint32_t)(441.0 / 88200 * 4294967296.0));
    ml_te_note_on(VOICE(1), 0, 0);
    drwave_renders = 0;
    for (n = 0, low = 1, i = 0; i < 345; i++) {
        int j;
        ml_te_render(VOICE(1), out, knobs);
        for (j = 0; j < 128; j++) {
            if (low && out[j] > 200) { n++; low = 0; }
            else if (out[j] < -200) low = 1;
        }
    }
    CHECK(2, drwave_renders == 0 && n >= 218 && n <= 223);

    /* 3: a new strike on the ringing voice (VIBES ring on: it is damped, then struck): louder again */
    before = peak();
    ml_te_note_on(VOICE(1), 0, 0);
    for (i = 0, n = 0; i < 8; i++) {
        ml_te_render(VOICE(1), out, knobs);
        n = peak() > n ? peak() : n;
    }
    CHECK(3, n > before + before / 4 && drwave_notes == 3);

    /* 4: knob push on a MALLET sound flips the page; engine_plus and TE do not see it */
    i = keys_seen;
    push(59, 0);
    CHECK(4, ml_page == 1 && ep_page[0] == 0 && keys_seen == i);

    /* 5: page 2, blue knob (MOTOR, continuous): slot 5 up, marker kept, saved, TE's knob handler
     * skipped; clamps inside the marker */
    i = saves;
    n = knobs_seen;
    turn(0, 1024);
    CHECK(5, (K(4) & 0xf000) == 0x7000 && (K(4) & 0x0fff) > 0 && saves == i + 1 && knobs_seen == n);
    for (i = 0; i < 200; i++)
        turn(0, 32767);
    CHECK(5, K(4) == MK(0x0fff));
    for (i = 0; i < 200; i++)
        turn(0, -32767);
    CHECK(5, K(4) == MK(0));

    /* 6: two more pushes: page 3, then page 1. Blue knob (INSTR, 8 values): one per detent,
     * clamped at TUBULAR / MARIMBA */
    push(61, 0);
    CHECK(6, ml_page == 2);
    push(61, 0);
    CHECK(6, ml_page == 0);
    low = (K(0) << 1) * 8 >> 15;
    turn(0, 1024);
    CHECK(6, ((K(0) << 1) * 8 >> 15) == low + 1 && low == 1);
    for (i = 0; i < 20; i++)
        turn(0, 1024);
    CHECK(6, ((K(0) << 1) * 8 >> 15) == 7);
    for (i = 0; i < 20; i++)
        turn(0, -1024);
    CHECK(6, ((K(0) << 1) * 8 >> 15) == 0 && knobs_seen == n);

    /* 7: shift + knob on a MALLET sound (not blue): consumed, no change; shift + push: not ours */
    i = saves;
    low = K(1);
    *TE_SHIFT_HELD = 1;
    turn(1, 1000);
    *TE_SHIFT_HELD = 0;
    CHECK(7, saves == i && knobs_seen == n && K(1) == low);
    i = keys_seen;
    push(60, 1);
    CHECK(7, ml_page == 0 && keys_seen == i + 2);

    /* 8: other screens are TE's: envelope subscreen knob goes to TE */
    *TE_SYNTH_SUBSCREEN = 1;
    turn(0, 100);
    CHECK(8, knobs_seen == n + 1);
    *TE_SYNTH_SUBSCREEN = 0;
    n = knobs_seen;

    /* 9: another engine with the same slot values is not MALLET */
    TE_SYNTH_TYPE[2] = TE_INSTR_FM;
    i = knobs_seen;
    turn(0, 100);
    CHECK(9, knobs_seen == i + 1 || ep_page[0] == 0);
    TE_SYNTH_TYPE[2] = TE_INSTR_DRWAVE;
    n = knobs_seen;

    /* 10: every value MALLET stores is inside drwave's ranges (boot-time validator) */
    {
        static const int16_t hi[8] = { 24575, 16383, 16383, 32767, 32767, 32767, 32767, 32767 };
        int k;

        for (ok = 1, k = 0; k < 4; k++) {
            push(59, 0);                        /* pages 2, 3, 1, 2 */
            for (i = 0; i < 40; i++) {
                turn(0, 32767); turn(1, 32767); turn(2, 32767); turn(3, 32767);
            }
            for (i = 0; i < 8; i++)
                ok &= K(i) >= 0 && K(i) <= hi[i];
            for (i = 0; i < 40; i++) {
                turn(0, -32767); turn(1, -32767); turn(2, -32767); turn(3, -32767);
            }
            for (i = 0; i < 8; i++)
                ok &= K(i) >= 0 && K(i) <= hi[i];
        }
        for (i = 4; i < 8; i++)
            ok &= (K(i) & 0xf000) == 0x7000;
        CHECK(10, ok);
        while (ml_page)
            push(59, 0);
    }

    /* 12: engine list: "mallet" becomes "drwave" (MALLET), "wheel" still does (WHEEL, same shared
     * point), others untouched */
    {
        static te_string a, b, c, d;

        name(&a, "mallet"); name(&b, "wheel"); name(&c, "drwave"); name(&d, "mallets");
        ml_page = 1;
        engine_name(&a, 0, 0);
        engine_name(&b, 0, 0);
        engine_name(&c, 0, 0);
        engine_name(&d, 0, 0);
        CHECK(12, a.size == 6 && a.bx.sso[0] == 'd' && a.bx.sso[5] == 'e' && a.bx.sso[6] == 0
                  && ml_page == 0 && b.size == 6 && b.bx.sso[0] == 'd' && c.size == 6
                  && c.bx.sso[0] == 'd' && d.size == 7 && d.bx.sso[0] == 'm');
    }

    /* 14: page 3 = the sound's FX knobs: spring's ranges (TE's table), saved via TE's FX save */
    {
        static const int16_t spring[4][2] = { { 1344, 16448 }, { 7744, 16448 }, { 0, 16384 }, { 0, 32767 } };
        volatile int16_t *rt = (volatile int16_t *)0x01de5c50 + TE_FX_SPRING * 24;
        volatile int16_t *fx = (volatile int16_t *)0xff801444 + 2 * 8;

        for (i = 0; i < 8; i++)
            K(i) = vibes[i];
        for (i = 0; i < 4; i++) {
            rt[i * 3] = spring[i][0];
            rt[i * 3 + 1] = spring[i][1];
            fx[i] = 8000 + 1000 * i;
        }
        ((volatile int32_t *)0xff801734)[2] = TE_FX_SPRING;
        ((volatile uint8_t *)0x01aaac20)[2] = 1;
        ml_page = 2;
        n = knobs_seen;
        i = fxsaves;
        turn(1, 1024);
        ok = fx[1] > 9000 && fxsaves == i + 1;
        for (i = 0; i < 200; i++)
            turn(0, 32767);
        ok &= fx[0] == 16448 && knobs_seen == n;
        CHECK(14, ok);
        push(59, 0);                            /* page 3 -> page 1 */
        CHECK(14, ml_page == 0);
    }

    /* 15: shift + blue turn browses the built-in bank: entry 0 = MARIMBA, one entry per click,
     * wraps; every entry inside drwave's ranges and marked; saved */
    {
        static const int16_t hi[8] = { 24575, 16383, 16383, 32767, 32767, 32767, 32767, 32767 };
        int e, k;

        i = saves;
        *TE_SHIFT_HELD = 1;
        turn(0, 1024);
        ok = ml_bank == 0 && saves == i + 1 && ((K(0) << 1) * 8 >> 15) == 0;
        turn(0, -1024);
        ok &= ml_bank == 15;                    /* wraps backwards */
        turn(0, 1024);
        for (e = 0; e < 16; e++) {
            for (k = 0; k < 8; k++)
                ok &= K(k) >= 0 && K(k) <= hi[k];
            for (k = 4; k < 8; k++)
                ok &= (K(k) & 0xf000) == 0x7000;
            turn(0, 1024);
        }
        ok &= ml_bank == 0;
        turn(1, 30000);                         /* shift + other knobs: nothing */
        *TE_SHIFT_HELD = 0;
        CHECK(15, ok && knobs_seen == n);
    }

    /* 11: cost per voice block: MARIMBA (3 modes) ringing, TUBULAR (6 modes) ringing, TUBULAR's
     * strike block (hard mallet: pulse + click) */
    bank_to(0);
    for (i = 0; i < 8; i++)
        knobs[i] = K(i);
    pitch((uint32_t)(261.63 / 88200 * 4294967296.0));
    ml_te_note_on(VOICE(1), 0, 0);
    ml_te_render(VOICE(1), out, knobs);
    ml_te_render(VOICE(1), out, knobs);
    t0 = cycles();
    ml_te_render(VOICE(1), out, knobs);
    cycles_plain = cycles() - t0;
    bank_to(14);
    for (i = 0; i < 8; i++)
        knobs[i] = K(i);
    ml_te_note_on(VOICE(1), 0, 0);
    t0 = cycles();
    ml_te_render(VOICE(1), out, knobs);
    cycles_strike = cycles() - t0;
    ml_te_render(VOICE(1), out, knobs);
    t0 = cycles();
    ml_te_render(VOICE(1), out, knobs);
    cycles_full = cycles() - t0;
    CHECK(11, cycles_full < 20000 && cycles_strike < 30000);    /* WHEEL: 30000 / 40000 */

    /* 16: the engine screen draws every instrument (bank entries MARIMBA, VIBES MOTOR, XYLOPHONE,
     * GLOCKENSPIEL, STEELPAN, HANDPAN, KALIMBA, TUBULAR BELL) with three keys held and sounding
     * (A4, B4, C5: C5 the newest, the mallet on it), page 1 (VIBES: page 2, the motor turning) */
    {
        static const int8_t keys[3] = { 41, 42, 28 };
        static const double hz[3] = { 440.0, 493.88, 523.25 };
        static const int8_t entry[8] = { 0, 4, 6, 8, 10, 12, 13, 14 };
        int s;

        for (i = 0; i < 3; i++)
            hook_key(keys[i], 1);
        for (s = 0; s < 8; s++) {
            bank_to(entry[s]);
            for (i = 0; i < 8; i++)
                knobs[i] = K(i);
            for (i = 0; i < 3; i++) {
                pitch((uint32_t)(hz[i] / 88200 * 4294967296.0));
                ml_te_note_on(VOICE(i), 0, 0);
                for (n = 0; n < 15; n++)        /* the motor turns */
                    ml_te_render(VOICE(i), out, knobs);
            }
            for (n = 0; n < 700; n++)           /* the title's second runs out */
                ml_tick();
            ml_page = s == 1 ? 1 : 0;
            t0 = cycles();
            hook_frame((uint16_t *)FBN(s));
            cycles_first[s] = cycles() - t0;
            t0 = cycles();                      /* the next frame: the same shapes again */
            hook_frame((uint16_t *)FBN(s));
            cycles_frame[s] = cycles() - t0;
        }
        ml_page = 0;
        for (i = 0; i < 3; i++)
            hook_key(keys[i], 0);
        CHECK(16, ml_bank == 14);
    }

    /* 17: bars lit as STATION 1's keys: MARIMBA, a held key (id 41 = A4) lights the A4 bar even
     * long after the start, though its voice starts 2 semitones low (TE's glide), not G4; dark on
     * release */
    {
        volatile uint16_t *fb = (volatile uint16_t *)FB4;
        const uint16_t green = (uint16_t)(((0x3d >> 3) << 11) | ((0xdc >> 2) << 5) | (0x97 >> 3));
        int lit_held, lit_g4, lit_after;

        bank_to(0);
        for (i = 0; i < 8; i++)
            knobs[i] = K(i);
        hook_key(41, 1);
        pitch((uint32_t)(392.0 / 88200 * 4294967296.0));
        ml_te_note_on(VOICE(2), 0, 0);
        for (n = 0; n < 300; n++)               /* > FLASH blocks */
            ml_te_render(VOICE(2), out, knobs);
        for (n = 0; n < 700; n++)
            ml_tick();
        hook_frame((uint16_t *)FB4);
        lit_held = fb[(319 - 236) * 256 + 59] == green;   /* A4: natural bar 12 */
        lit_g4 = fb[(319 - 219) * 256 + 59] == green;     /* G4: natural bar 11 */
        hook_key(41, 0);
        for (n = 0; n < 10; n++)
            ml_te_render(VOICE(2), out, knobs);
        hook_frame((uint16_t *)FB4);
        lit_after = fb[(319 - 236) * 256 + 59] == green;
        CHECK(17, lit_held && !lit_g4 && !lit_after);
    }

    /* 18: the sound's name over the instrument: shown when a sound is picked, for 1 s */
    {
        int on_pick, on_late, off;

        *TE_SHIFT_HELD = 1;
        turn(0, 1024);
        *TE_SHIFT_HELD = 0;
        hook_frame((uint16_t *)FB4);
        on_pick = ml_title_on;
        for (n = 0; n < 680; n++)               /* 0.99 s */
            ml_tick();
        hook_frame((uint16_t *)FB4);
        on_late = ml_title_on;
        for (n = 0; n < 10; n++)                /* 1.004 s */
            ml_tick();
        hook_frame((uint16_t *)FB4);
        off = !ml_title_on;
        CHECK(18, on_pick && on_late && off);
    }

    /* 13: back to a stock drwave preset (its slot 5 alone carries tag 7): TE's render and knobs */
    for (i = 0; i < 8; i++)
        K(i) = knobs[i] = stock[i];
    drwave_renders = 0;
    i = knobs_seen;
    ml_te_render(VOICE(1), out, knobs);
    turn(3, 100);
    CHECK(13, drwave_renders == 1 && knobs_seen == i + 1);
    return fails;
}
