/* DRUM ROLL bench (build/apps/op1plus): the composed image's seq tick, key, knob and frame hooks and
 * the two drum voice slots against an emulated VoiceCtl (drum note-on picks a voice and calls the
 * voice note_on slot, as TE's 0x01a84ba0 does) and a stub voice render. Returns failed-check bits. */
#include <op1/te246.h>
#include "../dr_core.h"

extern void hook_tick(void);
extern void hook_key(int key, int down);
extern void hook_knob(int knob, int16_t *diff);
extern void hook_frame(uint16_t *buf);
extern void compose_init(void);
extern void dr_te_note_on(void *self, const int16_t *params);
extern void dr_te_render(void *self, int16_t *out, const int16_t *params);
extern dr_pattern dr_pat[DR_PATTERNS];
extern volatile uint8_t dr_run, dr_open, dr_page, dr_sel, dr_cursor, dr_param, dr_cur;
extern char FB[];
extern volatile int rq_state;
extern int dr_te_drum_pick(void *msg);
extern volatile uint8_t dr_roll[8];
#define DRUM_SUB    ((volatile int32_t *)0x01aab9f4)

#define VOICE(i)    ((void *)(0xffb00000u + (i) * 0x14))
#define PARAMS(k)   ((const int16_t *)(0x01aaadb4u + (k) * 16))
#define NLOG        512

volatile int fails;
volatile uint32_t cycles_render, cycles_frame, cycles_tick, cycles_lp, cycles_plain;   /* read by run_drtest.py */
static struct { uint8_t on, note, voice; int8_t extra; uint32_t block, tape; } lg[NLOG];
static int nlog, active, max_active, violations, inputs, voice_ons, next_voice, rejected;
static uint8_t held[128];                       /* TE's sounding list: a held note is not hit again */
static uint32_t block;
static uint8_t voice_key[12];
static int render_mode;                         /* 0: constant 10000, 1: +-10000 */

void emu_on(te_note *n, int extra)
{
    int k = n->note - 53, vi;

    if (*TE_PLAYMODE != 1 || *TE_PLAYMODE_TARGET != 1 || k < 0 || k > 23 || *TE_DRUM_SHIFT_NOTE != -1)
        violations++;                           /* TE's drum note-on would assert / misplay */
    if (held[n->note & 127]) {
        rejected++;                             /* 0x01a84ba0: same note still held -> ignored */
        return;
    }
    held[n->note & 127] = 1;
    vi = next_voice;
    next_voice = (next_voice + 1) % 12;
    voice_key[vi] = (uint8_t)k;
    if (nlog < NLOG)
        lg[nlog++] = (typeof(lg[0])){ 1, (uint8_t)n->note, (uint8_t)vi, (int8_t)extra, block,
                                      (uint32_t)(*TE_TAPE_POS >> 6) };
    active++;
    if (active > max_active)
        max_active = active;
    if (k >= 0 && k <= 23)
        dr_te_note_on(VOICE(vi), PARAMS(k));    /* TE: voice + key parameter block */
}

void emu_off(te_note *n)
{
    if (*TE_PLAYMODE != 1)
        violations++;                           /* a drum note-off into the synth */
    if (!held[n->note & 127])
        return;                                 /* not in the list: TE does nothing */
    held[n->note & 127] = 0;
    if (nlog < NLOG)
        lg[nlog++] = (typeof(lg[0])){ 0, (uint8_t)n->note, 0, 0, block, 0 };
    active--;
}

void emu_voice_on(void *self, const int16_t *p) { (void)self; (void)p; voice_ons++; }

void emu_render(void *self, int16_t *out, const int16_t *p)
{
    int i;

    (void)self; (void)p;
    for (i = 0; i < 128; i++)
        out[i] = render_mode ? (int16_t)(i & 1 ? -10000 : 10000) : 10000;
}

void log_input(int a, int b) { (void)a; (void)b; inputs++; }

static uint32_t cycles(void)
{
    uint32_t c;
    __asm__ volatile("%0 = CYCLES;" : "=d"(c));
    return c;
}

static void tick(int n)
{
    while (n--) {
        hook_tick();
        block++;
        if (*TE_TAPE_STATE == 2)
            *TE_TAPE_POS += 64 << 6;
    }
}

static void push(int key, int shift)
{
    *TE_SHIFT_HELD = (uint8_t)shift;
    hook_key(key, 1);
    hook_key(key, 0);
    *TE_SHIFT_HELD = 0;
}

static void turn(int knob, int n, int shift)
{
    int16_t d = n > 0 ? 1024 : -1024;

    *TE_SHIFT_HELD = (uint8_t)shift;
    for (n = n < 0 ? -n : n; n; n--) {
        int16_t x = d;

        hook_knob(knob, &x);
    }
    *TE_SHIFT_HELD = 0;
}

static int find_on(int from, int note)
{
    int i;

    for (i = from; i < nlog; i++)
        if (lg[i].on && lg[i].note == note)
            return i;
    return -1;
}

#define BLUE 59
#define GREEN 60
#define WHITE 61
#define ORANGE 62

/* 11: "drum roll" in the drum engine list (row 64): TE skips the pick, the sound gets DRUM ROLL
 * (shown on its engine page without the shift + green gesture); a kit pick keeps it, dbox ends it.
 * T2-T4 (drum sub-screens 1-3) stay TE's even while DRUM ROLL is up. */
static void engine_list(void)
{
    int32_t msg[3] = { 0, 3, 64 };
    int ok = 1, n;

    dr_open = 0;
    *TE_EDIT_MODE = 1;
    *TE_DRUM_SLOT = 3;
    ok &= dr_te_drum_pick(msg) == 1 && dr_roll[3];
    n = inputs;
    turn(0, 1, 0);                              /* engine page: DRUM ROLL's knobs */
    ok &= inputs == n;
    *DRUM_SUB = 2;                              /* FX page: TE's (and the SDK effects') */
    turn(0, 1, 0);
    ok &= inputs == n + 1;
    *DRUM_SUB = 0;
    push(GREEN, 1);                             /* close for this sound only */
    turn(0, 1, 0);
    ok &= inputs == n + 2 && !dr_open;
    push(GREEN, 1);
    msg[2] = -5;                                /* a kit: still DRUM ROLL */
    ok &= dr_te_drum_pick(msg) == 0 && dr_roll[3];
    *TE_DRUM_SLOT = 0;                          /* another sound: not DRUM ROLL */
    turn(0, 1, 0);
    ok &= inputs == n + 3;
    *TE_DRUM_SLOT = 3;
    msg[2] = 1;                                 /* dbox */
    ok &= dr_te_drum_pick(msg) == 0 && !dr_roll[3];
    *TE_DRUM_SLOT = 0;
    dr_open = 1;
    if (!ok)
        fails |= 1 << 11;
}

/* 10: one drum on every 1/16 (FULL: each held one step) and a 3-hit ratchet: TE hears every hit */
static void back_to_back(void)
{
    dr_pattern *p = &dr_pat[2];
    int i, n = 0, r = 0;

    dr_cur = 2;
    p->lane[0].len = 16;
    for (i = 0; i < 16; i++)
        p->lane[0].s[i].on = 1;
    p->lane[1].len = 4;
    p->lane[1].rate = 0;                        /* quarters, ratchet 3 on step 1, LEN FULL */
    p->lane[1].s[0].on = 1;
    p->lane[1].s[0].p[DR_RATCH] = 3;
    dr_run = 0;
    tick(400);
    rejected = 0;
    nlog = 0;
    dr_run = 1;
    tick(1378);                                 /* one bar */
    dr_run = 0;
    tick(400);
    for (i = 0; i < nlog; i++) {
        n += lg[i].on && lg[i].note == 53;
        r += lg[i].on && lg[i].note == 54;
    }
    if (!(rejected == 0 && n == 16 && r == 3 && active == 0))
        fails |= 1 << 10;
    dr_cur = 0;
}

/* a groove on pattern B, drawn into FB + 1 (STEP focus) and FB + 2 (LANE focus) for the docs */
#define FBN(i)      ((uint16_t *)(FB + (i) * 320 * 512))

static void on(dr_lane *l, int s, int vel)
{
    l->s[s].on = 1;
    l->s[s].p[DR_VEL] = (int8_t)vel;
}

static void demo(void)
{
    dr_pattern *p = &dr_pat[1];
    int i;

    dr_cur = 1;
    on(&p->lane[0], 0, 100); on(&p->lane[0], 4, 100); on(&p->lane[0], 8, 100); on(&p->lane[0], 12, 100);
    on(&p->lane[0], 10, 55);
    on(&p->lane[1], 4, 100); on(&p->lane[1], 12, 100); on(&p->lane[1], 15, 40);
    p->lane[1].s[15].p[DR_PROB] = 50;
    p->lane[2].rate = 5;                        /* hats at 1/32 */
    p->lane[2].len = 32;
    for (i = 0; i < 32; i += 2)
        on(&p->lane[2], i, i % 8 == 0 ? 100 : i % 4 == 0 ? 70 : 40);
    p->lane[2].s[30].p[DR_RATCH] = 3;
    p->lane[3].len = 3;                         /* open hat, 3 against 4 */
    on(&p->lane[3], 2, 80);
    p->lane[3].s[2].p[DR_TONE] = 2;
    p->lane[4].rate = 2;                        /* clap on 1/8T */
    p->lane[4].len = 12;
    on(&p->lane[4], 3, 90); on(&p->lane[4], 9, 90);
    p->lane[5].len = 5;                         /* toms, 5 steps */
    on(&p->lane[5], 0, 90); on(&p->lane[5], 3, 70);
    p->lane[5].s[0].p[DR_TONE] = -5;
    p->lane[5].s[3].p[DR_TONE] = 3;
    p->lane[6].len = 7;
    on(&p->lane[6], 2, 60); on(&p->lane[6], 5, 60);
    p->lane[6].s[5].p[DR_NUDGE] = -20;
    on(&p->lane[7], 6, 100);
    p->lane[7].mute = 1;
    dr_open = 1;
    dr_page = 0;
    dr_sel = 2;
    dr_cursor = 6;
    dr_param = DR_VEL;
    dr_run = 1;                                 /* play */
    tick(900);
    hook_frame(FBN(1));
    dr_sel = 0;
    dr_cursor = 0;
    dr_page = 4;                                /* SOUND: LP, cutoff down, resonance up */
    turn(0, -3, 0);
    turn(0, 1, 0);
    turn(1, -10, 0);
    turn(2, 12, 0);
    hook_frame(FBN(2));
    dr_run = 0;
    dr_page = 0;
}

#define CHECK(bit, c) do { if (!(c)) fails |= 1 << (bit); } while (0)

int main(void)
{
    static int16_t out[128];
    int i, n, ok, a;

    __asm__ volatile("R0 = SYSCFG; BITSET (R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");  /* CCEN */
    *TE_BPM = 120.0f;
    *TE_PLAYMODE = 1;
    *TE_PLAYMODE_TARGET = 1;
    *TE_EDIT_MODE = 1;
    *TE_TAPE_STATE = 0;
    *TE_TAPE_POS = 0;
    *TE_DRUM_SHIFT_NOTE = -1;
    compose_init();

    /* 0: shift + green press opens (TE never sees it), any knob press = next page (5, wraps) */
    n = inputs;
    push(BLUE, 0);
    CHECK(0, !dr_open && inputs == n + 2);      /* closed: TE's */
    push(GREEN, 1);
    CHECK(0, dr_open && inputs == n + 2 && dr_page == 0);
    push(GREEN, 0);
    CHECK(0, dr_page == 1);
    push(BLUE, 0);                             /* blue press: cursor step on, any page */
    CHECK(0, dr_pat[0].lane[0].s[0].on);
    push(BLUE, 0);
    CHECK(0, !dr_pat[0].lane[0].s[0].on);
    push(ORANGE, 0);                            /* only green turns the page */
    CHECK(0, dr_page == 1);
    push(GREEN, 0);
    push(GREEN, 0);
    push(GREEN, 0);
    CHECK(0, dr_page == 4);
    push(GREEN, 0);
    CHECK(0, dr_page == 0 && inputs == n + 2);
    push(BLUE, 1);                              /* shift + blue press: no requantize take here */
    CHECK(0, rq_state == 0 && dr_open);
    *TE_EDIT_MODE = 2;                          /* tape screen: TE's knobs again */
    turn(0, 1, 0);
    CHECK(0, dr_cursor == 0 && inputs == n + 3);
    *TE_EDIT_MODE = 1;

    /* 1: page STEP: blue = step, green right = hit. Lane 1 four on the floor, lane 2 = 3 steps x . . */
    for (i = 0; i < 4; i++) {
        turn(1, 1, 0);
        turn(0, 4, 0);
    }
    CHECK(1, dr_pat[0].lane[0].s[0].on && dr_pat[0].lane[0].s[4].on && dr_pat[0].lane[0].s[12].on
             && !dr_pat[0].lane[0].s[1].on && dr_cursor == 0);
    turn(1, 1, 0);
    turn(1, -1, 0);                             /* green left = rest again */
    CHECK(1, !dr_pat[0].lane[0].s[0].on);
    turn(1, 1, 0);
    turn(0, 1, 1);                              /* shift + blue: lane 2 */
    push(GREEN, 0);
    push(GREEN, 0);
    push(GREEN, 0);                              /* page 4 LANE: green = length */
    turn(1, -13, 0);
    push(GREEN, 0);
    push(GREEN, 0);                              /* back to page 1 */
    turn(1, 1, 0);
    CHECK(1, dr_sel == 1 && dr_page == 0 && dr_pat[0].lane[1].len == 3 && dr_pat[0].lane[1].s[0].on);
    /* the edits played previews (stopped): let them out, then count from a clean log */
    tick(400);
    CHECK(1, nlog > 0 && active == 0);
    nlog = 0;
    voice_ons = 0;
    block = 0;

    /* 2: play one bar at 120 BPM: lane 1 at 0 / 22050 / 44100 / 66150, lane 2 every 3 sixteenths */
    push(WHITE, 1);                             /* shift + white press: play */
    CHECK(2, dr_run && dr_page == 0);
    tick(1378);                                 /* clock 0 .. 88192 */
    for (i = 0, n = 0, ok = 1; i < nlog; i++)
        if (lg[i].on && lg[i].note == 53) {
            ok &= lg[i].block == (uint32_t)(n * 22050 / 64) && lg[i].extra == 0;
            n++;
        }
    CHECK(2, ok && n == 4);
    for (i = 0, n = 0, ok = 1; i < nlog; i++)
        if (lg[i].on && lg[i].note == 54) {
            ok &= lg[i].block == (uint32_t)(n * 3 * 5512.5 / 64);
            n++;
        }
    CHECK(2, ok && n == 6);
    CHECK(2, violations == 0 && voice_ons == 10);

    /* 3: TONE +5 on lane 1 step 1 -> extra 5 in the next bar */
    turn(0, -1, 1);                             /* lane 1 */
    push(GREEN, 0);                              /* page 2 NOTE: green = TONE */
    turn(1, 5, 0);
    CHECK(3, dr_param == DR_TONE && dr_pat[0].lane[0].s[0].p[DR_TONE] == 5);
    a = nlog;
    tick(10);                                   /* the bar-2 downbeat (88200) */
    i = find_on(a, 53);
    CHECK(3, i >= 0 && lg[i].extra == 5 && lg[i].block == 1378);

    /* 4: VEL 50 + LEN 1/2 step: gain 1/2, gate 2756 * 2 voice samples, then a 256-sample fade */
    turn(2, -10, 0);                            /* page 2 white = LEN: FULL (12) -> 1/2 (2) */
    for (i = 0; i < 4; i++)
        push(GREEN, 0);                          /* page 1 */
    turn(2, -10, 0);                            /* white = VEL 50 */
    CHECK(4, dr_page == 0 && dr_param == DR_VEL);
    CHECK(4, dr_pat[0].lane[0].s[0].p[DR_VEL] == 50 && dr_pat[0].lane[0].s[0].p[DR_LEN] == 2);
    a = nlog;
    tick(1378 * 2 - 1388 + 2);                  /* to the bar-3 downbeat (176400 / 64 = 2756.25) */
    for (i = a; i < nlog && !(lg[i].on && lg[i].note == 53 && lg[i].block >= 2756); i++)
        ;
    i = i < nlog ? i : -1;
    CHECK(4, i >= 0 && lg[i].block == 2756);
    if (i >= 0) {
        int b, v = lg[i].voice;

        render_mode = 0;
        for (b = 0, ok = 1; b < 48; b++) {
            dr_te_render(VOICE(v), out, PARAMS(0));
            if (b < 43)                         /* 5512 samples = 43.06 blocks */
                ok &= out[0] == 5000 && out[127] == 5000;
            if (b == 44)
                ok &= out[0] < 5000 && out[0] > 0;
            if (b >= 46)
                ok &= out[0] == 0 && out[127] == 0;
        }
        CHECK(4, ok);
    }
    /* the note-off of a 1/2-step note half a step later */
    tick(60);
    for (a = i + 1; a < nlog && !(!lg[a].on && lg[a].note == 53); a++)
        ;
    CHECK(4, a < nlog && lg[a].block >= 2756 + 43 - 1 && lg[a].block <= 2756 + 43 + 1);

    /* 5: page SOUND: lane 1's sample LP at ~25 Hz damps +-10000 at Nyquist; lane 2's stays */
    for (i = 0; i < 4; i++)
        push(GREEN, 0);                          /* page 5 */
    CHECK(5, dr_page == 4);
    turn(0, 1, 0);                              /* blue = FILTER: LP */
    turn(1, -30, 0);                            /* green = CUTOFF 64 -> 4 */
    push(WHITE, 1);                             /* stop; then white press: hear the cursor step */
    tick(200);
    push(GREEN, 0);                              /* page 1 */
    push(WHITE, 0);
    CHECK(5, dr_page == 0);
    a = nlog;
    tick(1);
    i = find_on(a, 53);
    CHECK(5, i >= 0);
    if (i >= 0) {
        int b, v = lg[i].voice, mx = 0;
        uint32_t t0;

        render_mode = 1;
        for (b = 0; b < 8; b++) {
            t0 = cycles();
            dr_te_render(VOICE(v), out, PARAMS(0));
            cycles_render = cycles() - t0;
            for (n = 0; n < 128; n++)
                mx = out[n] > mx ? out[n] : -out[n] > mx ? -out[n] : mx;
        }
        CHECK(5, mx < 300);
    }
    emu_on(&(te_note){ 54, -1, 1, { 0, 0, 0 }, 0 }, 0);       /* lane 2's sample: no filter */
    dr_te_render(VOICE((next_voice + 11) % 12), out, PARAMS(1));
    CHECK(5, out[0] == 10000 && out[1] == -10000);
    emu_off(&(te_note){ 54, -1, 1, { 0, 0, 0 }, 0 });
    tick(200);                                  /* the preview's note-off (TE ignores a held note) */
    /* a key played by hand (no sequencer hit pending): the sample's filter too */
    emu_on(&(te_note){ 53, -1, 1, { 0, 0, 0 }, 0 }, 0);
    dr_te_render(VOICE((next_voice + 11) % 12), out, PARAMS(0));
    CHECK(5, out[100] < 1000 && out[100] > -1000);
    emu_off(&(te_note){ 53, -1, 1, { 0, 0, 0 }, 0 });
    /* LP keeps DC (exercises the fractional multiply): a held 10000 settles at 10000 */
    emu_on(&(te_note){ 53, -1, 1, { 0, 0, 0 }, 0 }, 0);
    render_mode = 0;
    for (i = 0; i < 60; i++)
        dr_te_render(VOICE((next_voice + 11) % 12), out, PARAMS(0));
    CHECK(5, out[127] > 9950 && out[127] < 10050);
    {
        uint32_t t0 = cycles();

        dr_te_render(VOICE((next_voice + 11) % 12), out, PARAMS(0));
        cycles_lp = cycles() - t0;              /* LP, unity, no gate */
        emu_on(&(te_note){ 54, -1, 1, { 0, 0, 0 }, 0 }, 0);
        t0 = cycles();
        dr_te_render(VOICE((next_voice + 11) % 12), out, PARAMS(1));
        cycles_plain = cycles() - t0;           /* no filter: TE's render (stub) + the slot */
        emu_off(&(te_note){ 54, -1, 1, { 0, 0, 0 }, 0 });
    }
    emu_off(&(te_note){ 53, -1, 1, { 0, 0, 0 }, 0 });

    /* 6: synth playmode: no drum notes, note-offs wait; back in drum playmode they go out */
    *TE_PLAYMODE = 0;
    *TE_PLAYMODE_TARGET = 0;
    a = nlog;
    tick(1400);
    CHECK(6, nlog == a && violations == 0);
    *TE_PLAYMODE = 1;
    *TE_PLAYMODE_TARGET = 1;

    /* 7: the tape runs: hits on the tape's grid (bar 3 = 176400). Play / stop from the tape screen
     * (shift + white press), not while the requantize screen owns it, nor in synth playmode */
    *TE_EDIT_MODE = 2;
    rq_state = 2;
    n = inputs;
    push(WHITE, 1);
    CHECK(7, !dr_run);
    rq_state = 0;
    *TE_PLAYMODE = 0;
    push(WHITE, 1);
    CHECK(7, !dr_run && inputs == n + 2);       /* synth playmode: TE's */
    *TE_PLAYMODE = 1;
    push(WHITE, 1);                             /* play again, from the tape screen */
    CHECK(7, dr_run && inputs == n + 2);
    *TE_EDIT_MODE = 1;
    *TE_TAPE_POS = 176400 << 6;
    *TE_TAPE_STATE = 2;
    *TE_DRUM_SHIFT_NOTE = 5;                    /* the user holds shift + a drum key meanwhile */
    a = nlog;
    tick(1378);
    CHECK(7, *TE_DRUM_SHIFT_NOTE == 5 && violations == 0);
    *TE_DRUM_SHIFT_NOTE = -1;
    for (i = a, n = 0, ok = 1; i < nlog; i++)
        if (lg[i].on && lg[i].note == 53) {
            ok &= lg[i].tape <= 176400 + (uint32_t)n * 22050 && lg[i].tape + 64 > 176400 + (uint32_t)n * 22050;
            n++;
        }
    CHECK(7, ok && n == 4);
    /* a jump (rewind to 0) */
    *TE_TAPE_POS = 0;
    a = nlog;
    tick(2);
    i = find_on(a, 53);
    CHECK(7, i >= 0 && lg[i].tape == 0);
    *TE_TAPE_STATE = 0;

    /* 8: stop: every note-on got its note-off; never many at once */
    push(WHITE, 1);
    CHECK(8, !dr_run);
    tick(3000);
    CHECK(8, active == 0 && max_active <= 8 && violations == 0);

    /* 9: screen draws in both focuses, the badge while closed and running; tick cost */
    {
        uint32_t t0;

        push(WHITE, 1);
        tick(400);
        t0 = cycles();
        hook_frame((uint16_t *)FB);
        cycles_frame = cycles() - t0;
        push(GREEN, 0);
        hook_frame((uint16_t *)FB);
        push(GREEN, 1);                         /* close */
        CHECK(9, !dr_open);
        hook_frame((uint16_t *)FB);
        t0 = cycles();
        tick(1);
        cycles_tick = cycles() - t0;
        engine_list();
        back_to_back();
        demo();
        push(WHITE, 1);                         /* stop: the other features' frames only */
        *TE_PLAYMODE = 0;
        t0 = cycles();
        hook_frame((uint16_t *)FB);
        cycles_frame -= cycles() - t0;
        *TE_PLAYMODE = 1;
    }
    return fails;
}
