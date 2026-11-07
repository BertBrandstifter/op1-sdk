/* ACID 3 bench (build/apps/op1plus + acid): engine host + ACID glue against stub TE functions and
 * TE's per-sample pitch buffer. Returns failed-check bits (0 = pass). */
#include <op1/te246.h>

extern void wh_te_render(void *self, int16_t *out, int16_t *knobs);    /* the engine host's */
extern void wh_te_note_on(void *self, int a1, int a2);
extern void compose_init(void);
extern void engine_name(te_string *out, void *db, const char *sql);    /* composer's chain */
extern void hook_key(int key, int down);
extern void hook_knob(int knob, int16_t *diff);
extern void hook_frame(uint16_t *buf);
extern int kb_note_on(void *note, int extra);  /* composer's kb note chains (observers first) */
extern int kb_note_off(void *note);
extern char FB[];
extern volatile uint8_t ac_page;
extern volatile int8_t ac_bank;
extern volatile uint8_t ac_title_on;
extern volatile int8_t ac_note_shown;
extern void ac_tick(void);
extern int ac_param(const volatile int16_t *k, int id);
extern void ac_param_set(volatile int16_t *k, int id, int v);
extern void ac_bank_knobs(int e, int16_t *k);

#define INC         ((volatile uint32_t *)0xff801950)
#define VOICE(i)    ((void *)(0xffb00000u + (i) * 0x24))

static int drwave_renders, drwave_notes, saves, fxsaves, keys_seen, knobs_seen;
static int16_t out[128];
volatile uint32_t cycles_plain, cycles_heavy;   /* read by run_acidtest.py */
volatile int fails;
volatile int bank_peak[16], bank_rms[16];       /* read by run_acidtest.py */
volatile int acc_pk[4];

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

static int loud(void) { return peak() > 300; }

static void kb(int note, int down)
{
    te_note n;

    n.note = note;
    n.vel = -1;
    n.midi_out = 1;
    n.extra = 0;
    if (down)
        kb_note_on(&n, 0);
    else
        kb_note_off(&n);
}

#define CHECK(bit, c) do { if (!(c)) fails |= 1 << (bit); } while (0)
#define K(i)        TE_SYNTH_KNOBS[2 * 8 + (i)]  /* sound 3 */
#define HZ(f)       ((uint32_t)((f) / 88200.0 * 4294967296.0))

/* parameter ids (acid.c's page order) */
enum { P_CUTOFF, P_RES, P_ENVMOD, P_DECAY, P_ACCENT, P_ACCDEC, P_SWEEP, P_SPEED, P_WAVE, P_SUB,
       P_TUNE, P_SLIDE, P_SOFT, P_TRACK, P_FM, P_DRIVE, P_MUFF, P_ACCS, P_SLIDES, P_LEVEL, NP };

/* the 'acid 3' preset (feature.json content_sql) = bank entry 1 */
static const int16_t preset[8] = { 6445, 10893, 8965, 9612, 29168, 28672, 31360, 4096 };

/* a saw, filter open, no resonance / envelope / accents / slides, level 10 */
static void saw_knobs(int16_t *k)
{
    ac_bank_knobs(0, k);
    ac_param_set(k, P_CUTOFF, 255); ac_param_set(k, P_RES, 0); ac_param_set(k, P_ENVMOD, 0);
    ac_param_set(k, P_ACCS, 0); ac_param_set(k, P_SLIDES, 2); ac_param_set(k, P_DRIVE, 0);
    ac_param_set(k, P_SOFT, 0);
}

/* zero crossings (rising, hysteresis) over `blocks` blocks of one voice */
static int crossings(int v, int16_t *knobs, int blocks)
{
    int n = 0, low = 1, i, j;

    for (i = 0; i < blocks; i++) {
        wh_te_render(VOICE(v), out, knobs);
        for (j = 0; j < 128; j++) {
            if (low && out[j] > 2000) { n++; low = 0; }
            else if (out[j] < -2000) low = 1;
        }
    }
    return n;
}

int main(void)
{
    static const int16_t stock[8] = { 20924, 4313, 8063, 0, 32767, 0, 0, 0 };
    int16_t knobs[8], saw[8];
    int i, n, ok;
    uint32_t t0;

    __asm__ volatile("R0 = SYSCFG; BITSET (R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");  /* CCEN */
    compose_init();
    *TE_EDIT_MODE = 0;
    *TE_SYNTH_SUBSCREEN = 0;
    *TE_SYNTH_SLOT = 2;
    TE_SYNTH_TYPE[2] = TE_INSTR_DRWAVE;
    saw_knobs(saw);

    /* 0: plain drwave (and MALLET's tag 7 with slot 8 tag 1 missing) stays TE's; preset = entry 1 */
    for (i = 0; i < 8; i++)
        knobs[i] = stock[i];
    wh_te_render(VOICE(0), out, knobs);
    CHECK(0, drwave_renders == 1 && out[0] == 1234);
    ac_bank_knobs(0, knobs);
    for (ok = 1, i = 0; i < 8; i++)
        ok &= knobs[i] == preset[i];
    CHECK(0, ok);
    ok = (knobs[4] >> 12) == 7 && (knobs[5] >> 12) == 7 && (knobs[6] >> 12) == 7 && (knobs[7] >> 12) == 1;
    CHECK(0, ok);

    /* 1: extended tag 8 = ACID: a saw at 441 Hz from TE's pitch buffer */
    for (i = 0; i < 8; i++)
        K(i) = knobs[i] = saw[i];
    pitch(HZ(441.0));
    wh_te_note_on(VOICE(1), 0, 0);
    drwave_renders = 0;
    n = crossings(1, knobs, 689);
    CHECK(1, drwave_renders == 0 && n >= 439 && n <= 443);

    /* 2: mono: a new note on voice 2 sounds; voice 1 fades within the block and is silent after */
    pitch(HZ(330.0));
    wh_te_note_on(VOICE(2), 0, 0);
    wh_te_render(VOICE(1), out, knobs);
    wh_te_render(VOICE(2), out, knobs);
    ok = loud();
    wh_te_render(VOICE(1), out, knobs);         /* 2nd block: the fade */
    ok &= peak() < 30000;
    wh_te_render(VOICE(2), out, knobs);
    ok &= loud();
    for (n = 0; n < 3; n++) {
        wh_te_render(VOICE(1), out, knobs);
        ok &= peak() == 0;
        wh_te_render(VOICE(2), out, knobs);
        ok &= loud();
    }
    CHECK(2, ok);

    /* 3: slide: key A held, key B pressed -> B slides from A's pitch (SLIDES LEGATO, SLIDE 360 ms);
     * B alone (A up first) jumps */
    {
        int slid, jumped;

        ac_param_set(knobs, P_SLIDES, 0); ac_param_set(knobs, P_SLIDE, 31);
        for (i = 0; i < 8; i++)
            K(i) = knobs[i];
        kb(48, 1);
        pitch(HZ(110.0));
        wh_te_note_on(VOICE(3), 0, 0);
        crossings(3, knobs, 100);
        kb(60, 1);
        pitch(HZ(220.0));
        wh_te_note_on(VOICE(4), 0, 0);
        wh_te_render(VOICE(3), out, knobs);     /* the fading voice before (no output check) */
        slid = crossings(4, knobs, 35);         /* ~51 ms: 220 Hz = 11 cycles */
        kb(48, 0);
        kb(60, 0);
        pitch(HZ(110.0));
        wh_te_note_on(VOICE(5), 0, 0);
        crossings(5, knobs, 100);
        pitch(HZ(220.0));
        wh_te_note_on(VOICE(0), 0, 0);
        jumped = crossings(0, knobs, 35);
        CHECK(3, slid <= 9 && jumped >= 10);
        ac_param_set(knobs, P_SLIDES, 2);
        for (i = 0; i < 8; i++)
            K(i) = knobs[i];
    }

    /* 4: knob push cycles 6 pages */
    i = keys_seen;
    push(59, 0);
    CHECK(4, ac_page == 1 && keys_seen == i);
    for (n = 0; n < 4; n++)
        push(60, 0);
    CHECK(4, ac_page == 5);
    push(59, 0);
    CHECK(4, ac_page == 0);

    /* 5: page 1 CUTOFF clamps at 255 (fields below kept); page 2 SWEEP one step per detent (max
     * HIGH); page 3 TUNE to +12; the tag kept */
    {
        int acc = ac_param(&K(0), P_ACCENT), tune = ac_param(&K(0), P_TUNE);

        for (n = 0; n < 200; n++)
            turn(0, 32767);
        CHECK(5, ac_param(&K(0), P_CUTOFF) == 255 && knobs_seen == 0
                 && ac_param(&K(0), P_ACCENT) == acc && ac_param(&K(0), P_TUNE) == tune);
    }
    push(59, 0);
    ac_param_set(&K(0), P_SWEEP, 0);
    turn(2, 1024);
    CHECK(5, ac_param(&K(0), P_SWEEP) == 1);
    turn(2, 32767);
    turn(2, 32767);
    CHECK(5, ac_param(&K(0), P_SWEEP) == 2);
    push(59, 0);
    for (n = 0; n < 40; n++)
        turn(2, 1024);
    CHECK(5, ac_param(&K(0), P_TUNE) == 24);
    CHECK(5, (K(4) >> 12) == 7 && (K(5) >> 12) == 7 && (K(6) >> 12) == 7 && (K(7) >> 12) == 1);
    push(59, 0);
    push(59, 0);
    push(59, 0);
    push(59, 0);
    CHECK(5, ac_page == 0);

    /* 6: shift + blue = bank: 16 entries in drwave's ranges with tag 8, each audible and below
     * clipping (a 110 Hz note for 0.3 s, accented ones included), then P-- (the sound as it was) */
    {
        static const int16_t hi[8] = { 24575, 16383, 16383, 32767, 32767, 32767, 32767, 32767 };
        int16_t before[8];
        int e, k, b, j;

        for (k = 0; k < 8; k++)
            before[k] = K(k);
        *TE_SHIFT_HELD = 1;
        turn(0, 1024);
        ok = ac_bank == 0;
        for (e = 0; e < 16; e++) {
            int pk = 0;
            uint64_t sq = 0;

            for (k = 0; k < 8; k++)
                ok &= K(k) >= 0 && K(k) <= hi[k];
            ok &= (K(4) >> 12) == 7 && (K(5) >> 12) == 7 && (K(6) >> 12) == 7 && (K(7) >> 12) == 1;
            for (k = 0; k < 8; k++)
                knobs[k] = K(k);
            for (j = 0; j < 4; j++) {           /* 4 notes: some accented (ACCENTS) */
                pitch(HZ(j & 1 ? 146.8 : 110.0));
                wh_te_note_on(VOICE(1 + (j & 1)), 0, 0);
                for (b = 0; b < 52; b++) {
                    int s;

                    wh_te_render(VOICE(1 + (j & 1)), out, knobs);
                    for (s = 0; s < 128; s++) {
                        int x = out[s] < 0 ? -out[s] : out[s];

                        pk = x > pk ? x : pk;
                        sq += (uint64_t)((x >> 4) * (x >> 4));
                    }
                }
            }
            bank_peak[e] = pk;
            bank_rms[e] = (int)(sq / (4 * 52 * 128));    /* (rms / 16)^2 */
            ok &= pk > 3000 && pk < 32000;
            turn(0, 1024);
        }
        ok &= ac_bank == -1;
        for (k = 0; k < 8; k++)
            ok &= K(k) == before[k];
        turn(0, -1024);
        ok &= ac_bank == 15;
        turn(0, 1024);
        *TE_SHIFT_HELD = 0;
        CHECK(6, ok && knobs_seen == 0);
    }

    /* 7: page 6 = FX knobs */
    {
        volatile int16_t *rt = (volatile int16_t *)0x01de5c50 + TE_FX_SPRING * 24;
        volatile int16_t *fx = (volatile int16_t *)0xff801444 + 2 * 8;

        rt[0] = 1344; rt[1] = 16448;
        fx[0] = 8000;
        ((volatile int32_t *)0xff801734)[2] = TE_FX_SPRING;
        ((volatile uint8_t *)0x01aaac20)[2] = 1;
        ac_page = 5;
        for (n = 0; n < 200; n++)
            turn(0, 32767);
        CHECK(7, fx[0] == 16448 && fxsaves == 200);
        ac_page = 0;
    }

    /* 8: engine list: "acid 3" -> "drwave", "mono 3" too (the chain), "acid" untouched */
    {
        static te_string a, b, c;

        name(&a, "acid 3"); name(&b, "mono 3"); name(&c, "acid");
        engine_name(&a, 0, 0);
        engine_name(&b, 0, 0);
        engine_name(&c, 0, 0);
        CHECK(8, a.size == 6 && a.bx.sso[0] == 'd' && b.size == 6 && b.bx.sso[0] == 'd'
                 && c.size == 4 && c.bx.sso[0] == 'a');
    }

    /* 9: cost per audio block (one voice renders the mono state): a saw, filter open; + sub,
     * overdrive, resonance, filter FM, tracking, an accent with HIGH sweep, a slide, muffler */
    for (i = 0; i < 8; i++)
        K(i) = knobs[i] = saw[i];
    pitch(HZ(220.0));
    wh_te_note_on(VOICE(5), 0, 0);
    wh_te_render(VOICE(5), out, knobs);
    t0 = cycles();
    wh_te_render(VOICE(5), out, knobs);
    cycles_plain = cycles() - t0;
    ac_param_set(knobs, P_CUTOFF, 60); ac_param_set(knobs, P_RES, 230); ac_param_set(knobs, P_ENVMOD, 180);
    ac_param_set(knobs, P_SUB, 3); ac_param_set(knobs, P_DRIVE, 20); ac_param_set(knobs, P_FM, 20);
    ac_param_set(knobs, P_TRACK, 20); ac_param_set(knobs, P_ACCS, 1); ac_param_set(knobs, P_SWEEP, 2);
    ac_param_set(knobs, P_SLIDES, 1); ac_param_set(knobs, P_MUFF, 2);
    for (i = 0; i < 8; i++)
        K(i) = knobs[i];
    pitch(HZ(330.0));
    wh_te_note_on(VOICE(0), 0, 0);
    wh_te_render(VOICE(0), out, knobs);
    t0 = cycles();
    wh_te_render(VOICE(0), out, knobs);
    cycles_heavy = cycles() - t0;
    CHECK(9, cycles_heavy < 150000);           /* mono: one render per audio block (MONO 3: 4 x 37k) */

    /* 10: TE's LFO on slot 3 changes the low bits TE hands over (SWEEP SPEED SUB): the fields come
     * from the stored knobs (no sub): blocks alternating TE's copy and the stored knobs sound the same;
     * with SUB HI stored the same note does change */
    {
        int16_t lfo[8], sub[8];
        uint32_t a = 0, b = 0, a2 = 0, c = 0;
        int j;

        for (i = 0; i < 8; i++)
            K(i) = lfo[i] = knobs[i] = sub[i] = saw[i];
        lfo[2] = (int16_t)(knobs[2] ^ 0x30);    /* SUB HI in TE's copy */
        ac_param_set(sub, P_SUB, 3);
        pitch(HZ(220.0));
        wh_te_note_on(VOICE(4), 0, 0);
        for (n = 0; n < 86; n++) {
            wh_te_render(VOICE(4), out, n & 1 ? lfo : knobs);
            for (j = 0; j < 128; j++)
                *(n & 1 ? &b : &a) += (uint32_t)(out[j] < 0 ? -out[j] : out[j]);
        }
        wh_te_note_on(VOICE(4), 0, 0);
        for (n = 0; n < 43; n++) {
            wh_te_render(VOICE(4), out, knobs);
            for (j = 0; j < 128; j++)
                a2 += (uint32_t)(out[j] < 0 ? -out[j] : out[j]);
        }
        for (i = 0; i < 8; i++)
            K(i) = sub[i];
        wh_te_note_on(VOICE(4), 0, 0);
        for (n = 0; n < 43; n++) {
            wh_te_render(VOICE(4), out, sub);
            for (j = 0; j < 128; j++)
                c += (uint32_t)(out[j] < 0 ? -out[j] : out[j]);
        }
        CHECK(10, a / 100 > b / 100 * 97 / 100 && b / 100 > a / 100 * 97 / 100
                  && (c / 100 > a2 / 100 * 11 / 10 || a2 / 100 > c / 100 * 11 / 10));
    }

    /* 11: ACCENTS 2ND: every 2nd note accented (louder) */
    {
        volatile int *pk = acc_pk;
        int j, b;

        for (i = 0; i < 8; i++)
            knobs[i] = saw[i];
        ac_param_set(knobs, P_ACCS, 3); ac_param_set(knobs, P_ACCENT, 63); ac_param_set(knobs, P_CUTOFF, 80);
        ac_param_set(knobs, P_ACCDEC, 20); ac_param_set(knobs, P_LEVEL, 4);   /* below the limiter */
        for (i = 0; i < 8; i++)
            K(i) = knobs[i];
        for (j = 0; j < 4; j++) {
            pitch(HZ(110.0));
            wh_te_note_on(VOICE(j & 1), 0, 0);
            for (pk[j] = 0, b = 0; b < 30; b++) {
                wh_te_render(VOICE(j & 1), out, knobs);
                if (b >= 2)                     /* after the retrigger dip (the sound before's tail) */
                    pk[j] = peak() > pk[j] ? peak() : pk[j];
            }
        }
        /* the count may start on any note after the earlier checks: the two pairs alternate */
        CHECK(11, (pk[0] > pk[1] * 5 / 4 && pk[2] > pk[3] * 5 / 4) || (pk[1] > pk[0] * 5 / 4 && pk[3] > pk[2] * 5 / 4));
    }

    /* 12: the engine screen draws (bank entry 3, page 1, a key held), and page 2 */
    *TE_SHIFT_HELD = 1;
    turn(0, 1024);
    turn(0, 1024);
    turn(0, 1024);
    *TE_SHIFT_HELD = 0;
    for (i = 0; i < 8; i++)
        knobs[i] = K(i);
    for (n = 0; n < 6; n++) {                   /* a short line for the lamps */
        kb(45 + (n * 5) % 12, 1);
        if (n & 1)
            kb(57, 1);
        pitch(HZ(110.0 * (1 + n % 3)));
        wh_te_note_on(VOICE(n % 6), 0, 0);
        crossings(n % 6, knobs, 20);
        if (n & 1)
            kb(57, 0);
        if (n < 5)
            kb(45 + (n * 5) % 12, 0);
    }
    ac_page = 0;
    turn(1, 1024);
    hook_frame((uint16_t *)FB);
    ok = ac_note_shown >= 0;
    ac_page = 1;
    turn(0, 1024);
    hook_frame((uint16_t *)(FB + 0x40000));
    ac_page = 0;
    CHECK(12, ac_bank == 2 && ok);

    /* 13: the sound's name: shown when a sound is picked, for 1 s, then gone */
    {
        int on_pick, on_late, off;

        *TE_SHIFT_HELD = 1;
        turn(0, 1024);
        *TE_SHIFT_HELD = 0;
        hook_frame((uint16_t *)FB);
        on_pick = ac_title_on;
        for (n = 0; n < 680; n++)
            ac_tick();
        hook_frame((uint16_t *)FB);
        on_late = ac_title_on;
        for (n = 0; n < 10; n++)
            ac_tick();
        hook_frame((uint16_t *)FB);
        off = !ac_title_on;
        CHECK(13, on_pick && on_late && off);
    }
    kb(45 + 25 % 12, 0);
    return fails;
}
