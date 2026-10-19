/* NOTE ROLL bench (build/apps/op1plus): the composed image's seq tick, key, knob, frame and main
 * hooks and the synth finger player's get_note call site, against an emulated TE: VoiceCtl (note
 * log, held notes), TE's synth finger sequencer (its data at 0x01dd98bc, the player's selected
 * pattern and steps, get_note / mark-dirty, a player that releases its notes and plays the next
 * step every 1/16 for each artist holding a pattern) and op1.db. Returns failed-check bits. */
#include <op1/te246.h>
#include "../nr_core.h"
#include "../nr_store.h"
#include "../nr_fs.h"

extern void hook_tick(void);
extern void hook_key(int key, int down);
extern void hook_knob(int knob, int16_t *diff);
extern void hook_frame(uint16_t *buf);
extern void compose_init(void);
extern void hook_main(void);
extern int nr_fs_get_note(void *data, int p, int step, int hand);
extern nr_pattern nr_pat[NR_PATTERNS];
extern nr_fs_clock nr_play[NR_PATTERNS];
extern volatile uint8_t nr_open, nr_page, nr_cur, nr_cursor, nr_voice, nr_ask, nr_ask_yes, dr_open;
extern volatile uint8_t nr_cpitch, nr_rec, nr_undo_n, nr_redo_n;
extern volatile uint16_t nr_owned;
extern volatile int16_t nr_ph_shown;
extern volatile uint32_t nr_px_held;
extern volatile uint8_t nr_px_bank;
extern volatile int rq_state;
extern char FB[];

#define NLOG        1024

/* TE's synth finger sequencer (noteroll.c FS_*) */
#define FS_DATA         ((void *)0x01dd98bc)
#define FS_LOADED       (*(volatile uint8_t *)0x01ddb564)
#define FS_ARTIST(a)    (*(volatile int32_t *)(0x01dd98c4 + 4 * (a)))
#define FS_NOTE(p, s, h) (*(volatile int32_t *)(0x01dd98cc + (p) * 0x100 + (s) * 8 + (h) * 4))
#define FS_LEN(p)       (*(volatile int32_t *)(0x01dda6cc + 4 * (p)))
#define FS_PLAYER_BUILT (*(volatile int32_t *)0x01ddc5a4)
#define FS_SELECTED     (*(volatile int32_t *)0x01ddc5c4)
#define FS_STEP(a)      (*(volatile int32_t *)(0x01ddc5b4 + 4 * (a)))

volatile int fails;
volatile uint32_t cycles_frame, cycles_tick;    /* read by run_nrtest.py */
static struct { uint8_t on, note, te; int16_t vel; uint32_t block; } lg[NLOG];
static int nlog, active, violations, inputs, retrig, in_te, marks, bad_calls;
static uint8_t held[128];
static uint32_t block;

void emu_on(te_note *n, int extra)
{
    if (*TE_PLAYMODE != 0 || *TE_PLAYMODE_TARGET != 0 || n->note < 0 || n->note > 127 || extra
        || n->midi_out != 1)
        violations++;                           /* a synth note in the drum, or not as TE plays it */
    if (n->note < 0 || n->note > 127)
        return;
    if (held[n->note])
        retrig++;
    else
        active++;
    held[n->note] = 1;
    if (nlog < NLOG)
        lg[nlog++] = (typeof(lg[0])){ 1, (uint8_t)n->note, (uint8_t)in_te, (int16_t)n->vel, block };
}

void emu_off(te_note *n)
{
    if (*TE_PLAYMODE != 0)
        violations++;                           /* a synth note-off into the drum */
    if (n->note < 0 || n->note > 127 || !held[n->note])
        return;
    held[n->note] = 0;
    active--;
    if (nlog < NLOG)
        lg[nlog++] = (typeof(lg[0])){ 0, (uint8_t)n->note, (uint8_t)in_te, 0, block };
}

void emu_voice_on(void *self, const int16_t *p) { (void)self; (void)p; }
void emu_render(void *self, int16_t *out, const int16_t *p) { (void)self; (void)out; (void)p; }
void log_input(int a, int b) { (void)a; (void)b; inputs++; }

/* ---- TE's synth finger sequencer, emulated */
int emu_get_note(void *data, int p, int s, int h)
{
    if (data != FS_DATA || p < 0 || p >= 14 || s < 0 || s >= 32 || h < 0 || h > 1) {
        bad_calls++;
        return -1;
    }
    return FS_NOTE(p, s, h);
}

void emu_set_note(void *data, int p, int s, int h, int n) { (void)data; (void)p; (void)s; (void)h; (void)n; bad_calls++; }
void emu_mark_dirty(void *data) { bad_calls += data != FS_DATA; marks++; }

static void te_clear(int p, int len)
{
    int s;

    for (s = 0; s < 32; s++)
        FS_NOTE(p, s, 0) = FS_NOTE(p, s, 1) = -1;
    FS_LEN(p) = len;
}

/* TE's synth finger player (0x019ee0a0): every artist holding a pattern makes a step event every
 * 1/16; at each, TE releases every note it plays (0x019ed564), then plays the step's notes through
 * the hooked get_note call site + 12 x the synth sound's octave, velocity 127 */
static struct { int p; uint32_t t0, k; uint8_t notes[4]; int n; } art[2] = {
    { -1, 0, 0, { 0, 0, 0, 0 }, 0 }, { -1, 0, 0, { 0, 0, 0, 0 }, 0 } };

static void te_release(int a)
{
    int i;

    for (i = 0; i < art[a].n; i++)
        emu_off(&(te_note){ art[a].notes[i], 0, 1, { 0, 0, 0 }, 0 });
    art[a].n = 0;
}

static void te_player_block(void)
{
    uint32_t now = block * 64;
    int a, h;

    in_te = 1;
    for (a = 0; a < 2; a++) {
        int p = FS_ARTIST(a), ev = 0;

        if (p != art[a].p) {
            te_release(a);
            art[a].p = p;
            if (p >= 0) {
                art[a].t0 = now;
                art[a].k = 0;
                FS_SELECTED = p;
                ev = 1;
            }
        } else if (p >= 0 && art[a].t0 + (uint32_t)((art[a].k + 1) * 11025u / 2) < now + 64) {
            art[a].k++;
            ev = 1;
        }
        if (!ev)
            continue;
        te_release(a);
        FS_STEP(a) = (int32_t)(art[a].k % (uint32_t)FS_LEN(p));
        for (h = 0; h < 2; h++) {
            int n = nr_fs_get_note(FS_DATA, p, FS_STEP(a), h);

            if (n >= 0) {
                n += 12 * TE_SYNTH_OCTAVE[*TE_SYNTH_SLOT & 7];
                emu_on(&(te_note){ n, 127, 1, { 0, 0, 0 }, 0 }, 0);
                art[a].notes[art[a].n++] = (uint8_t)n;
            }
        }
    }
    in_te = 0;
}

/* ---- TE's op1.db, emulated (as DRUM ROLL's bench): table sdk_noteroll */
static char db_row[NR_ROWS][NR_ROW_TEXT_MAX];
static uint8_t db_has[NR_ROWS];
static int db_creates, db_writes[NR_ROWS], db_bad, sem_depth, sem_bad, step_n;
static int db_obj;

static const char *after(const char *s, const char *prefix)
{
    while (*prefix)
        if (*s++ != *prefix++)
            return 0;
    return s;
}

static const char *row_id(const char *t, int *id)
{
    int v = 0, n = 0;

    while (t[n] >= '0' && t[n] <= '9' && n < 2)
        v = v * 10 + t[n++] - '0';
    if (!n || v >= NR_ROWS || (n == 2 && t[0] == '0'))
        return 0;
    *id = v;
    return t + n;
}

void *emu_db_op1(void) { return &db_obj; }

void emu_db_exec(void *db, const char *sql)
{
    const char *t;
    int id, n;

    if (db != &db_obj || sem_depth) {
        db_bad++;
        return;
    }
    if (after(sql, "CREATE TABLE IF NOT EXISTS sdk_noteroll (id INTEGER PRIMARY KEY, data TEXT NOT NULL)")) {
        db_creates++;
        return;
    }
    if (after(sql, "CREATE TABLE IF NOT EXISTS sdk_drumroll") || after(sql, "INSERT OR REPLACE INTO sdk_drumroll"))
        return;                                 /* DRUM ROLL's: not this bench's */
    t = after(sql, "INSERT OR REPLACE INTO sdk_noteroll (id, data) VALUES (");
    if (!t || !db_creates || !(t = row_id(t, &id)) || t[0] != ',' || t[1] != '\'') {
        db_bad++;
        return;
    }
    t += 2;
    for (n = 0; t[n] && t[n] != '\'' && t[n] != '%' && n < (int)NR_ROW_TEXT_MAX - 1; n++)
        db_row[id][n] = t[n];
    db_row[id][n] = 0;
    if (t[n] != '\'' || t[n + 1] != ')' || t[n + 2]) {
        db_bad++;
        return;
    }
    db_has[id] = 1;
    db_writes[id]++;
}

static char dr_dummy[4];

void emu_db_prepare(te_db_query *q, const char *sql)
{
    const char *t = after(sql, "SELECT data FROM sdk_noteroll WHERE id=");
    int id;

    q->stmt = 0;
    if (after(sql, "SELECT data FROM sdk_drumroll")) {
        q->stmt = dr_dummy;                     /* DRUM ROLL's rows: none */
        step_n = 1;
        return;
    }
    if (!sem_depth || q->db != &db_obj || !db_creates || !t || !(t = row_id(t, &id)) || *t) {
        db_bad++;
        return;
    }
    q->stmt = db_row[id];
    step_n = 0;
}

int emu_step(void *stmt)
{
    int id = (int)(((char *)stmt - db_row[0]) / NR_ROW_TEXT_MAX);

    if (stmt == dr_dummy)
        return 101;
    return step_n++ == 0 && db_has[id] ? 100 : 101;
}

const char *emu_column(void *stmt, int col) { return col ? 0 : (const char *)stmt; }
int emu_finalize(void *stmt) { (void)stmt; return 0; }
#define SEM ((void *)0x5e3a)
int emu_pend(void *sem, int t) { sem_bad += sem != SEM || t != 0 || sem_depth; sem_depth++; return 0; }
void emu_post(void *sem) { sem_bad += sem != SEM || sem_depth != 1; sem_depth--; }

/* ---- helpers */
static uint32_t cycles(void)
{
    uint32_t c;
    __asm__ volatile("%0 = CYCLES;" : "=d"(c));
    return c;
}

static void tick(int n)
{
    while (n--) {
        te_player_block();
        hook_tick();
        block++;
    }
}

static void mains(int n)
{
    while (n--)
        hook_main();
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

static int count_on(int from, int note, int te)
{
    int i, n = 0;

    for (i = from; i < nlog; i++)
        n += lg[i].on && lg[i].note == note && lg[i].te == te;
    return n;
}

static int find(int from, int on, int note)
{
    int i;

    for (i = from; i < nlog; i++)
        if (lg[i].on == on && lg[i].note == note)
            return i;
    return -1;
}

static void finger_screen(void)
{
    *TE_PLAYMODE = 0;
    *TE_PLAYMODE_TARGET = 0;
    *TE_EDIT_MODE = TE_EDIT_MODE_SEQ;
    *TE_SYNTH_SEQ_TYPE = TE_SEQ_FINGER;
}

#define BLUE 59
#define GREEN 60
#define WHITE 61
#define ORANGE 62
#define CHECK(bit, c) do { if (!(c)) fails |= 1 << (bit); } while (0)
#define FBN(i)      ((uint16_t *)(FB + (i) * 320 * 512))
#define QUIET       375
#define STEPB(k)    ((uint32_t)((k) * 11025u / 2 / 64))     /* block of 1/16 step k at 120 BPM */

int memcmp(const void *a, const void *b, __SIZE_TYPE__ n);
void *memset(void *d, int c, __SIZE_TYPE__ n);

int main(void)
{
    static nr_pattern want;
    nr_settings st;
    int i, n, a, b0, ok;

    __asm__ volatile("R0 = SYSCFG; BITSET (R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");  /* CCEN */
    *TE_BPM = 120.0f;
    *TE_EDIT_MODE = 2;                          /* tape screen */
    *TE_PLAYMODE = 0;
    *TE_PLAYMODE_TARGET = 0;
    *TE_SYNTH_SEQ_TYPE = TE_SEQ_FINGER;
    *TE_DRUM_SEQ_TYPE = TE_SEQ_PATTERN;
    *TE_SYNTH_SLOT = 2;
    TE_SYNTH_OCTAVE[2] = 0;
    *TE_TAPE_STATE = 0;
    for (i = 0; i < 14; i++)
        te_clear(i, 16);
    FS_ARTIST(0) = FS_ARTIST(1) = -1;
    FS_LOADED = 1;
    FS_PLAYER_BUILT = 1;
    FS_SELECTED = 0;
    /* TE pattern 3 (index 2): C4 + E4 on step 1, G4 on 5, A4 on 9 */
    FS_NOTE(2, 0, 0) = 60;
    FS_NOTE(2, 0, 1) = 64;
    FS_NOTE(2, 4, 0) = 67;
    FS_NOTE(2, 8, 1) = 69;
    /* op1.db: pattern 14 NOTE ROLL's (a note on step 3, D5, 2 steps), the settings */
    nr_pattern_init(&want);
    want.len = 8;
    nr_add(&want.s[2], 74, 60, 6);
    nr_store_pattern(13, &want, db_row[13]);
    db_has[13] = 1;
    memset(&st, 0, sizeof(st));
    st.owned[1] = 0x20;
    nr_store_settings(&st, db_row[NR_ROW_SETTINGS]);
    db_has[NR_ROW_SETTINGS] = 1;
    *TE_DB_LOCK = SEM;
    compose_init();

    /* 13: the first main-loop pass loads op1.db (pattern 14 + its ownership), the next one writes
     * pattern 14's summary into TE's copy (opening asks for it; nothing written yet) */
    mains(2);
    ok = db_creates == 1 && !db_bad && !sem_bad && !memcmp(&nr_pat[13], &want, sizeof(want)) && nr_owned == 1 << 13;
    mains(QUIET + 5);
    for (i = 0; i < NR_ROWS; i++)
        ok &= !db_writes[i];
    CHECK(13, ok);

    /* 0: where it opens: shift + blue press on the synth finger screen only (TE and requantize
     * never see it); not on the tape screen (requantize's take), not in drum playmode, not with
     * another synth sequencer; a plain blue press is TE's; shift + blue press closes */
    push(BLUE, 1);
    CHECK(0, !nr_open && rq_state == 1);        /* tape screen: requantize starts a take */
    push(BLUE, 1);                              /* ... and ends it (nothing recorded: back to idle) */
    CHECK(0, rq_state == 0);
    finger_screen();
    n = inputs;
    *TE_SYNTH_SEQ_TYPE = TE_SEQ_PATTERN;
    push(BLUE, 1);
    CHECK(0, !nr_open && inputs == n + 2 && rq_state == 0);
    *TE_SYNTH_SEQ_TYPE = TE_SEQ_FINGER;
    push(BLUE, 0);
    CHECK(0, !nr_open && inputs == n + 4);
    FS_SELECTED = 2;
    push(BLUE, 1);
    CHECK(0, nr_open && !dr_open && rq_state == 0 && inputs == n + 4 && nr_cur == 2 && nr_page == 0);
    mains(1);                                   /* opening: TE's copy of 14 = its summary */
    CHECK(0, FS_NOTE(13, 2, 0) == 74 && FS_NOTE(13, 2, 1) == -1 && FS_LEN(13) == 8 && marks >= 1);
    push(BLUE, 1);
    CHECK(0, !nr_open);
    hook_frame(FBN(3));                         /* TE's finger screen: the note */
    push(BLUE, 1);
    CHECK(0, nr_open);

    /* 1: the import of TE's pattern 3: the notes as stored, one step each, TE's copy untouched */
    {
        const nr_pattern *p = &nr_pat[2];

        CHECK(1, p->len == 16 && nr_voices(&p->s[0]) == 2 && p->s[0].n[0].key == 60 && p->s[0].n[1].key == 64
                 && p->s[4].n[0].key == 67 && p->s[8].n[0].key == 69 && p->s[0].n[0].len == NR_LEN_DEF
                 && !(nr_owned >> 2 & 1) && FS_NOTE(2, 0, 1) == 64);
    }

    /* 2: TE plays its own pattern (artist 0 holds it), its notes released at every step */
    nlog = 0;
    block = 0;
    FS_ARTIST(0) = 2;
    tick(STEPB(16) + 2);
    FS_ARTIST(0) = -1;
    tick(4);
    CHECK(2, count_on(0, 60, 1) == 2 && count_on(0, 67, 1) == 1 && count_on(0, 60, 0) == 0
             && !nr_play[2].on && active == 0 && violations == 0 && bad_calls == 0);

    /* 3: edits make it NOTE ROLL's: step 2 empty -> blue press adds a note (the last pitch, 60);
     * page 3 CHORD on step 1: MAJ on C4 (C E G); TE's copy = the summary (lowest + highest) */
    nr_cursor = 1;
    push(BLUE, 0);
    CHECK(3, (nr_owned >> 2 & 1) && nr_voices(&nr_pat[2].s[1]) == 1 && nr_pat[2].s[1].n[0].key == 60);
    turn(0, -1, 0);                             /* page 1, blue: step 1 */
    push(GREEN, 0);
    push(GREEN, 0);                             /* page 3 CHORD */
    nr_voice = 0;
    turn(2, 1, 0);                              /* white: CHORD single -> MAJ */
    CHECK(3, nr_page == 2 && nr_cursor == 0 && nr_voices(&nr_pat[2].s[0]) == 3 && nr_pat[2].s[0].n[2].key == 67);
    mains(1);
    CHECK(3, FS_NOTE(2, 0, 0) == 60 && FS_NOTE(2, 0, 1) == 67 && FS_NOTE(2, 1, 0) == 60 && FS_NOTE(2, 1, 1) == -1);
    hook_frame(FBN(2));                         /* screenshot: CHORD page */
    tick(200);                                  /* previews out */

    /* 4: played by NOTE ROLL when TE's artist holds it: TE silent, the chord in the block of TE's
     * first step event, + 12 x the synth sound's octave, VEL 100 -> 127; step 2's note a step later */
    TE_SYNTH_OCTAVE[2] = 1;
    nlog = 0;
    b0 = (int)block;
    FS_ARTIST(0) = 2;
    tick(STEPB(16) - 2);
    i = find(0, 1, 72);
    CHECK(4, i >= 0 && lg[i].block == (uint32_t)b0 && !lg[i].te && lg[i].vel == 127
             && find(0, 1, 76) >= 0 && find(0, 1, 79) >= 0);
    i = find(i + 1, 1, 72);                     /* step 2 (60 + 12) */
    CHECK(4, i >= 0 && lg[i].block >= (uint32_t)b0 + STEPB(1) && lg[i].block <= (uint32_t)b0 + STEPB(1) + 1);
    CHECK(4, count_on(0, 72, 1) + count_on(0, 76, 1) + count_on(0, 79, 1) + count_on(0, 81, 1) == 0);
    CHECK(4, nr_play[2].on && violations == 0 && count_on(0, 81, 0) == 1);  /* A4 on step 9 */
    FS_ARTIST(0) = -1;
    tick(STEPB(4));
    CHECK(4, !nr_play[2].on && active == 0);
    TE_SYNTH_OCTAVE[2] = 0;

    /* 5: lengths: the step-9 note 2 steps long (page 2 SHAPE, green), VEL 50 on it: its note-off
     * two steps after its note-on, velocity 64 */
    nr_cursor = 8;
    nr_cpitch = 69;                             /* the cursor on its note */
    push(GREEN, 0);
    push(GREEN, 0);
    push(GREEN, 0);                             /* page 3 -> 4 -> 5 -> 1 */
    turn(2, -10, 0);                            /* page 1 white = VEL 100 -> 50 */
    CHECK(5, nr_page == 0 && nr_pat[2].s[8].n[0].vel == 50);
    push(GREEN, 0);
    turn(1, 2, 0);                              /* page 2 green = LEN 1 -> 2 steps */
    CHECK(5, nr_pat[2].s[8].n[0].len == 6);
    tick(200);
    nlog = 0;
    b0 = (int)block;
    FS_ARTIST(0) = 2;
    tick(STEPB(12));
    FS_ARTIST(0) = -1;
    tick(STEPB(4));
    i = find(0, 1, 69);
    a = find(i + 1, 0, 69);
    CHECK(5, i >= 0 && a >= 0 && lg[i].vel == 64 && lg[a].block - lg[i].block >= STEPB(2) - 1
             && lg[a].block - lg[i].block <= STEPB(2) + 1 && active == 0);

    /* 6: TE's edits on NOTE ROLL's pattern 3 (finger screen, written into TE's copy): step record
     * of 62 on step 4, then TE deletes step 1 (both hands): the whole C E G chord goes; TE's length
     * 16 -> 12: the pattern's */
    FS_NOTE(2, 3, 0) = 62;
    mains(1);
    CHECK(6, nr_voices(&nr_pat[2].s[3]) == 1 && nr_pat[2].s[3].n[0].key == 62);
    FS_NOTE(2, 0, 0) = FS_NOTE(2, 0, 1) = -1;
    mains(1);
    CHECK(6, nr_voices(&nr_pat[2].s[0]) == 0 && nr_voices(&nr_pat[2].s[1]) == 1);
    FS_LEN(2) = 12;
    mains(1);
    CHECK(6, nr_pat[2].len == 12 && (nr_owned >> 2 & 1));
    mains(2);
    CHECK(6, FS_NOTE(2, 3, 0) == 62 && FS_NOTE(2, 0, 0) == -1 && FS_LEN(2) == 12);   /* stays */

    /* 7: page 4 PAT: LENGTH 12 -> 40: TE keeps 32; TRANSPOSE +2 */
    push(GREEN, 0);
    push(GREEN, 0);                             /* page 2 -> 3 -> 4 */
    CHECK(7, nr_page == 3);
    turn(0, 28, 0);
    turn(3, 2, 0);
    mains(2);
    CHECK(7, nr_pat[2].len == 40 && FS_LEN(2) == 32 && nr_pat[2].s[3].n[0].key == 64 && FS_NOTE(2, 3, 0) == 64);

    /* 8: drum playmode: nothing plays, a held note's note-off waits for synth playmode */
    nlog = 0;
    FS_ARTIST(0) = 2;
    tick(STEPB(3) + 3);                         /* step 4 (64) sounding, 1 step long */
    i = find(0, 1, 64);
    *TE_PLAYMODE = 1;
    *TE_PLAYMODE_TARGET = 1;
    FS_ARTIST(0) = -1;
    for (n = 0; n < 200; n++) {                 /* TE's synth player does not run in drum playmode */
        hook_tick();
        block++;
    }
    CHECK(8, i >= 0 && violations == 0 && active == 1);
    finger_screen();
    tick(2);
    CHECK(8, active == 0 && violations == 0);

    /* 9: the view follows TE's selected pattern while its key is held; the playhead shows */
    FS_ARTIST(0) = 6;
    tick(STEPB(3) + 2);
    mains(1);
    hook_frame(FBN(0));
    CHECK(9, nr_cur == 6 && nr_ph_shown == 3);
    FS_ARTIST(0) = -1;
    tick(4);
    FS_SELECTED = 2;
    mains(1);
    nr_cursor = 3;
    nr_voice = 0;
    push(GREEN, 0);
    push(GREEN, 0);                             /* page 1 NOTE */
    hook_frame(FBN(1));                         /* screenshot: NOTE page */

    /* 14: punch-in effects (the PO-20's set for the synth) on the black keys of TE's synth finger
     * screen: plain black = effects 1-10, shift + the lower F# G# A# C# = 11-14, shift + the others
     * stay TE's step record. Pattern 11: C E G on step 1, C5 on step 5. RETRIG from step 1: the
     * chord every step; OCT UP: an octave higher; ARP: C E G a third of a step apart; STACCATO: a
     * quarter step; SOLO BASS: only C of the chord; SOLO MELODY: only C5; BLINDS: C5 four times */
    {
        nr_pattern *pt = &nr_pat[10];
        int c60, c72, j, n0;

        nr_pattern_init(pt);
        nr_add(&pt->s[0], 60, 100, 4);
        nr_chord(&pt->s[0], 1);
        nr_add(&pt->s[4], 72, 100, 4);
        nr_owned |= 1 << 10;
        n = n0 = inputs;
        *TE_SHIFT_HELD = 1;
        hook_key(24, 1);                        /* shift + upper G#: TE's */
        *TE_SHIFT_HELD = 0;
        hook_key(24, 0);
        CHECK(14, inputs == n + 2 && !nr_px_held);
        nlog = 0;
        FS_ARTIST(0) = 10;
        tick(3);
        hook_key(14, 1);                        /* RETRIG */
        a = nlog;
        tick(8 * 5513 / 64);
        hook_key(14, 0);
        c60 = count_on(a, 60, 0);
        c72 = count_on(a, 72, 0);
        CHECK(14, c60 >= 8 && c60 <= 9 && c72 == 0 && inputs == n + 2);
        FS_ARTIST(0) = -1;
        tick(STEPB(4));
        hook_key(23, 1);                        /* OCT UP */
        a = nlog;
        FS_ARTIST(0) = 10;
        tick(STEPB(1));
        CHECK(14, find(a, 1, 72) >= 0 && find(a, 1, 76) >= 0 && find(a, 1, 79) >= 0 && count_on(a, 60, 0) == 0);
        hook_key(23, 0);
        FS_ARTIST(0) = -1;
        tick(STEPB(4));
        hook_key(22, 1);                        /* RISING ARP */
        a = nlog;
        b0 = (int)block;
        FS_ARTIST(0) = 10;
        tick(STEPB(1) + 2);
        i = find(a, 1, 60);
        j = find(a, 1, 64);
        n = find(a, 1, 67);
        CHECK(14, i >= 0 && j >= 0 && n >= 0 && lg[i].block == (uint32_t)b0
                  && lg[j].block >= (uint32_t)b0 + 28 && lg[j].block <= (uint32_t)b0 + 30
                  && lg[n].block >= (uint32_t)b0 + 57 && lg[n].block <= (uint32_t)b0 + 59);
        hook_key(22, 0);
        FS_ARTIST(0) = -1;
        tick(STEPB(4));
        hook_key(25, 1);                        /* STACCATO */
        a = nlog;
        FS_ARTIST(0) = 10;
        tick(STEPB(1));
        i = find(a, 1, 60);
        j = find(a, 0, 60);
        CHECK(14, i >= 0 && j >= 0 && lg[j].block - lg[i].block >= STEPB(1) / 4 - 1 && lg[j].block - lg[i].block <= STEPB(1) / 4 + 1);
        hook_key(25, 0);
        FS_ARTIST(0) = -1;
        tick(STEPB(4));
        hook_key(26, 1);                        /* BLINDS (bank A) */
        a = nlog;
        FS_ARTIST(0) = 10;
        tick(STEPB(8));
        CHECK(14, count_on(a, 72, 0) == 4 && count_on(a, 60, 0) == 4);
        hook_key(26, 0);
        FS_ARTIST(0) = -1;
        tick(STEPB(4));
        push(BLUE, 1);                          /* editor closed: green press = the bank */
        push(GREEN, 0);
        push(GREEN, 0);
        CHECK(14, nr_px_bank == 2 && !nr_open);
        hook_key(25, 1);                        /* SOLO BASS (bank C) */
        a = nlog;
        FS_ARTIST(0) = 10;
        tick(STEPB(8));
        CHECK(14, count_on(a, 60, 0) == 1 && count_on(a, 64, 0) == 0 && count_on(a, 67, 0) == 0
                  && count_on(a, 72, 0) == 1);  /* C5: the lowest note of its step */
        hook_key(25, 0);
        FS_ARTIST(0) = -1;
        tick(STEPB(4));
        hook_key(26, 1);                        /* SOLO MELODY */
        a = nlog;
        FS_ARTIST(0) = 10;
        tick(STEPB(8));
        CHECK(14, count_on(a, 60, 0) == 0 && count_on(a, 64, 0) == 0 && count_on(a, 72, 0) == 1
                  && nr_px_held == 1u << 28);
        hook_frame(FBN(4));                     /* screenshot: the punch-in window */
        hook_key(26, 0);
        push(GREEN, 0);                         /* back to bank A */
        FS_ARTIST(0) = -1;
        tick(STEPB(4));
        CHECK(14, active == 0 && violations == 0 && !nr_px_held && inputs == n0 + 2 && nr_px_bank == 0);
        nr_owned &= (uint16_t)~(1 << 10);
        FS_SELECTED = 2;                        /* TE selected 11 when it started: back to 3 */
        push(BLUE, 1);                          /* open again */
        mains(1);
        CHECK(14, nr_cur == 2 && nr_open);
    }

    /* 15: the pitch cursor: shift + blue turn moves it to any row, blue press adds a note there (a
     * second note on a step) and takes it away again. STEP REC (shift + white press): keys play notes
     * into the cursor step (the first replaces, more add), FORWARD / BACK move, a step passed by stays,
     * shift + green turn = an octave up; the keys are not TE's meanwhile */
    {
        nr_step *st;
        int k0;

        nr_page = 0;
        nr_cursor = 3;
        nr_cpitch = 64;
        st = &nr_pat[2].s[3];
        k0 = nr_voices(st);
        turn(0, 3, 1);                          /* shift + blue: 64 -> 67 */
        push(BLUE, 0);
        CHECK(15, nr_cpitch == 67 && nr_voices(st) == k0 + 1 && st->n[nr_voices(st) - 1].key == 67);
        push(BLUE, 0);
        CHECK(15, nr_voices(st) == k0);
        n = inputs;
        push(WHITE, 1);                         /* STEP REC on */
        CHECK(15, nr_rec && nr_open);
        nr_cursor = 0;
        hook_key(39, 1);                        /* F (key 0): 53 at octave 0 */
        hook_key(28, 1);                        /* + C (key 7): 60 */
        hook_key(39, 0);
        hook_key(28, 0);
        CHECK(15, nr_voices(&nr_pat[2].s[0]) == 2 && nr_pat[2].s[0].n[0].key == 53 && nr_pat[2].s[0].n[1].key == 60
                  && inputs == n);
        hook_key(TE_KEY_FORWARD, 1);
        hook_key(TE_KEY_FORWARD, 0);
        hook_key(TE_KEY_FORWARD, 1);            /* step 2 passed by: as it was */
        hook_key(TE_KEY_FORWARD, 0);
        k0 = nr_voices(&nr_pat[2].s[1]);
        turn(1, 1, 1);                          /* shift + green turn: an octave up */
        hook_key(29, 1);                        /* D (key 9) + 12: 74 into step 3 */
        hook_key(29, 0);
        CHECK(15, nr_cursor == 2 && nr_voices(&nr_pat[2].s[1]) == k0 && nr_voices(&nr_pat[2].s[2]) == 1
                  && nr_pat[2].s[2].n[0].key == 74);
        hook_key(TE_KEY_BACK, 1);
        hook_key(TE_KEY_BACK, 0);
        hook_key(TE_KEY_BACK, 1);
        hook_key(TE_KEY_BACK, 0);
        hook_key(40, 1);                        /* back on step 1: G (key 2) + 12 replaces it */
        hook_key(40, 0);
        CHECK(15, nr_cursor == 0 && nr_voices(&nr_pat[2].s[0]) == 1 && nr_pat[2].s[0].n[0].key == 67
                  && (nr_owned >> 2 & 1) && inputs == n);
        hook_frame(FBN(0));
        push(WHITE, 1);                         /* STEP REC off: the keys are TE's again */
        hook_key(40, 1);
        hook_key(40, 0);
        CHECK(15, !nr_rec && inputs == n + 2);
        tick(200);
        CHECK(15, violations == 0);
    }

    /* 16: undo / redo (shift + BACK / FORWARD): presses one step each, knob turns of one kind one
     * step; with the editor closed TE's own step record on the synth finger screen goes back (TE's
     * pattern: its notes written back by the main thread) and comes again */
    {
        static nr_pattern p0;
        int u0 = nr_undo_n, k0;

        mains(1);
        p0 = nr_pat[2];
        nr_page = 0;
        nr_cursor = 5;
        nr_cpitch = 70;
        push(BLUE, 0);                          /* a note on step 6 */
        nr_cpitch = 72;
        push(BLUE, 0);                          /* and another */
        turn(2, -5, 0);                         /* VEL x 5 of the second: one step */
        CHECK(16, nr_undo_n >= 3 && nr_redo_n == 0 && nr_voices(&nr_pat[2].s[5]) == nr_voices(&p0.s[5]) + 2);
        (void)u0;                               /* (the history may be full: the oldest steps go) */
        *TE_SHIFT_HELD = 1;
        hook_key(TE_KEY_BACK, 1);
        hook_key(TE_KEY_BACK, 0);
        hook_key(TE_KEY_BACK, 1);
        hook_key(TE_KEY_BACK, 0);
        *TE_SHIFT_HELD = 0;
        CHECK(16, nr_voices(&nr_pat[2].s[5]) == nr_voices(&p0.s[5]) + 1 && nr_redo_n == 2);
        *TE_SHIFT_HELD = 1;
        hook_key(TE_KEY_BACK, 1);
        hook_key(TE_KEY_BACK, 0);
        *TE_SHIFT_HELD = 0;
        CHECK(16, !memcmp(&nr_pat[2], &p0, sizeof(p0)) && nr_redo_n == 3);
        *TE_SHIFT_HELD = 1;
        hook_key(TE_KEY_FORWARD, 1);
        hook_key(TE_KEY_FORWARD, 0);
        *TE_SHIFT_HELD = 0;
        CHECK(16, nr_voices(&nr_pat[2].s[5]) == nr_voices(&p0.s[5]) + 1 && nr_redo_n == 2);
        *TE_SHIFT_HELD = 1;
        hook_key(TE_KEY_BACK, 1);
        hook_key(TE_KEY_BACK, 0);
        *TE_SHIFT_HELD = 0;
        CHECK(16, !memcmp(&nr_pat[2], &p0, sizeof(p0)));
        /* closed: TE's pattern 9 (index 8), TE step-records 65 on step 4 (seen by the main thread) */
        push(BLUE, 1);
        CHECK(16, !nr_open);
        k0 = nr_undo_n;
        FS_NOTE(8, 3, 0) = 65;
        mains(1);
        CHECK(16, nr_undo_n == k0 + 1);
        n = inputs;
        *TE_SHIFT_HELD = 1;
        hook_key(TE_KEY_BACK, 1);
        hook_key(TE_KEY_BACK, 0);
        *TE_SHIFT_HELD = 0;
        mains(1);                               /* the main thread writes TE's notes back */
        CHECK(16, FS_NOTE(8, 3, 0) == -1 && inputs == n && nr_redo_n == 1);
        mains(2);
        CHECK(16, nr_undo_n == k0 && FS_NOTE(8, 3, 0) == -1);  /* our write back: no step of its own */
        *TE_SHIFT_HELD = 1;
        hook_key(TE_KEY_FORWARD, 1);
        hook_key(TE_KEY_FORWARD, 0);
        *TE_SHIFT_HELD = 0;
        mains(1);
        CHECK(16, FS_NOTE(8, 3, 0) == 65 && nr_undo_n == k0 + 1);
        hook_frame(FBN(0));
        FS_NOTE(8, 3, 0) = -1;
        mains(1);
        FS_SELECTED = 2;
        push(BLUE, 1);                          /* open again on pattern 3 */
        mains(1);
        CHECK(16, nr_open && nr_cur == 2);
    }

    /* 17: bank B (arcade), pattern 11 again (C E G on step 1, C5 on step 5): CHIP ARP UP plays the
     * chord's notes in turn every 1/32 (C, then E half a step later); TRILL: C5 and D5 in turn;
     * POWER UP: C5 rolled at 1/32, a semitone up each time; ECHO: C5 again 3 and 6 steps later */
    {
        nr_pattern *pt = &nr_pat[10];
        int b0, i60, i64, j, prev;

        nr_pattern_init(pt);
        nr_add(&pt->s[0], 60, 100, 4);
        nr_chord(&pt->s[0], 1);
        nr_add(&pt->s[4], 72, 100, 4);
        nr_owned |= 1 << 10;
        push(BLUE, 1);                          /* close: green press = the bank */
        push(GREEN, 0);
        CHECK(17, nr_px_bank == 1);
        hook_key(13, 1);                        /* CHIP ARP UP */
        a = nlog;
        b0 = (int)block;
        FS_ARTIST(0) = 10;
        tick(STEPB(1) - 2);
        i60 = find(a, 1, 60);
        i64 = find(a, 1, 64);
        CHECK(17, i60 >= 0 && i64 >= 0 && lg[i60].block == (uint32_t)b0 && lg[i64].block >= (uint32_t)b0 + 42
                  && lg[i64].block <= (uint32_t)b0 + 44 && find(a, 1, 67) < 0);  /* 1/32 = 43 blocks */
        hook_key(13, 0);
        FS_ARTIST(0) = -1;
        tick(STEPB(4));
        hook_key(23, 1);                        /* TRILL */
        a = nlog;
        FS_ARTIST(0) = 10;
        tick(STEPB(5) + 2);
        CHECK(17, find(a, 1, 72) >= 0 && find(a, 1, 74) >= 0);
        hook_key(23, 0);
        FS_ARTIST(0) = -1;
        tick(STEPB(4));
        FS_ARTIST(0) = 10;
        tick(STEPB(4) + 3);                     /* step 5: C5 */
        hook_key(15, 1);                        /* POWER UP */
        a = nlog;
        tick(STEPB(2));
        hook_key(15, 0);
        for (i = a, j = 0, prev = 0, ok = 1; i < nlog; i++)
            if (lg[i].on && lg[i].note >= 72 && lg[i].note <= 96) {
                ok &= !prev || lg[i].note == prev + 1;
                prev = lg[i].note;
                j++;
            }
        CHECK(17, ok && j >= 3);
        FS_ARTIST(0) = -1;
        tick(STEPB(4));
        hook_key(24, 1);                        /* ECHO */
        a = nlog;
        b0 = (int)block;
        FS_ARTIST(0) = 10;
        tick(STEPB(12));
        hook_key(24, 0);
        CHECK(17, count_on(a, 72, 0) == 3);     /* step 5, + 3, + 6 */
        FS_ARTIST(0) = -1;
        tick(STEPB(8));
        push(GREEN, 0);
        push(GREEN, 0);                         /* back to A */
        CHECK(17, nr_px_bank == 0 && active == 0 && violations == 0);
        nr_owned &= (uint16_t)~(1 << 10);
        FS_SELECTED = 2;
        push(BLUE, 1);
        mains(1);
        CHECK(17, nr_open && nr_cur == 2);
    }

    /* 18: two patterns at once (artist 0: NOTE ROLL's pattern 11, C E G on step 1, C5 on step 5;
     * artist 1: TE's pattern 7, F4 on step 1 only): RETRIG pressed on step 7 acts on both; TE's
     * pattern taken over (TE plays nothing of it meanwhile) loops its last step with a note (1),
     * NOTE ROLL's its last one (5, C5); released: TE plays its own pattern again */
    {
        nr_pattern *pt = &nr_pat[10];
        int c65, c72;

        nr_pattern_init(pt);
        nr_add(&pt->s[0], 60, 100, 4);
        nr_chord(&pt->s[0], 1);
        nr_add(&pt->s[4], 72, 100, 4);
        nr_owned |= 1 << 10;
        te_clear(6, 16);
        FS_NOTE(6, 0, 0) = 65;
        nlog = 0;
        FS_ARTIST(0) = 10;
        FS_ARTIST(1) = 6;
        tick(STEPB(6) + 4);                     /* step 7 */
        CHECK(18, count_on(0, 65, 1) == 1 && count_on(0, 65, 0) == 0);
        hook_key(14, 1);                        /* RETRIG (bank A) */
        a = nlog;
        tick(8 * 5513 / 64);
        hook_key(14, 0);
        c65 = count_on(a, 65, 0);
        c72 = count_on(a, 72, 0);
        CHECK(18, c65 >= 7 && c65 <= 9 && count_on(a, 65, 1) == 0 && c72 >= 7 && c72 <= 9 && count_on(a, 60, 0) == 0);
        a = nlog;
        tick(16 * 5513 / 64);
        CHECK(18, count_on(a, 65, 1) >= 1 && count_on(a, 65, 0) == 0 && !(nr_owned >> 6 & 1));
        FS_ARTIST(0) = FS_ARTIST(1) = -1;
        tick(400);
        CHECK(18, active == 0 && violations == 0);
        te_clear(6, 16);
        nr_owned &= (uint16_t)~(1 << 10);
        FS_SELECTED = 2;
        mains(1);
    }

    /* 10: page 5 CLEAR: asks (NO), YES clears both sides, back to TE */
    for (i = 0; i < 4; i++)
        push(GREEN, 0);
    push(ORANGE, 0);
    ok = nr_page == 4 && nr_ask && !nr_ask_yes;
    turn(1, 1, 0);
    ok &= nr_ask_yes;
    hook_frame((uint16_t *)FB);
    push(WHITE, 0);
    mains(1);
    ok &= !nr_ask && !(nr_owned >> 2 & 1) && FS_NOTE(2, 3, 0) == -1 && FS_NOTE(2, 1, 0) == -1 && !nr_voices(&nr_pat[2].s[3]);
    CHECK(10, ok);

    /* 11: saving: NOTE ROLL's patterns and the settings ~3 s after the last edit, as in RAM */
    mains(QUIET + 2);
    {
        static nr_pattern got;
        nr_settings s;

        ok = db_writes[NR_ROW_SETTINGS] >= 1 && !db_bad && !sem_bad
             && nr_load_settings(db_row[NR_ROW_SETTINGS], &s) == 0 && (s.owned[0] | s.owned[1] << 8) == nr_owned;
        ok &= nr_load_pattern(13, db_row[13], &got) == 0 && !memcmp(&got, &nr_pat[13], sizeof(got));
        for (i = 0; i < 13; i++)
            ok &= !db_writes[i] || (nr_owned >> i & 1) || i == 2;
        CHECK(11, ok);
    }

    /* 12: closing, frame and tick cost; every note-on got its note-off */
    {
        uint32_t t0 = cycles();

        hook_frame((uint16_t *)FB);
        cycles_frame = cycles() - t0;
        push(BLUE, 1);
        t0 = cycles();
        tick(1);
        cycles_tick = cycles() - t0;
        tick(STEPB(20));
    }
    CHECK(12, !nr_open && active == 0 && violations == 0 && bad_calls == 0);
    return fails;
}
