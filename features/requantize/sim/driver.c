/* Requantize v2 bench (recipe op1plus) against an emulated clip-based TE tape:
 *   - injected keys only act after TE_KEY_WAKE, three audio blocks later (device-like latency)
 *   - a record pass (rec held + play from standstill) records until stop; every pass becomes a
 *     clip {track, start, end, tag}; recording is OVERDUB: existing clips stay (like the OP-1)
 *   - lift (key 10) removes the clip under the playhead on the selected track
 *   - T key selects a track (not while recording), seek applies one block later
 *   - the live synth is heard through the selected track's gain: muted selected track = silent
 * Checks the whole session: take -> panel -> preview -> commit (replace) -> the take's region on
 * its track holds only the quantized clip, with the original end and a 0.25 s pre-roll before the
 * original start (TE fades in every recording: the fade must hit the silent pre-roll, not the
 * first note). Then the piano roll: open, navigate, delete a note, transpose and fine-move another,
 * preview plays exactly the edited notes. Returns failed-check bits. */
#include <op1/te246.h>

extern void hook_kb_on(te_note *n, int extra);
extern void hook_kb_off(te_note *n);
extern void hook_tick(void);
extern void hook_key(int key, int down);
extern void hook_knob(int knob, int16_t *diff);
extern volatile int rq_state, rq_nout, rq_take_focus, rq_nnotes, rq_cur;
extern volatile uint32_t rq_rec_start, rq_rec_end;

struct note_log { int on, note; uint32_t pos; int state, audible; };
static struct note_log played[64];
static int nplayed, inputs, gains, seeks, fails, rec_held, pending_seek = -1, lifts;
static uint16_t queue[32];
static int nqueue, woken;

struct clip { int track, alive; uint32_t start, end; char tag; };
static struct clip clips[16];
static int nclips, recording = -1;              /* index of the clip being recorded */

static uint32_t tape(void) { return (uint32_t)(*TE_TAPE_POS >> 6); }
static int audible(void) { return !TE_TAPE_MUTE[*TE_TAPE_TRACK & 3]; }
void log_on(te_note *n, int extra) { (void)extra; if (nplayed < 64) played[nplayed++] = (struct note_log){ 1, n->note, tape(), *TE_TAPE_STATE, audible() }; }
void log_off(te_note *n) { if (nplayed < 64) played[nplayed++] = (struct note_log){ 0, n->note, tape(), *TE_TAPE_STATE, audible() }; }
void log_input(int a, int b) { (void)a; (void)b; inputs++; }
void emu_gains(void) { gains++; }
void emu_seek(int32_t pos) { seeks++; pending_seek = pos; }
void emu_push(uint16_t ev) { if (nqueue < 32) queue[nqueue++] = ev; }
void emu_wake(void) { if (!woken) woken = 3; }

static char tag_next = 'O';                     /* first pass = the original take */

static void start_pass(void)
{
    clips[nclips] = (struct clip){ *TE_TAPE_TRACK, 1, tape(), tape(), tag_next };
    recording = nclips++;
    tag_next = 'Q';                             /* later passes = quantized */
}

static void emu_apply(void)
{
    int i, c;

    for (i = 0; i < nqueue; i++) {
        int key = queue[i] & 0xff, down = queue[i] >> 8;
        if (key >= TE_KEY_T1 && key <= TE_KEY_T4 && down && *TE_TAPE_STATE != 1)
            *TE_TAPE_TRACK = key - TE_KEY_T1;
        if (key == TE_KEY_REC)
            rec_held = down;
        if (key == TE_KEY_PLAY && down) {
            *TE_TAPE_STATE = rec_held ? 1 : 2;
            if (rec_held)
                start_pass();
        }
        if (key == TE_KEY_STOP && down) {
            *TE_TAPE_STATE = 0;
            recording = -1;
        }
        if (key == 10 && down && *TE_TAPE_STATE == 0)   /* lift: clip under the playhead */
            for (c = 0; c < nclips; c++)
                if (clips[c].alive && clips[c].track == *TE_TAPE_TRACK
                    && clips[c].start <= tape() && tape() <= clips[c].end) {
                    clips[c].alive = 0;
                    lifts++;
                }
    }
    nqueue = 0;
}

static void tick(void)
{
    if (woken && --woken == 0)
        emu_apply();
    if (pending_seek >= 0) {
        *TE_TAPE_POS = pending_seek << 6;
        pending_seek = -1;
    } else if (*TE_TAPE_STATE == 1 || *TE_TAPE_STATE == 2) {
        *TE_TAPE_POS += 64 << 6;
    }
    if (recording >= 0)
        clips[recording].end = tape();
    hook_tick();
}

static void ticks(int n) { while (n--) tick(); }

static void key(int k, int shift)
{
    *TE_SHIFT_HELD = (uint8_t)shift;
    hook_key(k, 1);
    hook_key(k, 0);
    *TE_SHIFT_HELD = 0;
}

static void knob(int k, int d, int shift)
{
    int16_t diff = (int16_t)(d * 1024);

    *TE_SHIFT_HELD = (uint8_t)shift;
    hook_knob(k, &diff);
    *TE_SHIFT_HELD = 0;
}

static void kb(int on, int note)
{
    te_note n = { note, on ? 127 : 64, 1, { 0, 0, 0 }, 0 };
    if (on) hook_kb_on(&n, 0); else hook_kb_off(&n);
}

static int near(uint32_t a, uint32_t b, uint32_t tol) { return a + tol >= b && a <= b + tol; }

#define PREROLL 11025                           /* commit pass starts this long before the take */

#define CHECK(bit, c) do { if (!(c)) fails |= 1 << (bit); } while (0)

int main(void)
{
    int i, ok, ons, live;
    uint32_t o_start, o_end;

    *TE_BPM = 120.0f;
    *TE_EDIT_MODE = 2;
    *TE_TAPE_TRACK = 0;
    *TE_TAPE_POS = 100000 << 6;

    /* 0: shift + blue press starts a take */
    key(59, 1);
    CHECK(0, rq_state == 1 && inputs == 0);

    /* the user records the take on track 1 (a normal pass, emulated: clip 'O') */
    *TE_TAPE_STATE = 1;
    start_pass();
    ticks(30);  kb(1, 60);
    ticks(40);  kb(0, 60);
    ticks(25);  kb(1, 64);
    ticks(30);  kb(0, 64);
    ticks(50);  kb(1, 67);
    ticks(60);  kb(0, 67);
    ticks(65);
    *TE_TAPE_STATE = 0;
    recording = -1;
    o_start = clips[0].start;
    o_end = clips[0].end;
    CHECK(1, nplayed == 6 && near(rq_rec_start, o_start, 64) && near(rq_rec_end, o_end, 64));

    /* 2: shift + blue press -> requantize screen (roll, knobs on the notes), default target = the
     * take's own track */
    key(59, 1);
    CHECK(2, rq_state == 2 && rq_take_focus == 0 && rq_nnotes == 3);

    /* 3/4: preview: tape plays (not recording), original muted, three notes audible (another
     * track selected meanwhile), stops by itself, mute + selection restored, no clip added */
    nplayed = 0;
    key(61, 0);
    CHECK(3, rq_state == 3 && TE_TAPE_MUTE[0] == 1);
    ticks(1500);
    for (i = ons = 0; i < nplayed; i++)
        ons += played[i].on && played[i].state == 2 && played[i].audible;
    ticks(10);                                  /* the restoring T key has TE's key latency */
    CHECK(4, rq_state == 2 && *TE_TAPE_STATE == 0 && ons == 3 && TE_TAPE_MUTE[0] == 0
             && TE_TAPE_MUTE[1] == 0 && *TE_TAPE_TRACK == 0 && nclips == 1);

    /* 5: commit (replace): lift removes the original clip, a record pass makes one new clip on
     * track 1 from the pre-roll to the original end, the tape stops by itself */
    nplayed = 0;
    key(60, 0);                                 /* green: knobs to the take settings */
    i = rq_take_focus;
    key(62, 0);
    ticks(2000);
    CHECK(5, i == 1 && rq_state == 2 && *TE_TAPE_STATE == 0 && lifts == 1 && !clips[0].alive && nclips == 2);
    CHECK(6, clips[1].alive && clips[1].tag == 'Q' && clips[1].track == 0
             && near(clips[1].start, o_start - PREROLL, 128) && near(clips[1].end, o_end, 128));
    /* 7: nothing else on track 1 in the take's region (no overdub merge) */
    for (i = live = 0; i < nclips; i++)
        live += clips[i].alive && clips[i].track == 0 && clips[i].start < o_end && clips[i].end > o_start;
    CHECK(7, live == 1 && TE_TAPE_MUTE[0] == 0);
    /* 8: the three notes were played while recording, note-ons on the 1/16 grid (+-1 block) */
    ok = 1;
    for (i = ons = 0; i < nplayed; i++)
        if (played[i].on) {
            uint32_t k = (played[i].pos + 2756) * 2 / 11025;
            int32_t d = (int32_t)played[i].pos - (int32_t)((uint64_t)k * 11025 / 2);
            ok &= d >= -64 && d <= 64 && played[i].state == 1 && played[i].audible
                  && played[i].pos >= clips[1].start + PREROLL - 128;  /* past TE's fade-in */
            ons++;
        }
    CHECK(8, ok && ons == 3);

    /* 9: commit to another track: no lift, track 2 selected, original muted, new clip there */
    {
        int16_t fwd = 1024;
        hook_knob(2, &fwd);
    }
    key(62, 0);
    ticks(2000);
    CHECK(9, rq_state == 2 && lifts == 1 && *TE_TAPE_TRACK == 1 && TE_TAPE_MUTE[0] == 1
             && nclips == 3 && clips[2].track == 1 && near(clips[2].start, o_start - PREROLL, 128));

    /* 11: green press: knobs back to the take's three notes, cursor on the first */
    key(60, 0);
    CHECK(11, rq_take_focus == 0 && rq_nnotes == 3 && rq_cur == 0 && rq_state == 2);

    /* 12: blue turn -> second note (64), shift + white press deletes it: 2 notes left to play */
    knob(0, 1, 0);
    i = rq_cur;
    key(61, 1);
    CHECK(12, i == 1 && rq_nout == 4 && rq_state == 2);

    /* 13: back to the first note, shift + white turn = +12 (72), green turn = +5 ms (FIXED);
     * preview plays 72 five ms after its grid point and 67 (quantized), not 64 */
    knob(0, -1, 0);
    knob(2, 1, 1);
    knob(1, 1, 0);
    nplayed = 0;
    key(61, 0);
    CHECK(13, rq_state == 3);
    ticks(1500);
    ok = rq_state == 2 && rq_take_focus == 0;
    for (i = ons = 0; i < nplayed; i++)
        if (played[i].on) {
            uint32_t at = played[i].pos - (played[i].note == 72 ? 220 : 0);
            uint32_t k = (at + 2756) * 2 / 11025;
            int32_t d = (int32_t)at - (int32_t)((uint64_t)k * 11025 / 2);

            /* 72 FIXED: exactly; 67 AUTO, now alone in the pass: SNAP/QUEUE keep a note inside
             * the 25 ms late window where it was played */
            ok &= (played[i].note == 72 ? d >= -64 && d <= 64
                   : played[i].note == 67 && d >= -64 && d <= 1102 + 64)
                  && played[i].state == 2 && played[i].audible;
            ons++;
        }
    CHECK(14, ok && ons == 2);

    /* 15: green press: knobs to the take settings; shift + orange press there (every note back to
     * AUTO, see test_rq_roll.c) starts no commit and keeps the deleted note out */
    key(60, 0);
    i = rq_take_focus;
    key(62, 1);
    CHECK(15, i == 1 && rq_take_focus == 1 && rq_state == 2 && rq_nout == 4);

    /* 10: shift + blue press leaves the panel; shift + stop is TE's */
    key(59, 1);
    i = inputs;
    key(TE_KEY_STOP, 1);
    CHECK(10, rq_state == 0 && inputs == i + 2);
    return fails;
}
