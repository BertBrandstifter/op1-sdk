/* PALETTE bench (build/apps/palettebench): engine host + PALETTE glue against stub TE functions and
 * TE's per-sample pitch buffer. Returns failed-check bits (0 = pass). */
#include <op1/te246.h>

extern void wh_te_render(void *self, int16_t *out, int16_t *knobs);    /* the engine host's */
extern void wh_te_note_on(void *self, int a1, int a2);
extern void compose_init(void);
extern void engine_name(te_string *out, void *db, const char *sql);    /* composer's chain */
extern void hook_key(int key, int down);
extern void hook_knob(int knob, int16_t *diff);
extern void hook_frame(uint16_t *buf);
extern char FB[];
extern volatile uint8_t pl_page;
extern volatile int8_t pl_bank;
extern volatile uint8_t pl_title_on;
extern volatile int8_t pl_note_shown;
extern const int pl_npatches;
extern void pl_tick(void);
extern int pl_param(const volatile int16_t *k, int id);
extern void pl_param_set(volatile int16_t *k, int id, int v);
extern void pl_bank_knobs(int e, int16_t *k);

#define INC         ((volatile uint32_t *)0xff801950)
#define VOICE(i)    ((void *)(0xffb00000u + (i) * 0x24))

static int drwave_renders, drwave_notes, saves, fxsaves, keys_seen, knobs_seen;
static int16_t out[128];
volatile uint32_t cycles_one, cycles_four;      /* read by run_palettetest.py */
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

static int loud(void) { return peak() > 1000; }

static int level[6];

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

enum { P_LEVEL = 0, P_WAVE = 4, P_PITCH = 8, P_CUT = 12, P_TIME = 16 };
enum { E_ORGAN = 5, E_VOX = 8, E_SYNBASS = 21 };                /* bank entries (pl_bank.inc order) */

/* the 'palette' preset (feature.json content_sql) = bank entry 1 */
static const int16_t preset[8] = { 13032, 11508, 7392, 224, 28783, 28795, 28783, 8303 };
static const int16_t hi[8] = { 24575, 16383, 16383, 32767, 32767, 32767, 32767, 32767 };

static int tagged(volatile int16_t *k)
{
    return (k[4] >> 12) == 7 && (k[5] >> 12) == 7 && (k[6] >> 12) == 7 && (k[7] >> 12) == 2;
}

static void load(int16_t *knobs, int e)
{
    int i;

    pl_bank_knobs(e, knobs);
    for (i = 0; i < 8; i++)
        K(i) = knobs[i];
}

int main(void)
{
    static const int16_t stock[8] = { 20924, 4313, 8063, 0, 32767, 0, 0, 0 };
    int16_t knobs[8];
    int i, n, ok;
    uint32_t t0;

    __asm__ volatile("R0 = SYSCFG; BITSET (R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");  /* CCEN */
    compose_init();
    *TE_EDIT_MODE = 0;
    *TE_SYNTH_SUBSCREEN = 0;
    *TE_SYNTH_SLOT = 2;
    TE_SYNTH_TYPE[2] = TE_INSTR_DRWAVE;

    /* 0: plain drwave stays TE's (also 7 7 7 0: tag 7's slots with slot 8 untagged); the preset =
     * bank entry 1, inside drwave's ranges, extended tag 9 */
    for (i = 0; i < 8; i++)
        knobs[i] = stock[i];
    wh_te_render(VOICE(0), out, knobs);
    CHECK(0, drwave_renders == 1 && out[0] == 1234);
    knobs[5] = knobs[6] = 0x7000;
    knobs[7] = 0x0123;
    wh_te_render(VOICE(0), out, knobs);
    CHECK(0, drwave_renders == 2);
    pl_bank_knobs(0, knobs);
    for (ok = 1, i = 0; i < 8; i++)
        ok &= knobs[i] == preset[i] && knobs[i] >= 0 && knobs[i] <= hi[i];
    CHECK(0, ok);

    /* 1: tag 9 = PALETTE: the organ sounds, drwave is not called */
    load(knobs, E_ORGAN);
    pitch(HZ(220.0));
    wh_te_note_on(VOICE(1), 0, 0);
    drwave_renders = 0;
    for (ok = 0, i = 0; i < 40; i++) {
        wh_te_render(VOICE(1), out, knobs);
        ok |= loud();
    }
    CHECK(1, drwave_renders == 0 && ok && loud());

    /* 2: poly: held keys A (voice 3) and B (voice 4) sound together; B's key up releases B (T4 18
     * ms), A sounds on */
    hook_key(39, 1);
    pitch(HZ(220.0));
    wh_te_note_on(VOICE(3), 0, 0);
    block(knobs);
    hook_key(40, 1);
    pitch(HZ(247.0));
    wh_te_note_on(VOICE(4), 0, 0);
    for (n = 0; n < 10; n++)
        block(knobs);
    ok = level[3] && level[4];
    hook_key(40, 0);
    for (n = 0; n < 40; n++)
        block(knobs);
    ok &= level[3] && !level[4];
    hook_key(39, 0);
    for (n = 0; n < 40; n++)
        block(knobs);
    ok &= !level[3];
    CHECK(2, ok);

    /* 3: knob push cycles 6 pages */
    i = keys_seen;
    push(59, 0);
    CHECK(3, pl_page == 1 && keys_seen == i);
    for (n = 0; n < 4; n++)
        push(60, 0);
    CHECK(3, pl_page == 5);
    push(59, 0);
    CHECK(3, pl_page == 0);

    /* 4: page 1 LEVEL clamps at 31 (WAVE / CUTOFF kept); page 2 WAVE one step per detent, the tag
     * kept; page 3 PITCH to 31; page 5 TIME to 7 */
    {
        int w = pl_param(&K(0), P_WAVE), c = pl_param(&K(0), P_CUT), pa = K(4) & 0xf00;

        for (n = 0; n < 40; n++)
            turn(0, 1024);
        CHECK(4, pl_param(&K(0), P_LEVEL) == 31 && knobs_seen == 0
                 && pl_param(&K(0), P_WAVE) == w && pl_param(&K(0), P_CUT) == c && (K(4) & 0xf00) == pa);
    }
    push(59, 0);
    i = pl_param(&K(0), P_WAVE + 1);
    turn(1, 1024);
    CHECK(4, pl_param(&K(0), P_WAVE + 1) == i + 1 && tagged(&K(0)));
    push(59, 0);
    for (n = 0; n < 40; n++)
        turn(2, 1024);
    CHECK(4, pl_param(&K(0), P_PITCH + 2) == 31 && tagged(&K(0)));
    push(59, 0);
    push(59, 0);
    for (n = 0; n < 10; n++)
        turn(3, 1024);
    CHECK(4, pl_param(&K(0), P_TIME + 3) == 7 && tagged(&K(0)));
    push(59, 0);
    push(59, 0);
    CHECK(4, pl_page == 0);

    /* 5: shift + blue = bank, every entry in drwave's ranges with tag 9, then P-- (the sound as it was) */
    {
        int16_t before[8];
        int e, k;

        for (k = 0; k < 8; k++)
            before[k] = K(k);
        *TE_SHIFT_HELD = 1;
        turn(0, 1024);
        ok = pl_bank == 0;
        for (e = 0; e < pl_npatches; e++) {
            for (k = 0; k < 8; k++)
                ok &= K(k) >= 0 && K(k) <= hi[k];
            ok &= tagged(&K(0));
            turn(0, 1024);
        }
        ok &= pl_bank == -1;
        for (k = 0; k < 8; k++)
            ok &= K(k) == before[k];
        turn(0, -1024);
        ok &= pl_bank == pl_npatches - 1;
        turn(0, 1024);
        *TE_SHIFT_HELD = 0;
        CHECK(5, ok && knobs_seen == 0 && pl_npatches >= 20);
    }

    /* 6: page 6 = FX knobs */
    {
        volatile int16_t *rt = (volatile int16_t *)0x01de5c50 + TE_FX_SPRING * 24;
        volatile int16_t *fx = (volatile int16_t *)0xff801444 + 2 * 8;

        rt[0] = 1344; rt[1] = 16448;
        fx[0] = 8000;
        ((volatile int32_t *)0xff801734)[2] = TE_FX_SPRING;
        ((volatile uint8_t *)0x01aaac20)[2] = 1;
        pl_page = 5;
        for (n = 0; n < 200; n++)
            turn(0, 32767);
        CHECK(6, fx[0] == 16448 && fxsaves == 200);
        pl_page = 0;
    }

    /* 7: engine list: "palette" -> "drwave", "trio" too (the chain), "palettes" untouched */
    {
        static te_string a, b, c;

        name(&a, "palette"); name(&b, "trio"); name(&c, "palettes");
        engine_name(&a, 0, 0);
        engine_name(&b, 0, 0);
        engine_name(&c, 0, 0);
        CHECK(7, a.size == 6 && a.bx.sso[0] == 'd' && b.size == 6 && c.size == 8 && c.bx.sso[0] == 'p');
    }

    /* 8: cost per voice block: the organ (one tone + two), VOX CHOIR (four tones, filters) */
    load(knobs, E_ORGAN);
    for (i = 1; i < 4; i++)
        pl_param_set(knobs, P_LEVEL + i, 0);
    pitch(HZ(220.0));
    wh_te_note_on(VOICE(5), 0, 0);
    for (n = 0; n < 20; n++)
        wh_te_render(VOICE(5), out, knobs);
    t0 = cycles();
    wh_te_render(VOICE(5), out, knobs);
    cycles_one = cycles() - t0;
    load(knobs, E_VOX);
    pitch(HZ(330.0));
    wh_te_note_on(VOICE(0), 0, 0);
    for (n = 0; n < 400; n++)
        wh_te_render(VOICE(0), out, knobs);
    t0 = cycles();
    wh_te_render(VOICE(0), out, knobs);
    cycles_four = cycles() - t0;
    CHECK(8, cycles_four < 48000 && loud());    /* 3 sounding (12-tone budget): 3 x 48k = MONO's 4 x 37k */

    /* 9: TE's LFO on slot 1 changes the low bits TE hands over (tone A's WAVE / CUTOFF): the fields
     * come from the stored knobs (the same sound) */
    {
        int16_t lfo[8];
        int a, b;

        load(knobs, E_SYNBASS);                 /* no LFO depths (the organ's tremolo starts anywhere) */
        for (i = 0; i < 8; i++)
            lfo[i] = knobs[i];
        lfo[0] = (int16_t)(knobs[0] ^ 0x1ff);
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

    /* 10: the engine screen draws (bank entry 2, page 1; a key held and sounding), and page 2 into
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
    pl_page = 0;
    hook_frame((uint16_t *)FB);
    pl_page = 1;
    hook_frame((uint16_t *)(FB + 0x40000));
    pl_page = 0;
    hook_key(41, 0);
    CHECK(10, pl_bank == 1);

    /* 11: polyphony counts tones (budget 12): six notes of the 3-tone organ: the 4 newest sound, the
     * 2 oldest fade out (one block) and then render silence; 4 tones (VOX CHOIR): 3; 1 tone: all 6 */
    {
        static const int8_t want[3] = { 4, 3, 6 };
        int c;

        for (c = 0; c < 3; c++) {
            load(knobs, c == 1 ? E_VOX : E_ORGAN);
            if (c == 2)
                for (i = 1; i < 4; i++)
                    pl_param_set(knobs, P_LEVEL + i, 0);
            for (i = 0; i < 8; i++)
                K(i) = knobs[i];
            for (i = 0; i < 6; i++) {
                pitch(HZ(220.0 + 30 * i));
                wh_te_note_on(VOICE(i), 0, 0);
            }
            for (n = 0; n < 300; n++) {         /* past VOX CHOIR's attack */
                int v;

                ok = 0;
                for (v = 0; v < 6; v++) {
                    wh_te_render(VOICE(v), out, knobs);
                    ok += loud();
                }
            }
            CHECK(11, ok == want[c]);
        }
        load(knobs, E_ORGAN);
    }

    /* 12: the sounding note: a held key (id 41: A, octave from the voice = A4, MIDI 69) shows A4 long
     * after the start though the voice starts 2 semitones low; none after its key up */
    {
        int shown_held, shown_after;

        hook_key(41, 1);
        pitch(HZ(392.0));
        wh_te_note_on(VOICE(2), 0, 0);
        for (n = 0; n < 300; n++)
            wh_te_render(VOICE(2), out, knobs);
        hook_frame((uint16_t *)FB);
        shown_held = pl_note_shown;
        hook_key(41, 0);
        for (n = 0; n < 10; n++)
            wh_te_render(VOICE(2), out, knobs);
        hook_frame((uint16_t *)FB);
        shown_after = pl_note_shown;
        CHECK(12, shown_held == 69 && shown_after == -1);
    }
    /* 13: the patch's name: shown when a patch is picked, for 1 s (Seq ticks of 64 samples) */
    {
        int on_pick, on_late, off;

        *TE_SHIFT_HELD = 1;
        turn(0, 1024);
        *TE_SHIFT_HELD = 0;
        hook_frame((uint16_t *)FB);
        on_pick = pl_title_on;
        for (n = 0; n < 680; n++)
            pl_tick();
        hook_frame((uint16_t *)FB);
        on_late = pl_title_on;
        for (n = 0; n < 10; n++)
            pl_tick();
        hook_frame((uint16_t *)FB);
        off = !pl_title_on;
        CHECK(13, on_pick && on_late && off);
    }
    return fails;
}
