/* Quantizer end-to-end bench (features/quantize) in the bf524 simulator.
 * The driver plays the KeyHandler/MIDI/Seq threads: it calls the generated hooks at the TE call
 * sites exactly as TE would, advances TE's metronome phase like TE's audio code does, and logs
 * every note TE's voice code would have played (stubs at 0x01a85750 / 0x01a85840).
 * Returns a bitmask of failed checks (0 = pass). */
#include <op1/te246.h>

/* generated hook entry points (linker symbols from run_quanttest.py) */
extern void hook_kb_on(te_note *n, int extra);
extern void hook_kb_off(te_note *n);
extern void hook_midi_on(te_note *n, int extra);
extern void hook_tick(void);
extern void hook_key(int key, int down);
extern void hook_frame(uint16_t *buf);
extern void hook_knob(int knob, int16_t *diff);
extern volatile int q_grid;                     /* the quantizer's grid index */
extern volatile int q_queue_mode;
extern volatile int q_anchor_mode;
extern char FB[];                               /* 320 lines x 512 bytes, linker symbol */

#define BEAT_Q14    (22050 << 14)               /* 120 BPM */
#define MAXLOG      32

struct entry { int kind, note, vel, tick; };
static struct entry log[MAXLOG];
static int nlog, ticks, te_ticks, keys_seen, fails, last_extra, last_shift;

void log_on(te_note *n, int extra)
{
    last_extra = extra;
    last_shift = *TE_DRUM_SHIFT_NOTE;
    if (nlog < MAXLOG)
        log[nlog++] = (struct entry){ 1, n->note, n->vel, ticks };
}

void log_off(te_note *n)
{
    if (nlog < MAXLOG)
        log[nlog++] = (struct entry){ 2, n->note, n->vel, ticks };
}

void log_tick(void) { te_ticks++; }
void log_key(int key, int down) { (void)key; (void)down; keys_seen++; }
static int knobs_seen;
void log_knob(int knob, int16_t *diff) { (void)knob; (void)diff; knobs_seen++; }

/* one 64-frame audio block: TE's metronome counts down, then the Seq thread runs */
static void tick(void)
{
    int32_t ph = *TE_METRO_TO_BEAT - (64 << 14);

    if (ph < 0)
        ph += BEAT_Q14;
    *TE_METRO_TO_BEAT = ph;
    ticks++;
    hook_tick();
}

static void run_to(int t)
{
    while (ticks < t)
        tick();
}

static void note(int on, int midi, int num)
{
    te_note n = { num, on ? 127 : 64, (uint8_t)!midi, { 0, 0, 0 }, 0 };

    if (on && midi)
        hook_midi_on(&n, 0);
    else if (on)
        hook_kb_on(&n, 0);
    else
        hook_kb_off(&n);
}

static void combo(void)                         /* shift + insert, down and up */
{
    *TE_SHIFT_HELD = 1;
    hook_key(TE_KEY_INSERT, 1);
    hook_key(TE_KEY_INSERT, 0);
    *TE_SHIFT_HELD = 0;
}

#define CHECK(bit, c) do { if (!(c)) fails |= 1 << (bit); } while (0)
#define IS(i, k, n, t) (nlog > (i) && log[i].kind == (k) && log[i].note == (n) && log[i].tick == (t))

int main(void)
{
    int i;

    q_anchor_mode = 0;                            /* checks 0-22: grid fixed to the metronome */
    q_queue_mode = 0;                             /* checks 0-20: snap mode (queue is the default) */
    *TE_BPM = 120.0f;
    *TE_PLAYMODE = 0;
    *TE_PLAYMODE_TARGET = 0;
    *TE_TAPE_STATE = 0;
    *TE_METRO_BEAT_LEN = BEAT_Q14;
    *TE_METRO_TO_BEAT = 3000 << 14;             /* next beat 3000 samples ahead */

    /* 0: quantizer off -> stock: note plays synchronously, nothing held */
    note(1, 0, 50);
    note(0, 0, 50);
    CHECK(0, nlog == 2 && IS(0, 1, 50, 0) && IS(1, 2, 50, 0));
    run_to(10);
    CHECK(1, te_ticks == 10 && nlog == 2);      /* TE's own tick still runs every block */

    /* 2: shift + insert is consumed, a plain key is not; grid -> 1/16 (5512.5 samples) */
    combo();
    hook_key(5, 1);
    CHECK(2, keys_seen == 1);

    /* grid points (our clock, now = 64 * ticks) at 3000 + k * 5512.5. At tick 10 (now 640),
     * 3152 samples past the previous point -> outside the 25 ms window -> held to 3000. */
    nlog = 0;
    note(1, 0, 60);
    CHECK(3, nlog == 0);                        /* held, not played */
    run_to(46);                                 /* now 2944 */
    CHECK(4, nlog == 0);
    run_to(47);                                 /* now 3008: first block at/after 3000 */
    CHECK(5, IS(0, 1, 60, 47) && log[0].vel == 127);

    /* 6: the note-off moves as far as its note-on did (2360 samples), so the held length is kept:
     *    pressed at 640, released at 3200 -> on at 3000, off at 5560 = block 87. (The old rule
     *    played this off right away because it fell in the late window: a 3 ms note.) */
    run_to(50);
    note(0, 0, 60);
    run_to(51);
    CHECK(6, nlog == 1);

    /* 7: MIDI note-on just late (now 3840, 840 after grid) -> immediately (next block) */
    run_to(60);
    note(1, 1, 62);
    run_to(61);
    CHECK(7, IS(1, 1, 62, 61));

    /* 8: early note held for the next grid point: 3000 + floor(5512.5) = 8512 = block 133 */
    run_to(70);
    note(1, 0, 64);
    run_to(132);
    CHECK(8, nlog == 3 && IS(2, 2, 60, 87));
    run_to(133);
    CHECK(9, IS(3, 1, 64, 133));

    /* 10: press and release in the same block while the on is held: on at 14025 (block 220),
     *     off one block (QZ_MIN_LEN) later at 14089 (block 221). At tick 140 (now 8960) the note
     *     would still be inside the late window of 8512, so press at tick 160 (now 10240). */
    run_to(160);
    note(1, 0, 65);                             /* held to 14025 */
    note(0, 0, 65);
    run_to(400);
    CHECK(10, IS(4, 1, 65, 220) && IS(5, 2, 65, 221));

    /* 11: turning off with a note held flushes it on the next block; afterwards stock again */
    run_to(410);
    note(1, 0, 67);                             /* held to the next point */
    for (i = 0; i < 8; i++)                     /* 1/16 -> ... -> 1/16T -> 1/32T -> off */
        combo();
    run_to(411);
    CHECK(11, IS(6, 1, 67, 411));
    note(0, 0, 67);                             /* idle again: plays synchronously */
    CHECK(12, IS(7, 2, 67, 411));

    /* 13: drums are quantized too: at tick 420 (now 26880) the 1/16 grid point 25050 is 1830
     * behind (outside the window) -> held to 30562 = block 478 */
    combo();                                    /* on again (1/16) */
    *TE_PLAYMODE = 1;
    *TE_PLAYMODE_TARGET = 1;                    /* switch finished (target == playmode) */
    run_to(420);
    note(1, 0, 40);
    run_to(477);
    CHECK(13, nlog == 8);
    run_to(478);
    CHECK(13, nlog == 9 && log[8].note == 40 && log[8].tick == 478);

    /* 27: drum octave + shift state are the ones from the press: octave +1 at press, +3 when it
     * plays -> extra -24; shift note 5 at press, -1 when it plays -> 5 during note-on, -1 after */
    *TE_DRUM_SLOT = 2;
    TE_DRUM_OCTAVE[2] = 1;
    *TE_DRUM_SHIFT_NOTE = 5;
    run_to(480);
    note(1, 0, 41);                             /* 30720: 158 after 30562 (window) -> next block */
    TE_DRUM_OCTAVE[2] = 3;
    *TE_DRUM_SHIFT_NOTE = -1;
    run_to(481);
    CHECK(27, nlog == 10 && log[9].note == 41 && last_extra == -24 && last_shift == 5
              && *TE_DRUM_SHIFT_NOTE == -1);
    TE_DRUM_OCTAVE[2] = 0;
    nlog--;                                     /* keep the log indices of later checks */

    /* 14: 1/2 with the tape stopped: half notes on every other metronome beat. Beats passed at
     * 3000 (#1) and 25050 (#2); at tick 440 (now 28160) the next beat is #3 (odd), so the next
     * half-note point is beat #4 = 3000 + 3 * 22050 = 69150 -> first block 1081. */
    *TE_PLAYMODE = 0;
    *TE_PLAYMODE_TARGET = 0;
    for (i = 0; i < 3; i++)                     /* 1/16 -> 1/8 -> 1/4 -> 1/2 */
        combo();
    run_to(440);
    note(1, 0, 70);
    run_to(1080);
    CHECK(14, nlog == 9);
    run_to(1081);
    CHECK(15, IS(9, 1, 70, 1081));

    /* 16/17: blink dot (UI (313, 7) -> panel line 6, column 7): green right on the beat-aligned
     * grid point just passed (69150, now 69184), gone 60 blocks (3840 samples > 60 ms) later */
    {
        volatile uint16_t *px = (volatile uint16_t *)FB + 6 * 256 + 7;

        hook_frame((uint16_t *)FB);
        CHECK(16, *px == 0x07e0);
        run_to(1141);
        *px = 0x1234;                           /* TE clears the buffer before every frame */
        hook_frame((uint16_t *)FB);
        CHECK(17, *px == 0x1234);
    }

    /* 18-20: tempo screen (edit mode 4) + shift + white knob steps the grid, TE never sees it;
     * other knobs, no shift, or another screen pass through */
    {
        int16_t up = 1024, down = -1024;

        *TE_EDIT_MODE = TE_EDIT_MODE_TEMPO;
        *TE_SHIFT_HELD = 1;
        hook_knob(2, &up);                      /* 1/2 -> 1/32 */
        CHECK(18, q_grid == 5 && knobs_seen == 0);
        hook_knob(2, &down);
        hook_knob(2, &down);                    /* -> 1/4 */
        hook_knob(0, &up);                      /* shift + blue = fine BPM: TE's */
        CHECK(19, q_grid == 3 && knobs_seen == 1);
        *TE_SHIFT_HELD = 0;
        hook_knob(2, &up);                      /* white = tape speed: TE's */
        *TE_SHIFT_HELD = 1;
        *TE_EDIT_MODE = 0;
        hook_knob(2, &up);                      /* other screen: TE's */
        CHECK(20, q_grid == 3 && knobs_seen == 3);

        /* 21: tempo screen + shift + orange: left = SNAP, right = QUEUE (both consumed) */
        *TE_EDIT_MODE = TE_EDIT_MODE_TEMPO;
        hook_knob(3, &down);
        CHECK(21, q_queue_mode == 0 && knobs_seen == 3);
        hook_knob(3, &up);
        CHECK(21, q_queue_mode == 1 && knobs_seen == 3);
        *TE_SHIFT_HELD = 0;
        *TE_EDIT_MODE = 0;
    }

    /* 22: queue mode at 1/4 (22050): two notes 61 ms apart (more than the 40 ms chord window)
     * inside one step play one step apart. Grid at 3000 + k * 22050; at tick 1200 (now 76800)
     * prev point 69150, next 91200. */
    run_to(1200);
    note(1, 0, 72);                             /* 7650 after 69150: next slot 91200 */
    run_to(1242);                               /* + 2688 samples (61 ms) */
    note(1, 0, 76);                             /* slot 91200 taken: 113250 */
    run_to(1800);
    CHECK(22, nlog == 12 && IS(10, 1, 72, 1425) && IS(11, 1, 76, 1770));

    /* 23/24: phrase start. Release both (offs keep their held lengths: blocks 2025 and 2328),
     * wait more than a beat, then a note far off the metronome grid plays on the very next block
     * and anchors the grid; a note 3200 samples later goes to anchor + 1/4 (22050). */
    note(0, 0, 72);
    note(0, 0, 76);
    q_anchor_mode = 1;
    run_to(3000);
    CHECK(23, nlog == 14 && IS(12, 2, 72, 2025) && IS(13, 2, 76, 2328));
    note(1, 0, 80);                             /* now 192000: 192000 - 3000 = 8.5 beats: off grid */
    run_to(3001);
    run_to(3050);
    note(1, 0, 81);                             /* 195200 -> 192000 + 22050 = 214050 = block 3345 */
    run_to(3344);
    CHECK(24, nlog == 15 && IS(14, 1, 80, 3001));
    run_to(3345);
    CHECK(25, IS(15, 1, 81, 3345));

    /* 26: chord in queue mode: three keys within 25 ms (key-scanner stagger) share one slot.
     * Phrase grid anchored at 192000 (1/4 = 22050): at tick 3400 the next point is 236100. */
    note(0, 0, 80);
    note(0, 0, 81);
    run_to(3400);
    note(1, 0, 60);
    run_to(3408);
    note(1, 0, 64);
    run_to(3417);                               /* 17 blocks = 25 ms after the first */
    note(1, 0, 67);
    run_to(3700);
    CHECK(26, nlog == 21 && log[18].note == 60 && log[19].note == 64 && log[20].note == 67
              && log[18].tick == log[19].tick && log[19].tick == log[20].tick);
    return fails;
}
