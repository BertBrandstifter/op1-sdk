/* CHORD+ bench (chordplus + requantize) in the bf524 simulator: the driver plays TE's KeyHandler and
 * Seq threads through the generated hooks and logs what TE's voice code would play.
 * Returns a bitmask of failed checks (0 = pass). */
#include <op1/te246.h>

extern void hook_kb_on(te_note *n, int extra);
extern void hook_kb_off(te_note *n);
extern void hook_tick(void);
extern void hook_key(int key, int down);
extern void hook_frame(uint16_t *buf);
extern void hook_knob(int knob, int16_t *diff);
extern volatile uint8_t cp_on, cp_pinned, cp_strum;
extern volatile uint8_t cp_set[3];              /* key, scale, voicing */
extern char FB[];

#define BEAT_Q14    (22050 << 14)               /* 120 BPM */
#define MAXLOG          256

struct entry { int kind, note, tick; };
static struct entry log[MAXLOG];
static int nlog, ticks, keys_seen, knobs_seen, fails;

void log_on(te_note *n, int extra) { (void)extra; if (nlog < MAXLOG) log[nlog++] = (struct entry){ 1, n->note, ticks }; }
void log_off(te_note *n) { if (nlog < MAXLOG) log[nlog++] = (struct entry){ 2, n->note, ticks }; }
void log_tick(void) { }
void log_key(int key, int down) { (void)key; (void)down; keys_seen++; }
void log_knob(int knob, int16_t *diff) { (void)knob; (void)diff; knobs_seen++; }

static void tick(void)
{
    int32_t ph = *TE_METRO_TO_BEAT - (64 << 14);

    if (ph < 0)
        ph += BEAT_Q14;
    *TE_METRO_TO_BEAT = ph;
    ticks++;
    hook_tick();
}

static void run(int n) { while (n-- > 0) tick(); }

static void key(int on, int num)
{
    te_note n = { num, 100, 1, { 0, 0, 0 }, 0 };

    if (on)
        hook_kb_on(&n, 0);
    else
        hook_kb_off(&n);
}

static void press(int k, int shift)
{
    *TE_SHIFT_HELD = (uint8_t)shift;
    hook_key(k, 1);
    hook_key(k, 0);
    *TE_SHIFT_HELD = 0;
}

static void turn(int knob, int16_t d) { hook_knob(knob, &d); }

/* the log since index from equals the given (kind, note) pairs */
static int logged(int from, const int *want, int n)
{
    int i;

    if (nlog - from != n)
        return 0;
    for (i = 0; i < n; i++)
        if (log[from + i].kind != want[2 * i] || log[from + i].note != want[2 * i + 1])
            return 0;
    return 1;
}

#define CHECK(bit, c) do { if (!(c)) fails |= 1 << (bit); } while (0)

/* a black key as TE delivers it: on_key with its id, then its keyboard note */
static void black(int on, int id, int num)
{
    hook_key(id, on);
    key(on, num);
}

int main(void)
{
    int m, i;

    *TE_PLAYMODE = 0;
    *TE_KEYS_ENABLED = 1;
    run(4);

    /* 0: off = stock: one key, one note */
    m = nlog;
    key(1, 60);
    key(0, 60);
    { static const int w[] = { 1, 60, 2, 60 }; CHECK(0, logged(m, w, 2)); }

    /* 1: shift + insert turns CHORD+ on, consumed (down and up); plain insert while the panel shows
     * pins it, again unpins (closes) it; with the panel closed plain insert is TE's; shift + insert
     * opens the panel again */
    press(TE_KEY_INSERT, 1);
    CHECK(1, cp_on == 1 && keys_seen == 0);
    press(TE_KEY_INSERT, 0);
    CHECK(1, cp_pinned && keys_seen == 0);
    press(TE_KEY_INSERT, 0);
    CHECK(1, !cp_pinned && keys_seen == 0);
    press(TE_KEY_INSERT, 0);
    CHECK(1, keys_seen == 2);
    press(TE_KEY_INSERT, 1);
    CHECK(1, cp_on == 1 && keys_seen == 2);

    /* 2: white key = its chord (C major: C = C E G), release stops all three */
    m = nlog;
    key(1, 60);
    key(0, 60);
    { static const int w[] = { 1, 60, 1, 64, 1, 67, 2, 60, 2, 64, 2, 67 }; CHECK(2, logged(m, w, 6)); }

    /* 3: held D# (7th) makes no sound; D = D minor 7 */
    m = nlog;
    key(1, 63);
    key(1, 62);
    key(0, 62);
    key(0, 63);
    { static const int w[] = { 1, 62, 1, 65, 1, 69, 1, 72, 2, 62, 2, 65, 2, 69, 2, 72 }; CHECK(3, logged(m, w, 8)); }

    /* 4: C then E held: shared E and G sound once; releasing C stops only C */
    m = nlog;
    key(1, 60);
    key(1, 64);
    key(0, 60);
    key(0, 64);
    { static const int w[] = { 1, 60, 1, 64, 1, 67, 1, 71, 2, 60, 2, 64, 2, 67, 2, 71 }; CHECK(4, logged(m, w, 8)); }

    /* 5: panel (open after turning on): blue = KEY, one semitone per detent -> D; C plays D
     * major; the knobs are not TE's while the panel is open */
    turn(0, 1024);
    turn(0, 1024);
    CHECK(5, cp_set[0] == 2 && knobs_seen == 0);
    m = nlog;
    key(1, 60);
    key(0, 60);
    { static const int w[] = { 1, 62, 1, 66, 1, 69, 2, 62, 2, 66, 2, 69 }; CHECK(5, logged(m, w, 6)); }
    turn(0, -1024);
    turn(0, -1024);

    /* 6: the upper octave's black keys (by place, from on_key): C# = power chord, A# = second
     * inversion, both A# = third, G# = WIDE; silent, as the lower ones */
    m = nlog;
    black(1, 26, 73);
    key(1, 60);
    key(0, 60);
    black(0, 26, 73);
    { static const int w[] = { 1, 60, 1, 67, 1, 72, 2, 60, 2, 67, 2, 72 }; CHECK(6, logged(m, w, 6)); }
    m = nlog;
    black(1, 25, 70);
    key(1, 60);
    key(0, 60);
    { static const int w[] = { 1, 67, 1, 72, 1, 76, 2, 67, 2, 72, 2, 76 }; CHECK(6, logged(m, w, 6)); }
    m = nlog;
    black(1, 15, 58);
    key(1, 60);
    key(0, 60);
    black(0, 15, 58);
    black(0, 25, 70);
    { static const int w[] = { 1, 72, 1, 76, 1, 79, 2, 72, 2, 76, 2, 79 }; CHECK(6, logged(m, w, 6)); }
    m = nlog;
    black(1, 24, 68);
    key(1, 60);
    key(0, 60);
    black(0, 24, 68);
    { static const int w[] = { 1, 48, 1, 67, 1, 76, 2, 48, 2, 67, 2, 76 }; CHECK(6, logged(m, w, 6)); }
    m = nlog;
    key(1, 60);                                 /* nothing held: the plain chord again */
    key(0, 60);
    { static const int w[] = { 1, 60, 1, 64, 1, 67, 2, 60, 2, 64, 2, 67 }; CHECK(6, logged(m, w, 6)); }

    /* 7: strum 20 ms (orange knob, panel reopened): notes ~882 samples apart (13-14 blocks);
     * release before the last one drops it */
    run(3000);
    i = knobs_seen;
    turn(3, 1024);
    CHECK(7, knobs_seen == i);                  /* 4.4 s: the panel is still open (5 s) */
    turn(3, -1024);                             /* (a change: the 5 s start again) */
    run(3500);                                  /* panel closes 5 s after the last change */
    i = knobs_seen;
    turn(3, 1024);
    CHECK(7, knobs_seen == i + 1);              /* panel closed: knobs are TE's again */
    press(TE_KEY_INSERT, 1);                 /* panel again (CHORD+ stays on) */
    CHECK(7, cp_on == 1);
    for (i = 0; i < 4; i++)
        turn(3, 1024);
    CHECK(7, cp_strum == 20);
    m = nlog;
    key(1, 60);
    run(20);
    key(0, 60);
    run(40);
    CHECK(7, nlog - m == 4 && log[m].note == 60 && log[m + 1].note == 64
             && log[m + 1].tick - log[m].tick >= 13 && log[m + 1].tick - log[m].tick <= 14
             && log[m + 2].kind == 2 && log[m + 3].kind == 2);
    for (i = 0; i < 4; i++)
        turn(3, -1024);

    /* 8: shift + insert while the panel is open: off -> stock again */
    press(TE_KEY_INSERT, 1);
    CHECK(8, cp_on == 0);
    m = nlog;
    key(1, 60);
    key(0, 60);
    { static const int w[] = { 1, 60, 2, 60 }; CHECK(8, logged(m, w, 2)); }

    /* 9: drum playmode: keys are TE's even with CHORD+ on */
    press(TE_KEY_INSERT, 1);
    *TE_PLAYMODE = 1;
    m = nlog;
    key(1, 60);
    key(0, 60);
    { static const int w[] = { 1, 60, 2, 60 }; CHECK(9, logged(m, w, 2)); }
    *TE_PLAYMODE = 0;

    /* 11: insert (plain) while the panel shows pins it: still open long after 5 s (the knobs ours),
     * insert again unpins and closes it; plain insert with the panel closed is TE's */
    run(4000);                                  /* the panel from 9 closed by now */
    i = keys_seen;
    press(TE_KEY_INSERT, 0);
    CHECK(11, keys_seen == i + 2 && !cp_pinned);    /* closed: TE's insert */
    press(TE_KEY_INSERT, 1);                    /* panel */
    press(TE_KEY_INSERT, 0);                    /* pin */
    CHECK(11, cp_pinned && cp_on && keys_seen == i + 2);
    run(8000);                                  /* 11.6 s */
    m = knobs_seen;
    turn(0, 1024);
    turn(0, -1024);
    CHECK(11, knobs_seen == m);                 /* still open: the knobs are the panel's */
    hook_frame((uint16_t *)FB);                 /* screenshot: PINNED */
    press(TE_KEY_INSERT, 0);                    /* unpin: closed */
    turn(0, 1024);
    CHECK(11, !cp_pinned && cp_on && knobs_seen == m + 1 && keys_seen == i + 2);
    return fails;
}
