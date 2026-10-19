/* WHEEL bench (build/apps/op1plus): drwave voice render/note_on wrappers against stub TE functions
 * and TE's per-sample pitch buffer, page pushes, knob edits + TE's save, the engine-list rename,
 * and the cost per 128-sample voice block in CPU cycles. Returns failed-check bits (0 = pass). */
#include <op1/te246.h>

extern void wh_te_render(void *self, int16_t *out, int16_t *knobs);    /* engine host's (linked as) */
extern void compose_init(void);
extern void wh_te_note_on(void *self, int a1, int a2);
extern void engine_name(te_string *out, void *db, const char *sql);    /* composer's chain */
extern void hook_key(int key, int down);
extern void hook_knob(int knob, int16_t *diff);
extern void hook_frame(uint16_t *buf);
extern char FB[];
extern volatile uint8_t wh_page, lf_page, ep_page[4];
extern volatile int8_t wh_bank;
extern volatile uint8_t wh_title_on;
extern void wh_tick(void);

#define INC         ((volatile uint32_t *)0xff801950)
#define VOICE(i)    ((void *)(0xffb00000u + (i) * 0x24))
#define CENTRE      (0x5000 | 2048)             /* page 2 marker, offset 0 */

static int drwave_renders, drwave_notes, saves, fxsaves, keys_seen, knobs_seen;
static int16_t out[128], first[128];
volatile uint32_t cycles_flute, cycles_full, cycles_heavy;     /* read by run_wheeltest.py */
volatile int fails;                                             /* read by run_wheeltest.py */

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

#define CHECK(bit, c) do { if (!(c)) fails |= 1 << (bit); } while (0)
#define K(i)        TE_SYNTH_KNOBS[2 * 8 + (i)]  /* sound 3 */

int main(void)
{
    /* a stock drwave preset ("fake piano"): slot 5 32767, 6-8 zero */
    static const int16_t stock[8] = { 20924, 4313, 8063, 0, 32767, 0, 0, 0 };
    static const int16_t flute[8] = { 0, 0, 0, 0, CENTRE, CENTRE, CENTRE, 0x5000 };
    int16_t knobs[8];
    int i, n, low, ok;
    uint32_t t0;

    __asm__ volatile("R0 = SYSCFG; BITSET (R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");  /* CCEN */
    compose_init();                             /* engines register with the engine host */
    *TE_EDIT_MODE = 0;
    *TE_SYNTH_SUBSCREEN = 0;
    *TE_SYNTH_SLOT = 2;
    TE_SYNTH_TYPE[2] = TE_INSTR_DRWAVE;
    for (i = 0; i < 8; i++)
        K(i) = knobs[i] = stock[i];

    /* 0: plain drwave sound -> TE's drwave render, untouched; its knob turn and push are TE's
     * (push: engine_plus' paging, as before WHEEL) */
    wh_te_render(VOICE(0), out, knobs);
    CHECK(0, drwave_renders == 1 && out[0] == 1234 && out[127] == 1234);
    turn(0, 100);
    CHECK(0, knobs_seen == 1 && saves == 0);
    push(59, 0);
    CHECK(0, ep_page[0] == 1 && wh_page == 0);
    push(59, 0);

    /* 1: note_on goes to TE's drwave note_on */
    wh_te_note_on(VOICE(1), 0, 0);
    CHECK(1, drwave_notes == 1);

    /* 2: WHEEL (FLUTE, everything off) renders from TE's pitch buffer: 441 Hz -> 441 cycles in
     * 689 blocks */
    for (i = 0; i < 8; i++)
        K(i) = knobs[i] = flute[i];
    pitch((uint32_t)(441.0 / 88200 * 4294967296.0));
    wh_te_note_on(VOICE(1), 0, 0);
    drwave_renders = 0;
    for (n = 0, low = 1, i = 0; i < 689; i++) {
        int j;
        wh_te_render(VOICE(1), out, knobs);
        if (i == 0)
            for (j = 0; j < 128; j++)
                first[j] = out[j];
        for (j = 0; j < 128; j++) {
            if (low && out[j] > 200) { n++; low = 0; }
            else if (out[j] < -200) low = 1;
        }
    }
    CHECK(2, drwave_renders == 0 && n >= 440 && n <= 442);

    /* 3: a new note restarts the voice: same first block */
    wh_te_note_on(VOICE(1), 0, 0);
    wh_te_render(VOICE(1), out, knobs);
    for (i = 0, ok = 1; i < 128; i++)
        ok &= out[i] == first[i];
    CHECK(3, ok && drwave_notes == 3);

    /* 4: knob push on a WHEEL sound flips the page; engine_plus and TE do not see it */
    i = keys_seen;
    push(59, 0);
    CHECK(4, wh_page == 1 && ep_page[0] == 0 && lf_page == 0 && keys_seen == i);

    /* 5: page 2, blue knob (BASS, 17 values -8..+8): one value per detent, slot 5 at the value's
     * zone centre, marker kept, saved, TE's knob handler skipped; clamps at -8 / +8 */
    i = saves;
    n = knobs_seen;
    low = ((K(4) & 0x0fff) << 3) * 17 >> 15;
    turn(0, 1024);
    CHECK(5, (K(4) & 0xf000) == 0x5000 && (((K(4) & 0x0fff) << 3) * 17 >> 15) == low + 1
             && saves == i + 1 && knobs_seen == n);
    for (i = 0; i < 40; i++)
        turn(0, 32767);                         /* clamps inside the marker */
    CHECK(5, K(4) == (0x5000 | 3975));          /* +8: zone 16 of 17 */
    for (i = 0; i < 40; i++)
        turn(0, -32767);
    CHECK(5, K(4) == (0x5000 | 120));           /* -8: zone 0 */

    /* 6: two more pushes: page 3, then page 1. White knob (DRIVE): slot 3 up; clamped at
     * drwave's slot-3 maximum (16383) */
    push(61, 0);
    CHECK(6, wh_page == 2);
    push(61, 0);
    for (i = 0; i < 40; i++)
        turn(2, 32767);
    CHECK(6, wh_page == 0 && K(2) == 16383 && knobs_seen == n);
    for (i = 0; i < 40; i++)
        turn(2, -32767);
    CHECK(6, K(2) == 0);

    /* 19: orange knob (ROTARY, 3 values): OFF -> SLOW -> FAST in one detent each, then clamped */
    for (i = 0; i < 3; i++)
        turn(3, -1024);
    low = (K(3) << 1) * 3 >> 15;
    turn(3, 1024);
    ok = (K(3) << 1) * 3 >> 15;
    turn(3, 1024);
    n = (K(3) << 1) * 3 >> 15;
    turn(3, 1024);
    CHECK(19, low == 0 && ok == 1 && n == 2 && ((K(3) << 1) * 3 >> 15) == 2);
    n = knobs_seen;

    /* 7: shift + knob on a WHEEL sound: consumed, no change; shift + push: not WHEEL's */
    i = saves;
    *TE_SHIFT_HELD = 1;
    turn(1, 1000);
    *TE_SHIFT_HELD = 0;
    CHECK(7, saves == i && knobs_seen == n && K(1) == 0);
    i = keys_seen;
    push(60, 1);
    CHECK(7, wh_page == 0 && keys_seen == i + 2);

    /* 8: other screens are TE's: envelope subscreen knob and push go to TE */
    *TE_SYNTH_SUBSCREEN = 1;
    turn(0, 100);
    CHECK(8, knobs_seen == n + 1);
    *TE_SYNTH_SUBSCREEN = 0;

    /* 9: another engine with the same slot values is not WHEEL (fm ignores slots 5-8) */
    TE_SYNTH_TYPE[2] = TE_INSTR_FM;
    i = knobs_seen;
    turn(0, 100);
    CHECK(9, knobs_seen == i + 1 || ep_page[0] == 0);
    TE_SYNTH_TYPE[2] = TE_INSTR_DRWAVE;

    /* 10: every value WHEEL stores is inside drwave's ranges (boot-time validator) */
    {
        static const int16_t lo[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        static const int16_t hi[8] = { 24575, 16383, 16383, 32767, 32767, 32767, 32767, 32767 };
        int k;

        for (ok = 1, k = 0; k < 4; k++) {
            push(59, 0);                        /* both pages */
            for (i = 0; i < 40; i++) {
                turn(0, 32767); turn(1, 32767); turn(2, 32767); turn(3, 32767);
            }
            for (i = 0; i < 8; i++)
                ok &= K(i) >= lo[i] && K(i) <= hi[i];
            for (i = 0; i < 40; i++) {
                turn(0, -32767); turn(1, -32767); turn(2, -32767); turn(3, -32767);
            }
            for (i = 0; i < 8; i++)
                ok &= K(i) >= lo[i] && K(i) <= hi[i];
        }
        CHECK(10, ok);
    }

    /* 11: cost per voice block: FLUTE (1 bar), FULL (9 bars), FULL + 3rd perc + click +
     * rotary fast + drive */
    for (i = 0; i < 8; i++)
        knobs[i] = flute[i];
    wh_te_render(VOICE(1), out, knobs);
    t0 = cycles();
    wh_te_render(VOICE(1), out, knobs);
    cycles_flute = cycles() - t0;
    knobs[0] = 16383;                           /* FULL */
    wh_te_render(VOICE(1), out, knobs);
    t0 = cycles();
    wh_te_render(VOICE(1), out, knobs);
    cycles_full = cycles() - t0;
    knobs[1] = 5851; knobs[2] = 12000; knobs[3] = 16383; knobs[7] = 0x5fff;
    wh_te_note_on(VOICE(1), 0, 0);
    t0 = cycles();
    wh_te_render(VOICE(1), out, knobs);
    cycles_heavy = cycles() - t0;
    CHECK(11, cycles_full < 30000 && cycles_heavy < 40000);

    /* 12: engine list: "wheel" becomes "drwave" (WHEEL), "lofi" still becomes "pulse" (LOFI,
     * same shared point), others untouched */
    {
        static te_string a, b, c, d;

        name(&a, "wheel"); name(&b, "lofi"); name(&c, "drwave"); name(&d, "wheels");
        wh_page = 1;
        engine_name(&a, 0, 0);
        engine_name(&b, 0, 0);
        engine_name(&c, 0, 0);
        engine_name(&d, 0, 0);
        CHECK(12, a.size == 6 && a.bx.sso[0] == 'd' && a.bx.sso[5] == 'e' && a.bx.sso[6] == 0
                  && wh_page == 0 && b.size == 5 && b.bx.sso[0] == 'p' && c.size == 6
                  && c.bx.sso[0] == 'd' && d.size == 6 && d.bx.sso[0] == 'w');
    }

    /* 14: page 3 = the sound's FX knobs: spring's ranges (TE's table), saved via TE's FX save;
     * FX off: consumed, nothing changes */
    {
        static const int16_t spring[4][2] = { { 1344, 16448 }, { 7744, 16448 }, { 0, 16384 }, { 0, 32767 } };
        volatile int16_t *rt = (volatile int16_t *)0x01de5c50 + TE_FX_SPRING * 24;
        volatile int16_t *fx = (volatile int16_t *)0xff801444 + 2 * 8;

        for (i = 0; i < 8; i++)
            K(i) = flute[i];
        for (i = 0; i < 4; i++) {
            rt[i * 3] = spring[i][0];
            rt[i * 3 + 1] = spring[i][1];
            fx[i] = 8000 + 1000 * i;
        }
        ((volatile int32_t *)0xff801734)[2] = TE_FX_SPRING;
        ((volatile uint8_t *)0x01aaac20)[2] = 1;
        wh_page = 2;
        n = knobs_seen;
        i = fxsaves;
        turn(1, 1024);
        ok = fx[1] > 9000 && fxsaves == i + 1;
        for (i = 0; i < 200; i++) {
            turn(0, 32767); turn(1, 32767);
        }
        ok &= fx[0] == 16448 && fx[1] == 16448;
        for (i = 0; i < 200; i++) {
            turn(0, -32767); turn(1, -32767);
        }
        ok &= fx[0] == 1344 && fx[1] == 7744 && fx[2] == 10000 && knobs_seen == n;
        ((volatile uint8_t *)0x01aaac20)[2] = 0;
        i = fxsaves;
        turn(0, 32767);
        CHECK(14, ok && fx[0] == 1344 && fxsaves == i && knobs_seen == n);
        push(59, 0);                            /* page 3 -> page 1 */
        CHECK(14, wh_page == 0);
    }

    /* 15: shift + blue turn browses the built-in bank: entry 0 = JAZZ TRIO (JAZZ, 3RD, SLOW),
     * one entry per click, wraps; every entry inside drwave's ranges and marked; saved */
    {
        static const int16_t hi[8] = { 24575, 16383, 16383, 32767, 32767, 32767, 32767, 32767 };
        int e, k;

        wh_page = 0;
        i = saves;
        *TE_SHIFT_HELD = 1;
        turn(0, 1024);
        ok = wh_bank == 0 && saves == i + 1
             && (K(0) << 1) * 16 >> 15 == 3 && (K(1) << 1) * 7 >> 15 == 2 && (K(3) << 1) * 3 >> 15 == 1;
        turn(0, -1024);
        ok &= wh_bank == 23;                    /* wraps backwards */
        turn(0, 1024);
        for (e = 0; e < 24; e++) {
            for (k = 0; k < 8; k++)
                ok &= K(k) >= 0 && K(k) <= hi[k];
            for (k = 4; k < 8; k++)
                ok &= (K(k) & 0xf000) == 0x5000;
            turn(0, 1024);
        }
        ok &= wh_bank == 0;
        turn(1, 30000);                         /* shift + other knobs: nothing */
        *TE_SHIFT_HELD = 0;
        CHECK(15, ok && knobs_seen == n);
    }

    /* 16: the engine screen draws: bank entry 2 (SOUL JAZZ) with three keys held and sounding
     * (A4, C5, E5), page 1; page 2 into a second buffer */
    {
        static const int8_t keys[3] = { 41, 42, 28 };
        static const double hz[3] = { 440.0, 493.88, 523.25 };    /* the keys: A4, B4, C5 */

        *TE_SHIFT_HELD = 1;
        turn(0, 1024);
        *TE_SHIFT_HELD = 0;
        for (i = 0; i < 8; i++)
            knobs[i] = K(i);
        for (i = 0; i < 3; i++) {
            hook_key(keys[i], 1);
            pitch((uint32_t)(hz[i] / 88200 * 4294967296.0));
            wh_te_note_on(VOICE(i), 0, 0);
            wh_te_render(VOICE(i), out, knobs);
        }
        wh_page = 0;
        hook_frame((uint16_t *)FB);
        wh_page = 1;
        hook_frame((uint16_t *)(FB + 0x40000));
        wh_page = 0;
        for (i = 0; i < 3; i++)
            hook_key(keys[i], 0);
        CHECK(16, wh_bank == 1);
    }

    /* 17: keyboard picture (as STATION 1): a held key (id 41: note 4 = A, TE octave +1 -> A4) lights A4
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
        lit_held = fb[(319 - 255) * 256 + 78] == green;   /* A4: white key 12, lower part */
        lit_g4 = fb[(319 - 243) * 256 + 78] == green;     /* G4: white key 11 */
        hook_key(41, 0);
        for (n = 0; n < 10; n++)
            wh_te_render(VOICE(2), out, knobs);
        hook_frame((uint16_t *)FB);
        lit_after = fb[(319 - 255) * 256 + 78] == green;
        CHECK(17, lit_held && !lit_g4 && !lit_after);
    }

    /* 18: the sound's name over the organ: shown when a sound is picked, for 1 s (Seq ticks of 64
     * samples), then gone */
    {
        int on_pick, on_late, off;

        *TE_SHIFT_HELD = 1;
        turn(0, 1024);
        *TE_SHIFT_HELD = 0;
        hook_frame((uint16_t *)FB);
        on_pick = wh_title_on;
        for (n = 0; n < 680; n++)               /* 0.99 s */
            wh_tick();
        hook_frame((uint16_t *)FB);
        on_late = wh_title_on;
        for (n = 0; n < 10; n++)                /* 1.004 s */
            wh_tick();
        hook_frame((uint16_t *)FB);
        off = !wh_title_on;
        CHECK(18, on_pick && on_late && off);
    }

    /* 13: back to a stock drwave preset: TE's render, TE's knobs */
    for (i = 0; i < 8; i++)
        K(i) = knobs[i] = stock[i];
    drwave_renders = 0;
    i = knobs_seen;
    wh_te_render(VOICE(1), out, knobs);
    turn(3, 100);
    CHECK(13, drwave_renders == 1 && knobs_seen == i + 1);
    return fails;
}
