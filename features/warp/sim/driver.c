/* WARP bench (build/apps/op1plus): engine host + WARP glue against stub TE functions and TE's
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
extern volatile uint8_t wp_page;
extern volatile int8_t wp_bank;

#define INC         ((volatile uint32_t *)0xff801950)
#define VOICE(i)    ((void *)(0xffb00000u + (i) * 0x24))
#define P2(t, v)    ((int16_t)(((t) << 12) | (v)))

static int drwave_renders, drwave_notes, saves, fxsaves, keys_seen, knobs_seen;
static int16_t out[128], first[128];
volatile uint32_t cycles_plain, cycles_warp, cycles_heavy;     /* read by run_warptest.py */
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

#define CHECK(bit, c) do { if (!(c)) fails |= 1 << (bit); } while (0)
#define K(i)        TE_SYNTH_KNOBS[2 * 8 + (i)]  /* sound 3 */

int main(void)
{
    static const int16_t stock[8] = { 20924, 4313, 8063, 0, 32767, 0, 0, 0 };
    /* SINE, BEND, amount 0, colour 0; page 2 all 0 */
    static const int16_t sine[8] = { 512, 512, 0, 0, P2(6, 0), P2(6, 0), P2(6, 0), P2(6, 0) };
    int16_t knobs[8];
    int i, n, low, ok;
    uint32_t t0;

    __asm__ volatile("R0 = SYSCFG; BITSET (R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");  /* CCEN */
    compose_init();
    *TE_EDIT_MODE = 0;
    *TE_SYNTH_SUBSCREEN = 0;
    *TE_SYNTH_SLOT = 2;
    TE_SYNTH_TYPE[2] = TE_INSTR_DRWAVE;

    /* 0: plain drwave and a tag-7 look-alike stay TE's */
    for (i = 0; i < 8; i++)
        knobs[i] = stock[i];
    wh_te_render(VOICE(0), out, knobs);
    for (i = 4; i < 8; i++)
        knobs[i] = P2(7, 100);
    wh_te_render(VOICE(0), out, knobs);
    CHECK(0, drwave_renders == 2 && out[0] == 1234);

    /* 1: tag 6 = WARP: plain sine at 441 Hz from TE's pitch buffer; restart = same block */
    for (i = 0; i < 8; i++)
        K(i) = knobs[i] = sine[i];
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
    CHECK(1, drwave_renders == 0 && n >= 440 && n <= 442 && drwave_notes == 1);
    wh_te_note_on(VOICE(1), 0, 0);
    wh_te_render(VOICE(1), out, knobs);
    for (i = 0, ok = 1; i < 128; i++)
        ok &= out[i] == first[i];
    CHECK(1, ok);

    /* 2: tag 5 goes to WHEEL (a different sound from the same pitch), not drwave, not WARP */
    {
        static int16_t wk[8] = { 0, 0, 0, 0, P2(5, 2048), P2(5, 2048), P2(5, 2048), P2(5, 0) };
        int diff = 0;

        wh_te_note_on(VOICE(2), 0, 0);
        wh_te_render(VOICE(2), out, wk);
        for (i = 0; i < 128; i++)
            diff |= out[i] != first[i];
        CHECK(2, drwave_renders == 0 && diff && out[0] != 1234);
    }

    /* 3: knob push cycles 3 pages; engine_plus / TE do not see it */
    i = keys_seen;
    push(59, 0);
    CHECK(3, wp_page == 1 && keys_seen == i);
    push(59, 0);
    push(59, 0);
    CHECK(3, wp_page == 0);

    /* 4: page 1 AMOUNT up (slot 3, clamped at 16383); page 2 SWEEP keeps tag 6 */
    i = saves;
    for (n = 0; n < 60; n++)
        turn(2, 32767);
    CHECK(4, K(2) == 16383 && saves == i + 60 && knobs_seen == 0);
    push(59, 0);
    for (n = 0; n < 60; n++)
        turn(0, 32767);
    CHECK(4, K(4) == P2(6, 4095));
    for (n = 0; n < 60; n++)
        turn(0, -32767);
    CHECK(4, K(4) == P2(6, 0));
    push(59, 0);
    push(59, 0);

    /* 5: shift + blue = bank: entry 0 (CZ BASS), all 16 in drwave's ranges with tag 6, wraps */
    {
        static const int16_t hi[8] = { 24575, 16383, 16383, 32767, 32767, 32767, 32767, 32767 };
        int e, k;

        *TE_SHIFT_HELD = 1;
        turn(0, 1024);
        ok = wp_bank == 0;
        for (e = 0; e < 16; e++) {
            for (k = 0; k < 8; k++)
                ok &= K(k) >= 0 && K(k) <= hi[k];
            for (k = 4; k < 8; k++)
                ok &= (K(k) >> 12) == 6;
            turn(0, 1024);
        }
        ok &= wp_bank == 0;
        turn(0, -1024);
        ok &= wp_bank == 15;
        *TE_SHIFT_HELD = 0;
        CHECK(5, ok && knobs_seen == 0);
    }

    /* 6: page 3 = FX knobs: spring knob 1 inside TE's range, saved via TE's FX save */
    {
        volatile int16_t *rt = (volatile int16_t *)0x01de5c50 + TE_FX_SPRING * 24;
        volatile int16_t *fx = (volatile int16_t *)0xff801444 + 2 * 8;

        rt[0] = 1344; rt[1] = 16448;
        fx[0] = 8000;
        ((volatile int32_t *)0xff801734)[2] = TE_FX_SPRING;
        ((volatile uint8_t *)0x01aaac20)[2] = 1;
        wp_page = 2;
        for (n = 0; n < 200; n++)
            turn(0, 32767);
        CHECK(6, fx[0] == 16448 && fxsaves == 200);
        wp_page = 0;
    }

    /* 7: engine list: "warp" -> "drwave"; "wheel" and "lofi" still renamed by their features */
    {
        static te_string a, b, c, d;

        name(&a, "warp"); name(&b, "wheel"); name(&c, "lofi"); name(&d, "warps");
        engine_name(&a, 0, 0);
        engine_name(&b, 0, 0);
        engine_name(&c, 0, 0);
        engine_name(&d, 0, 0);
        CHECK(7, a.size == 6 && a.bx.sso[0] == 'd' && b.size == 6 && b.bx.sso[0] == 'd'
                 && c.size == 5 && c.bx.sso[0] == 'p' && d.size == 5 && d.bx.sso[0] == 'w');
    }

    /* 8: cost per voice block: plain sine, SAW + SYNC, FM + unison + colour + motion + sweep */
    for (i = 0; i < 8; i++)
        knobs[i] = sine[i];
    wh_te_render(VOICE(1), out, knobs);
    t0 = cycles();
    wh_te_render(VOICE(1), out, knobs);
    cycles_plain = cycles() - t0;
    knobs[0] = 5120; knobs[1] = 1536; knobs[2] = 8000;     /* SAW, SYNC, amount ~50 % */
    wh_te_render(VOICE(1), out, knobs);
    t0 = cycles();
    wh_te_render(VOICE(1), out, knobs);
    cycles_warp = cycles() - t0;
    knobs[1] = 8704; knobs[3] = 8000;                      /* FM, colour */
    knobs[4] = P2(6, 3000); knobs[5] = P2(6, 2000); knobs[6] = P2(6, 3000); knobs[7] = P2(6, 4095);
    wh_te_note_on(VOICE(1), 0, 0);
    t0 = cycles();
    wh_te_render(VOICE(1), out, knobs);
    cycles_heavy = cycles() - t0;
    CHECK(8, cycles_heavy < 30000);

    /* 9: the engine screen draws (bank entry SYNC LEAD, page 1) - dumped as a screenshot */
    *TE_SHIFT_HELD = 1;
    turn(0, 1024);
    turn(0, 1024);
    *TE_SHIFT_HELD = 0;
    wp_page = 0;
    hook_frame((uint16_t *)FB);
    CHECK(9, wp_bank == 1);
    return fails;
}
