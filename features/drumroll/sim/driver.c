/* DRUM ROLL bench (build/apps/op1plus): the composed image's seq tick, key, knob, frame and main hooks,
 * the finger sequencer call sites (get_note, set_note) and the two drum voice slots, against an
 * emulated TE: VoiceCtl (a drum note-on picks a voice and calls the voice note_on slot, as TE's
 * 0x01a84ba0 does), a stub voice render, TE's finger sequencer (its drum data at 0x01dda710, the
 * player's selected pattern, get_note / set_note / mark-dirty, and a player that makes a step event
 * every 1/16 for each artist holding a pattern) and op1.db. Returns failed-check bits. */
#include <op1/te246.h>
#include "../dr_core.h"
#include "../dr_store.h"
#include "../dr_fs.h"

extern void hook_tick(void);
extern void hook_key(int key, int down);
extern void hook_knob(int knob, int16_t *diff);
extern void hook_frame(uint16_t *buf);
extern void compose_init(void);
extern void hook_main(void);
extern void dr_te_note_on(void *self, const int16_t *params);
extern void dr_te_render(void *self, int16_t *out, const int16_t *params);
extern int dr_fs_get_note(void *data, int p, int step, int hand);
extern void dr_fs_set_note(void *data, int p, int step, int hand, int n);
extern dr_pattern dr_pat[DR_PATTERNS];
extern dr_fs_clock dr_play[DR_PATTERNS];
extern volatile uint8_t dr_open, dr_page, dr_sel, dr_cursor, dr_param, dr_cur;
extern volatile uint8_t dr_swing, dr_zoom, dr_ask, dr_ask_yes;
extern volatile uint16_t dr_owned;
extern volatile int16_t dr_ph_shown;
extern volatile uint32_t dr_px_held;
extern volatile uint8_t dr_px_bank;
extern volatile uint8_t dr_rec, dr_undo_n, dr_redo_n;
extern char FB[];
extern volatile int rq_state;

#define VOICE(i)    ((void *)(0xffb00000u + (i) * 0x14))
#define PARAMS(k)   ((const int16_t *)(0x01aaadb4u + (k) * 16))
#define NLOG        768

/* TE's finger sequencer (drumroll.c FS_*) */
#define FS_DATA         ((void *)0x01dda710)
#define FS_LOADED       (*(volatile uint8_t *)0x01ddb564)
#define FS_ARTIST(a)    (*(volatile int32_t *)(0x01dda718 + 4 * (a)))
#define FS_NOTE(p, s, h) (*(volatile int32_t *)(0x01dda720 + (p) * 0x100 + (s) * 8 + (h) * 4))
#define FS_LEN(p)       (*(volatile int32_t *)(0x01ddb520 + 4 * (p)))
#define FS_PLAYER_BUILT (*(volatile int32_t *)0x01ddc5a4)
#define FS_SELECTED     (*(volatile int32_t *)0x01ddc5c4)
#define FS_STEP(a)      (*(volatile int32_t *)(0x01ddc5b4 + 4 * (a)))

volatile int fails;
volatile uint32_t cycles_render, cycles_frame, cycles_tick, cycles_lp, cycles_plain;   /* read by run_drtest.py */
static struct { uint8_t on, note, voice, te; int8_t extra; uint32_t block; } lg[NLOG];
static int nlog, active, max_active, violations, inputs, voice_ons, next_voice, rejected, in_te;
static uint8_t held[128];                       /* TE's sounding list: a held note is not hit again */
static uint32_t block;
static uint8_t voice_key[12];
static int render_mode;                         /* 0: constant 10000, 1: +-10000 */

void emu_on(te_note *n, int extra)
{
    int k = n->note - 53, vi;

    if (in_te && k > 23)
        k %= 24;                                /* TE's finger notes carry the octave (+24 each) */
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
        lg[nlog++] = (typeof(lg[0])){ 1, (uint8_t)n->note, (uint8_t)vi, (uint8_t)in_te, (int8_t)extra, block };
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
        lg[nlog++] = (typeof(lg[0])){ 0, (uint8_t)n->note, 0, (uint8_t)in_te, 0, block };
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

/* ---- TE's finger sequencer, emulated */
static int marks, bad_calls;

int emu_get_note(void *data, int p, int s, int h)
{
    if (data == (void *)0x01dd98bc)
        return 0x5a5a;                          /* the synth finger data: a marker, never DRUM ROLL's */
    if (data != FS_DATA || p < 0 || p >= 14 || s < 0 || s >= 32 || h < 0 || h > 1) {
        bad_calls++;                            /* TE asserts */
        return -1;
    }
    return FS_NOTE(p, s, h);
}

void emu_set_note(void *data, int p, int s, int h, int n)
{
    if (data != FS_DATA || p < 0 || p >= 14 || s < 0 || s >= 32 || h < 0 || h > 1) {
        bad_calls++;
        return;
    }
    FS_NOTE(p, s, h) = n;
    marks++;
}

void emu_mark_dirty(void *data) { bad_calls += data != FS_DATA; marks++; }

static void te_clear(int p, int len)
{
    int s;

    for (s = 0; s < 32; s++)
        FS_NOTE(p, s, 0) = FS_NOTE(p, s, 1) = -1;
    FS_LEN(p) = len;
}

/* TE's finger screen (0x019ec734): step record of drum key k at step s of pattern p, through the
 * hooked call site, into the first free hand */
static void te_step_record(int p, int s, int k)
{
    if (FS_NOTE(p, s, 0) < 0)
        dr_fs_set_note(FS_DATA, p, s, 0, 53 + k);
    else if (FS_NOTE(p, s, 1) < 0)
        dr_fs_set_note(FS_DATA, p, s, 1, 53 + k);
}

static void te_delete_step(int p, int s)        /* 0x019ec4c8: both hands */
{
    dr_fs_set_note(FS_DATA, p, s, 0, -1);
    dr_fs_set_note(FS_DATA, p, s, 1, -1);
}

/* TE's drum finger player: every artist holding a pattern makes a step event every 1/16 (5512.5
 * samples at 120 BPM) from the block it started in; the step's notes through the hooked get_note
 * call site, played as TE does (velocity 127), each released at the artist's next step */
static struct { int p; uint32_t t0, k; uint8_t notes[2]; } art[2] = { { -1, 0, 0, { 0, 0 } }, { -1, 0, 0, { 0, 0 } } };

static void te_player_block(void)
{
    uint32_t now = block * 64;
    int a, h;

    in_te = 1;
    for (a = 0; a < 2; a++) {
        int p = FS_ARTIST(a), ev = 0;

        if (p != art[a].p) {
            for (h = 0; h < 2; h++)
                if (art[a].notes[h]) {
                    emu_off(&(te_note){ art[a].notes[h], -1, 0, { 0, 0, 0 }, 0 });
                    art[a].notes[h] = 0;
                }
            art[a].p = p;
            if (p >= 0) {
                art[a].t0 = now;
                art[a].k = 0;
                FS_SELECTED = p;                /* TE: the started pattern is the selected one */
                ev = 1;
            }
        } else if (p >= 0) {
            uint32_t due = art[a].t0 + (uint32_t)((art[a].k + 1) * 11025u / 2);

            if (due < now + 64) {
                art[a].k++;
                ev = 1;
            }
        }
        if (!ev)
            continue;
        FS_STEP(a) = (int32_t)(art[a].k % (uint32_t)FS_LEN(p));
        for (h = 0; h < 2; h++) {
            int n;

            if (art[a].notes[h]) {
                emu_off(&(te_note){ art[a].notes[h], -1, 0, { 0, 0, 0 }, 0 });
                art[a].notes[h] = 0;
            }
            n = dr_fs_get_note(FS_DATA, p, (int)(art[a].k % (uint32_t)FS_LEN(p)), h);
            if (n >= 0) {
                emu_on(&(te_note){ n, -1, 0, { 0, 0, 0 }, 0 }, 0);
                art[a].notes[h] = (uint8_t)n;
            }
        }
    }
    in_te = 0;
}

/* ---- TE's op1.db, emulated (stubs at TE_DB_OP1 / _EXEC / _PREPARE, the sqlite calls and the VDK
 * semaphore): table sdk_drumroll as text rows; anything but DRUM ROLL's exact statements counts as bad
 * (on the device: a fatal assert) */
static char db_row[DR_ROWS][DR_ROW_TEXT_MAX];
static uint8_t db_has[DR_ROWS];
static int db_creates, db_writes[DR_ROWS], db_bad, sem_depth, sem_bad, step_n;
static int db_obj;

static const char *after(const char *s, const char *prefix)    /* s past prefix, or 0 */
{
    while (*prefix)
        if (*s++ != *prefix++)
            return 0;
    return s;
}

static const char *row_id(const char *t, int *id)              /* "0".."14": past it, or 0 */
{
    int v = 0, n = 0;

    while (t[n] >= '0' && t[n] <= '9' && n < 2)
        v = v * 10 + t[n++] - '0';
    if (!n || v >= DR_ROWS || (n == 2 && t[0] == '0'))
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
        db_bad++;                               /* TE_DB_EXEC locks itself: never inside our lock */
        return;
    }
    if (after(sql, "CREATE TABLE IF NOT EXISTS sdk_drumroll (id INTEGER PRIMARY KEY, data TEXT NOT NULL)")) {
        db_creates++;
        return;
    }
    if (after(sql, "CREATE TABLE IF NOT EXISTS sdk_noteroll") || after(sql, "INSERT OR REPLACE INTO sdk_noteroll"))
        return;                                 /* NOTE ROLL's: not this bench's */
    t = after(sql, "INSERT OR REPLACE INTO sdk_drumroll (id, data) VALUES (");
    if (!t || !db_creates || !(t = row_id(t, &id)) || t[0] != ',' || t[1] != '\'') {
        db_bad++;
        return;
    }
    t += 2;
    for (n = 0; t[n] && t[n] != '\'' && t[n] != '%' && n < (int)DR_ROW_TEXT_MAX - 1; n++)
        db_row[id][n] = t[n];
    db_row[id][n] = 0;
    if (t[n] != '\'' || t[n + 1] != ')' || t[n + 2]) {
        db_bad++;
        return;
    }
    db_has[id] = 1;
    db_writes[id]++;
}

static char nr_dummy[4];

void emu_db_prepare(te_db_query *q, const char *sql)
{
    const char *t = after(sql, "SELECT data FROM sdk_drumroll WHERE id=");
    int id;

    q->stmt = 0;
    if (after(sql, "SELECT data FROM sdk_noteroll")) {
        q->stmt = nr_dummy;                     /* NOTE ROLL's rows: none */
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
    if (stmt == nr_dummy)
        return 101;
    int id = (int)(((char *)stmt - db_row[0]) / DR_ROW_TEXT_MAX);

    return step_n++ == 0 && db_has[id] ? 100 : 101;
}

const char *emu_column(void *stmt, int col) { return col ? 0 : (const char *)stmt; }
int emu_finalize(void *stmt) { (void)stmt; return 0; }
#define SEM ((void *)0x5e3a)                   /* the handle the bench puts at TE_DB_LOCK */
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
        te_player_block();                      /* TE's player first (TE's seq tick), then the hook */
        hook_tick();
        block++;
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

static int count_on(int from, int note, int te)
{
    int i, n = 0;

    for (i = from; i < nlog; i++)
        n += lg[i].on && lg[i].note == note && lg[i].te == te;
    return n;
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
#define CHECK(bit, c) do { if (!(c)) fails |= 1 << (bit); } while (0)
#define FBN(i)      ((uint16_t *)(FB + (i) * 320 * 512))

int memcmp(const void *a, const void *b, __SIZE_TYPE__ n);     /* lib/rt.c (linked into the driver) */
void *memset(void *d, int c, __SIZE_TYPE__ n);
#define QUIET       375                         /* drumroll.c SAVE_WAIT */
#define STEPB(k)    ((uint32_t)((k) * 11025u / 2 / 64))     /* block of 1/16 step k at 120 BPM */

static void mains(int n)
{
    while (n--)
        hook_main();
}

static void finger_screen(void)
{
    *TE_PLAYMODE = 1;
    *TE_EDIT_MODE = TE_EDIT_MODE_SEQ;
    *TE_DRUM_SEQ_TYPE = TE_SEQ_FINGER;
}

static void stop_all(void)
{
    FS_ARTIST(0) = FS_ARTIST(1) = -1;
    tick(400);
}

/* 13a: the first main-loop pass creates the table and loads what op1.db holds: pattern 14 (row 13,
 * DRUM ROLL's per the owned mask) and the settings (a filter on K21, zoom 2 bars); a damaged row (12)
 * and missing rows keep defaults */
static dr_pattern want;

static void store_load(void)
{
    dr_settings st;
    dr_pattern fresh;
    int ok;

    dr_pattern_init(&want);
    want.lane[4].key = 11;
    want.lane[4].len = 9;
    want.lane[4].s[8].on = 1;
    want.lane[4].s[8].p[DR_TONE] = 7;
    dr_store_pattern(13, &want, db_row[13]);
    db_has[13] = 1;
    memset(&st, 0, sizeof(st));
    st.zoom = 1;
    st.owned[1] = 0x20;                         /* pattern 14 */
    st.filt[20].type = 1;
    st.filt[20].cut = 30;
    st.filt[20].res = 90;
    dr_store_settings(&st, db_row[DR_ROW_SETTINGS]);
    db_has[DR_ROW_SETTINGS] = 1;
    dr_store_pattern(12, &want, db_row[12]);
    db_row[12][7] = '#';                        /* damaged */
    db_has[12] = 1;
    *TE_DB_LOCK = SEM;

    mains(1);
    dr_pattern_init(&fresh);
    ok = db_creates == 1 && !db_bad && !sem_bad && !sem_depth;
    ok &= !memcmp(&dr_pat[13], &want, sizeof(want)) && !memcmp(&dr_pat[12], &fresh, sizeof(fresh))
          && !memcmp(&dr_pat[0], &fresh, sizeof(fresh)) && dr_zoom == 1 && dr_swing == 0 && dr_cur == 0
          && dr_owned == 1 << 13;
    mains(QUIET + 5);                           /* nothing edited: nothing written */
    for (int i = 0; i < DR_ROWS; i++)
        ok &= !db_writes[i];
    dr_zoom = 0;
    dr_owned = 1 << 13;                         /* the rest of the bench: TE's patterns but 14 */
    if (!ok)
        fails |= 1 << 13;
}

/* 13b: saving. The first pass after the quiet time writes the DRUM ROLL patterns that changed and
 * the settings (owned mask), each decoding to what is in RAM; TE's patterns are never written.
 * Closing writes at the next pass. */
static int saved_as_ram(int id)
{
    static dr_pattern got;

    return dr_load_pattern(id, db_row[id], &got) == 0 && !memcmp(&got, &dr_pat[id], sizeof(got));
}

static void store_save(void)
{
    dr_settings st;
    int ok = 1, i, n = 0;

    mains(QUIET + 2);
    for (i = 0; i < DR_PATTERNS; i++) {
        if (db_writes[i] && !(dr_owned >> i & 1))
            ok = 0;                             /* a TE pattern written */
        if (dr_owned >> i & 1 && i != 13)
            ok &= db_writes[i] >= 1 && saved_as_ram(i);   /* (closing the editor saves too) */
        n += db_writes[i];
    }
    ok &= n >= 1 && db_writes[DR_ROW_SETTINGS] >= 1;
    ok &= dr_load_settings(db_row[DR_ROW_SETTINGS], 4, &st) == 0 && st.cur == dr_cur && st.zoom == dr_zoom
          && (st.owned[0] | st.owned[1] << 8) == dr_owned && st.filt[20].cut == 30;
    mains(QUIET + 5);
    n = db_writes[2];
    finger_screen();
    dr_open = 1;
    dr_cur = 2;
    dr_page = 3;
    turn(2, 1, 0);                              /* LANE page, white = rate of the lane */
    mains(QUIET - 10);
    ok &= db_writes[2] == n;                    /* still waiting */
    mains(20);
    ok &= db_writes[2] == n + 1 && saved_as_ram(2);
    turn(2, -1, 0);
    push(GREEN, 1);                             /* close */
    mains(1);
    ok &= db_writes[2] == n + 2 && saved_as_ram(2) && !dr_open;
    ok &= !db_bad && !sem_bad && !sem_depth && db_creates == 1;
    dr_page = 0;
    if (!ok)
        fails |= 1 << 14;
}

/* a groove, drawn into FB + 1 (STEP page, playing) and FB + 2 (SOUND page) for the docs */
static void on(dr_lane *l, int s, int vel)
{
    l->s[s].on = 1;
    l->s[s].p[DR_VEL] = (int8_t)vel;
}

static void demo(void)
{
    dr_pattern *p = &dr_pat[4];
    int i;

    finger_screen();
    dr_pattern_init(p);
    dr_owned |= 1 << 4;
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
    dr_cur = 4;
    dr_page = 0;
    dr_sel = 2;
    dr_cursor = 6;
    dr_param = DR_VEL;
    FS_ARTIST(0) = 4;                           /* hold finger pattern 5 */
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
    turn(0, -1, 0);                             /* filter off again */
    stop_all();
    dr_page = 0;
}

/* 12: page 6 CLEAR: a push asks (NO picked), push = ok; turn right = YES, push clears the pattern
 * on both sides and gives it back to TE; leaving the screen drops the question. Draws the dialog into
 * FB + 3 for the docs. */
static void clear_dialog(void)
{
    dr_pattern before;
    int ok, s, empty = 1;

    finger_screen();
    dr_open = 1;
    dr_cur = 4;                                 /* the demo groove (DRUM ROLL's) */
    dr_page = 0;
    before = dr_pat[4];
    push(GREEN, 0); push(GREEN, 0); push(GREEN, 0); push(GREEN, 0); push(GREEN, 0);
    ok = dr_page == 5 && !dr_ask;
    turn(1, 3, 0);                              /* knobs do nothing on the page */
    ok &= !memcmp(&before, &dr_pat[4], sizeof(before));
    push(ORANGE, 0);
    ok &= dr_ask && !dr_ask_yes;
    turn(2, 1, 0);
    turn(2, -1, 0);                             /* back to NO */
    push(BLUE, 0);                              /* ok on NO: nothing happens */
    ok &= !dr_ask && !memcmp(&before, &dr_pat[4], sizeof(before)) && (dr_owned >> 4 & 1);
    push(WHITE, 0);
    ok &= dr_ask && !dr_ask_yes;
    *TE_EDIT_MODE = TE_EDIT_MODE_DRUM;          /* the screen goes away: no question left */
    hook_frame((uint16_t *)FB);
    ok &= !dr_ask;
    finger_screen();
    push(WHITE, 0);
    turn(3, 1, 0);                              /* YES */
    ok &= dr_ask && dr_ask_yes && dr_page == 5;
    hook_frame((uint16_t *)(FB + 3 * 320 * 512));
    push(ORANGE, 0);
    for (s = 0; s < 32; s++)
        empty &= FS_NOTE(4, s, 0) == -1 && FS_NOTE(4, s, 1) == -1;
    for (s = 0; s < DR_LANES * DR_STEPS; s++)
        empty &= !dr_pat[4].lane[s / DR_STEPS].s[s % DR_STEPS].on;
    ok &= !dr_ask && empty && !(dr_owned >> 4 & 1) && dr_cursor == 0 && dr_page == 5
          && dr_pat[4].lane[0].len == FS_LEN(4);
    dr_page = 0;
    if (!ok)
        fails |= 1 << 12;
}

int main(void)
{
    static int16_t out[128];
    int i, n, ok, a, b0, s;

    __asm__ volatile("R0 = SYSCFG; BITSET (R0, 1); SYSCFG = R0; CSYNC;" ::: "R0");  /* CCEN */
    *TE_BPM = 120.0f;
    *TE_PLAYMODE = 1;
    *TE_PLAYMODE_TARGET = 1;
    *TE_EDIT_MODE = TE_EDIT_MODE_DRUM;
    *TE_DRUM_SEQ_TYPE = TE_SEQ_FINGER;
    *TE_TAPE_STATE = 0;
    *TE_DRUM_SHIFT_NOTE = -1;
    for (i = 0; i < 14; i++)
        te_clear(i, 16);
    FS_ARTIST(0) = FS_ARTIST(1) = -1;
    FS_LOADED = 1;
    FS_PLAYER_BUILT = 1;
    FS_SELECTED = 0;
    /* TE pattern 3 (index 2): kick on 0 4 8 12, snare on 4 12 (hand 1), a hat on 2 an octave up */
    for (s = 0; s < 16; s += 4)
        FS_NOTE(2, s, 0) = 53;
    FS_NOTE(2, 4, 1) = 57;
    FS_NOTE(2, 12, 1) = 57;
    FS_NOTE(2, 2, 0) = 62 + 24;
    compose_init();
    store_load();

    /* 0: where it opens: not on the drum screen, not with another drum sequencer; on the finger
     * screen shift + blue press opens (TE and requantize never see it) on TE's selected pattern,
     * a plain blue press and shift + green press are TE's; closed, TE keeps every knob; shift +
     * blue press closes (no requantize take) */
    n = inputs;
    push(BLUE, 1);
    CHECK(0, !dr_open && inputs == n + 2 && rq_state == 0);     /* drum screen: TE's */
    finger_screen();
    *TE_DRUM_SEQ_TYPE = TE_SEQ_PATTERN;
    push(BLUE, 1);
    CHECK(0, !dr_open && inputs == n + 4);
    *TE_DRUM_SEQ_TYPE = TE_SEQ_FINGER;
    push(BLUE, 0);
    CHECK(0, !dr_open && inputs == n + 6);
    turn(0, 1, 0);
    push(GREEN, 1);
    CHECK(0, !dr_open && inputs == n + 9);
    FS_SELECTED = 2;
    push(BLUE, 1);
    CHECK(0, dr_open && inputs == n + 9 && dr_cur == 2 && dr_page == 0 && dr_owned == 1 << 13 && rq_state == 0);
    push(BLUE, 1);                              /* shift + blue press: closes, no requantize take */
    CHECK(0, rq_state == 0 && !dr_open && inputs == n + 9);
    mains(1);                                   /* closing saves at the next pass (settings only) */
    CHECK(0, db_writes[DR_ROW_SETTINGS] == 1 && !db_writes[2]);
    push(BLUE, 1);
    CHECK(0, dr_open && dr_owned == 1 << 13);
    *TE_EDIT_MODE = TE_EDIT_MODE_DRUM;          /* drum screen: TE's knobs again */
    turn(0, 1, 0);
    CHECK(0, inputs == n + 10);
    finger_screen();

    /* 1: the import of TE's pattern 3: lanes in order of first use (kick, hat, snare), the rest on the
     * lowest unused keys, length 16, the hat's octave as TONE +12; nothing written back to TE's own
     * pattern; opening wrote the summary of DRUM ROLL's pattern 14 (loaded) into TE's copy */
    {
        const dr_pattern *p = &dr_pat[2];

        CHECK(1, p->lane[0].key == 0 && p->lane[1].key == 9 && p->lane[2].key == 4 && p->lane[3].key == 1);
        CHECK(1, p->lane[0].s[0].on && p->lane[0].s[12].on && !p->lane[0].s[1].on && p->lane[2].s[4].on
                 && p->lane[1].s[2].on && p->lane[1].s[2].p[DR_TONE] == 12 && p->lane[0].len == 16);
        CHECK(1, FS_NOTE(2, 4, 1) == 57 && FS_NOTE(2, 2, 0) == 62 + 24);   /* TE's own: untouched */
        CHECK(1, FS_NOTE(13, 8, 0) == 53 + 11 && FS_NOTE(13, 8, 1) == -1);  /* loaded 14: its summary */
    }

    /* 2: TE plays its own pattern (artist 0 holds pattern 3): TE's notes, nothing from DRUM ROLL */
    nlog = 0;
    block = 0;
    FS_ARTIST(0) = 2;
    tick(1378);                                 /* one bar */
    FS_ARTIST(0) = -1;
    tick(10);
        CHECK(2, count_on(0, 53, 1) == 4 && count_on(0, 57, 1) == 2 && count_on(0, 53, 0) == 0 && !dr_play[2].on);
    CHECK(2, violations == 0 && active == 0 && bad_calls == 0);

    /* 3: an edit makes it DRUM ROLL's: blue press = step 1 of the kick lane off then on again (as it
     * was), green right on lane 4 (key 1) step 1 = a third hit on step 1; TE's copy = the summary */
    dr_sel = 0;
    dr_cursor = 0;
    push(BLUE, 0);
    CHECK(3, (dr_owned >> 2 & 1) && !dr_pat[2].lane[0].s[0].on && FS_NOTE(2, 0, 0) == -1 && marks > 0);
    push(BLUE, 0);
    turn(0, 3, 1);                              /* shift + blue: lane 4 */
    turn(1, 1, 0);                              /* green right: hit */
    turn(0, -1, 1);
    turn(0, -1, 1);                             /* lane 2: the hat (key 9) */
    turn(1, 1, 0);                              /* + a hat on step 1 */
    CHECK(3, dr_sel == 1 && dr_pat[2].lane[3].s[0].on && dr_pat[2].lane[1].s[0].on && dr_pat[2].lane[0].s[0].on);
    CHECK(3, FS_NOTE(2, 0, 0) == 53 && FS_NOTE(2, 0, 1) == 62 && FS_NOTE(2, 4, 0) == 53 && FS_NOTE(2, 4, 1) == 57
             && FS_NOTE(2, 2, 0) == 62 && FS_NOTE(2, 1, 0) == -1);  /* the summary has no octave: 53 + key */
    tick(400);                                  /* the edits' previews out */
    CHECK(3, active == 0);

    /* 4: DRUM ROLL plays it when TE's artist holds it: TE plays nothing of it; three hits on step 1
     * in the block of TE's first step event, the kick every 4 steps in the block of TE's step event
     * (within one block), the snare on 5 and 13, the hat on 1 and 3 (TONE +12 -> extra 12) */
    nlog = 0;
    b0 = (int)block;
    FS_ARTIST(0) = 2;
    tick(1378);
    for (i = 0, n = 0, ok = 1; i < nlog; i++)
        if (lg[i].on && lg[i].note == 53 && !lg[i].te) {
            int want = b0 + (int)((uint32_t)n * 22050 / 64);

            ok &= lg[i].block >= (uint32_t)want && lg[i].block <= (uint32_t)want + 1 && lg[i].extra == 0;
            n++;
        }
    CHECK(4, ok && n == 4);
    CHECK(4, count_on(0, 53, 1) + count_on(0, 57, 1) + count_on(0, 62, 1) == 0);           /* TE silent */
    CHECK(4, count_on(0, 57, 0) == 2 && count_on(0, 54, 0) == 1 && count_on(0, 62, 0) == 2);
    i = find_on(0, 54);
    CHECK(4, i >= 0 && lg[i].block == (uint32_t)b0);
    for (i = 0, ok = 0; i < nlog; i++)
        ok += lg[i].on && lg[i].note == 62 && lg[i].extra == 12;
    CHECK(4, ok == 1 && dr_play[2].on && violations == 0 && rejected == 0);

    /* 5: a second artist holds DRUM ROLL pattern 14 (loaded: lane 5 key 11, step 9, TONE +7) at the
     * same time; it starts on its own first step event */
    FS_ARTIST(1) = 13;
    a = nlog;
    b0 = (int)block;
    tick(9 * 5513 / 64 + 2);
    i = find_on(a, 53 + 11);
    CHECK(5, i >= 0 && lg[i].extra == 7 && lg[i].block >= (uint32_t)(b0 + 8 * 5512 / 64)
             && lg[i].block <= (uint32_t)(b0 + 8 * 5513 / 64 + 1) && dr_play[13].on && dr_play[2].on);
    CHECK(5, count_on(a, 53, 0) >= 2);          /* pattern 3 goes on meanwhile */
    CHECK(5, dr_cur == 2);                      /* no key / knob event: the view stays */

    /* 6: released: the pattern stops at once, every note-on got its note-off */
    FS_ARTIST(0) = FS_ARTIST(1) = -1;
    a = nlog;
    tick(2);
    CHECK(6, !dr_play[2].on && !dr_play[13].on);
    tick(3000);
    CHECK(6, count_on(a, 53, 0) == 0 && active == 0 && max_active <= 10 && violations == 0);
    /* ... and after 3 steps without step events (TE's player stopped, the artist still set) */
    FS_ARTIST(0) = 2;
    tick(10);
    art[0].t0 = 0x7fffffff;                     /* TE's player makes no more events */
    tick(3 * 5513 / 64 + 3);
    CHECK(6, !dr_play[2].on);
    FS_ARTIST(0) = -1;
    tick(3000);
    art[0].p = -1;
    CHECK(6, active == 0);

    /* 7: TE's step record into DRUM ROLL's pattern 3: key 20 on step 7 -> an empty lane gets key 20;
     * the kick on step 4 deleted in TE (both hands) -> every 1/16 lane's step 5 off (also the snare) */
    marks = 0;
    te_step_record(2, 6, 20);
    for (i = 0; i < DR_LANES && dr_pat[2].lane[i].key != 20; i++)
        ;
    CHECK(7, i < DR_LANES && dr_pat[2].lane[i].s[6].on && (dr_owned >> 2 & 1) && FS_NOTE(2, 6, 0) == 53 + 20);
    te_delete_step(2, 4);
    CHECK(7, !dr_pat[2].lane[0].s[4].on && !dr_pat[2].lane[2].s[4].on && dr_pat[2].lane[0].s[8].on);
    /* TE's step record into a TE pattern on screen: imported again */
    dr_cur = 6;
    te_step_record(6, 3, 2);
    CHECK(7, dr_pat[6].lane[0].key == 2 && dr_pat[6].lane[0].s[3].on && !(dr_owned >> 6 & 1));
    te_delete_step(6, 3);
    CHECK(7, !dr_pat[6].lane[0].s[3].on);
    dr_cur = 2;

    /* 8: the view follows TE's selected pattern at the next key / knob event; shift + green turn
     * browses the 14 patterns (TE's are imported when shown) */
    FS_SELECTED = 6;
    turn(0, 0, 0);                              /* no movement: nothing */
    CHECK(8, dr_cur == 2);
    dr_page = 0;
    turn(0, 1, 0);                              /* blue turn: follows first, then moves the cursor */
    CHECK(8, dr_cur == 6 && !(dr_owned >> 6 & 1));
    turn(1, 20, 1);
    CHECK(8, dr_cur == 13);
    turn(1, -11, 1);
    CHECK(8, dr_cur == 2 && dr_pat[2].lane[0].key == 0);      /* DRUM ROLL's pattern: not re-imported */
    hook_key(39, 1);                            /* a pattern key (TE's): TE starts pattern 2 meanwhile */
    FS_SELECTED = 1;
    hook_key(39, 0);                            /* let go: the view follows */
    CHECK(8, dr_cur == 1);
    FS_SELECTED = 2;
    turn(1, 0, 1);
    hook_key(39, 1);
    hook_key(39, 0);
    CHECK(8, dr_cur == 2);
    dr_cursor = 0;
    dr_sel = 0;

    /* 16: the pattern length. TE's length of DRUM ROLL's pattern 3 16 -> 24 (finger screen): every
     * lane follows at the next main-loop pass (a 1/8T lane: 18 steps); page 6 PAT, blue turn: all
     * lanes and TE's length (TE keeps at most 32, a longer pattern stays), the other knobs nothing;
     * TE's length of a TE pattern on screen: imported again */
    {
        dr_pattern *pt = &dr_pat[2];
        int l, m;

        pt->lane[7].rate = 2;                   /* 1/8T */
        pt->lane[7].len = 12;
        FS_LEN(2) = 24;
        mains(1);
        for (l = 0, ok = 1; l < DR_LANES; l++)
            ok &= pt->lane[l].len == (l == 7 ? 18 : 24);
        CHECK(16, ok && FS_LEN(2) == 24);
        mains(1);
        CHECK(16, pt->lane[0].len == 24);
        dr_page = 5;
        m = marks;
        turn(0, 8, 0);                          /* blue: 32 */
        CHECK(16, pt->lane[0].len == 32 && pt->lane[7].len == 24 && FS_LEN(2) == 32 && marks > m);
        turn(1, 3, 0);
        turn(2, 3, 0);
        turn(3, 3, 0);                          /* the other knobs: nothing */
        CHECK(16, pt->lane[0].len == 32 && dr_page == 5 && !dr_ask);
        turn(0, 10, 0);                         /* 42: TE stays at 32 */
        mains(2);
        CHECK(16, pt->lane[0].len == 42 && pt->lane[7].len == 32 && FS_LEN(2) == 32);
        dr_cursor = 40;
        FS_LEN(2) = 12;                         /* TE shortens it: the cursor stays on the lane */
        mains(1);
        CHECK(16, pt->lane[0].len == 12 && pt->lane[7].len == 9 && dr_cursor < 12);
        FS_LEN(2) = 16;
        mains(1);
        CHECK(16, pt->lane[0].len == 16 && pt->lane[7].len == 12 && (dr_owned >> 2 & 1));
        pt->lane[7].rate = DR_RATE_16;
        pt->lane[7].len = 16;
        dr_cur = 6;                             /* TE's pattern 7 on screen */
        FS_LEN(6) = 8;
        mains(1);
        CHECK(16, dr_pat[6].lane[0].len == 8 && !(dr_owned >> 6 & 1));
        FS_LEN(6) = 16;
        mains(1);
        dr_cur = 2;
        dr_page = 0;
        dr_cursor = 0;
        dr_sel = 0;
    }

    /* 17: the playhead while a pattern key is held (no key / knob event meanwhile): the view goes to
     * the pattern TE started at the next main-loop pass; TE's own pattern: TE's step; DRUM ROLL's:
     * its clock; nothing plays: none */
    {
        int st;

        hook_frame((uint16_t *)FB);
        CHECK(17, dr_cur == 2 && dr_ph_shown == -1);
        FS_ARTIST(0) = 6;                       /* TE's pattern 7, held */
        tick(5 * 5513 / 64 + 2);
        mains(1);
        hook_frame((uint16_t *)FB);
        st = FS_STEP(0);
        CHECK(17, dr_cur == 6 && st == 5 && dr_ph_shown == st);
        FS_ARTIST(0) = -1;
        tick(10);
        FS_ARTIST(0) = 2;                       /* DRUM ROLL's pattern 3, held */
        tick(3 * 5513 / 64 + 2);
        mains(1);
        hook_frame((uint16_t *)FB);
        CHECK(17, dr_cur == 2 && dr_play[2].on && dr_ph_shown == 3);
        stop_all();
        hook_frame((uint16_t *)FB);
        CHECK(17, dr_ph_shown == -1);
    }

    /* 18: punch-in effects (12 of the PO-20's) on the black keys of TE's drum finger screen: plain
     * black = effects 1-10, shift + the lower F# G# = 11, 12, shift + the others stay TE's step
     * record. Pattern 11: the kick (53) on step 1, key 3 (55) on steps 2..16.
     * RETRIG from step 1: the kick every step, none of 55; + RETRIG FAST: twice a step; GLITCH:
     * pieces from elsewhere in the bar; RISE: the kick at 1/32, one semitone up each time; SOLO 1:
     * the kick lane only; FILL: extra hits; released: as before */
    {
        dr_pattern *pt = &dr_pat[10];
        int k53, k55, s, j;

        dr_pattern_init(pt);
        pt->lane[1].key = 2;
        on(&pt->lane[0], 0, 100);
        for (s = 1; s < 16; s++)
            on(&pt->lane[1], s, 100);
        dr_owned |= 1 << 10;
        n = inputs;
        *TE_SHIFT_HELD = 1;
        hook_key(24, 1);                        /* shift + upper G#: TE's */
        *TE_SHIFT_HELD = 0;
        hook_key(24, 0);
        CHECK(18, inputs == n + 2 && !dr_px_held);
        nlog = 0;
        FS_ARTIST(0) = 10;
        tick(3);                                /* step 1, a few blocks in */
        hook_key(14, 1);                        /* RETRIG */
        CHECK(18, dr_px_held == 1 << 1 && inputs == n + 2);
        a = nlog;
        tick(8 * 5513 / 64);
        k53 = count_on(a, 53, 0);
        k55 = count_on(a, 55, 0);
        CHECK(18, k53 >= 8 && k53 <= 9 && k55 == 0);
        hook_key(15, 1);                        /* + RETRIG FAST: the last pressed wins */
        memset(FBN(5), 0, 320 * 512);
        hook_frame(FBN(5));                     /* screenshot: the punch-in window */
        a = nlog;
        tick(4 * 5513 / 64);
        CHECK(18, count_on(a, 53, 0) >= 8 && count_on(a, 53, 0) <= 9);
        hook_key(15, 0);
        hook_key(14, 0);
        CHECK(18, !dr_px_held && inputs == n + 2);
        {                                       /* a bar as it is, then a bar of GLITCH: */
            uint32_t w0 = block, w1, kb0 = 0, kb1 = 0;      /* the kick moves (or comes more */
            int n0, n1;                                     /* or less often) */

            a = nlog;
            tick(16 * 5513 / 64);
            n0 = count_on(a, 53, 0);
            i = find_on(a, 53);
            kb0 = i >= 0 ? lg[i].block - w0 : 0;
            hook_key(32, 1);                    /* GLITCH */
            w1 = block;
            a = nlog;
            tick(16 * 5513 / 64);
            hook_key(32, 0);
            n1 = count_on(a, 53, 0);
            i = find_on(a, 53);
            kb1 = i >= 0 ? lg[i].block - w1 : 0;
            k55 = count_on(a, 55, 0);
            CHECK(18, n0 == 1 && (n1 != 1 || kb1 + 2 < kb0 || kb1 > kb0 + 2) && n1 + k55 >= 8);
        }
        tick(STEPB(64) - 3 - 44 * 5513 / 64 - 1);   /* to just before a loop start */
        hook_key(22, 1);                        /* RISE */
        a = nlog;
        tick(4 * 5513 / 64);
        for (i = a, j = 0, ok = 1; i < nlog; i++)
            if (lg[i].on && lg[i].note == 53) {
                ok &= lg[i].extra == j || lg[i].extra == j + 1;
                j = lg[i].extra;
            }
        CHECK(18, ok && j >= 6 && count_on(a, 55, 0) == 0 && dr_px_held == 1 << 4);
        hook_key(22, 0);
        CHECK(18, !dr_px_held);
        hook_key(26, 1);                        /* SOLO 1 */
        a = nlog;
        tick(16 * 5513 / 64);
        hook_key(26, 0);
        CHECK(18, count_on(a, 55, 0) == 0 && count_on(a, 53, 0) == 1);
        a = nlog;
        tick(16 * 5513 / 64);
        k55 = count_on(a, 55, 0);
        CHECK(18, k55 >= 14 && k55 <= 16);      /* back to every step */
        hook_key(13, 1);                        /* FILL */
        a = nlog;
        tick(16 * 5513 / 64);
        hook_key(13, 0);
        CHECK(18, count_on(a, 53, 0) + count_on(a, 55, 0) > 16 + 2 && violations == 0);
        FS_ARTIST(0) = -1;
        tick(400);
        CHECK(18, active == 0 && rejected == 0 && inputs == n + 2);
        FS_SELECTED = 2;                        /* TE selected 11 when it started: back to 3 */
        mains(1);
        CHECK(18, dr_cur == 2);
    }
    /* 19: the audio effects on a voice played by hand: BLINDS shuts the second half of every 1/16,
     * LP SWEEP damps Nyquist after a bar, HP SWEEP takes DC away after one; released: untouched */
    {
        int b, v, z = 0, op = 0, mx = 0;

        emu_on(&(te_note){ 53, -1, 1, { 0, 0, 0 }, 0 }, 0);
        v = (next_voice + 11) % 12;
        render_mode = 0;
        hook_key(25, 1);                        /* BLINDS */
        for (b = 0; b < 2 * 5513 / 64; b++) {
            tick(1);
            dr_te_render(VOICE(v), out, PARAMS(0));
            for (i = 0; i < 128; i++) {
                z += out[i] == 0;
                op += out[i] >= 4990;
            }
        }
        hook_key(25, 0);
        CHECK(19, z > 20 * 128 && op > 20 * 128);
        hook_key(23, 1);                        /* LP SWEEP */
        tick(16 * 5513 / 64 + 2);
        render_mode = 1;
        for (b = 0; b < 4; b++)
            dr_te_render(VOICE(v), out, PARAMS(0));
        for (i = 0; i < 128; i++)
            mx = out[i] > mx ? out[i] : -out[i] > mx ? -out[i] : mx;
        hook_key(23, 0);
        CHECK(19, mx < 3000);
        tick(40);                               /* LP opens again */
        hook_key(24, 1);                        /* HP SWEEP: a bar */
        tick(16 * 5513 / 64 + 2);
        render_mode = 0;
        for (b = 0; b < 30; b++)
            dr_te_render(VOICE(v), out, PARAMS(0));
        CHECK(19, out[127] < 500 && out[127] > -500);
        hook_key(24, 0);
        tick(40);
        dr_te_render(VOICE(v), out, PARAMS(0));
        CHECK(19, (out[0] == 5000 || out[0] == 10000) && !dr_px_held);
        emu_off(&(te_note){ 53, -1, 1, { 0, 0, 0 }, 0 });
        tick(10);
    }

    /* 20: STEP REC (shift + white press): keys play drums into the cursor step of every lane (the
     * first key clears the step, more keys add), a drum without a lane gets an empty one, FORWARD /
     * BACK move (a step passed by stays); the keys are not TE's meanwhile; off: TE's again */
    {
        dr_pattern *pt = &dr_pat[2];
        int l, kick = -1, snare = -1, nk, on1, k;

        dr_page = 0;
        dr_sel = 0;
        dr_cursor = 0;
        n = inputs;
        push(WHITE, 1);
        CHECK(20, dr_rec && dr_open);
        hook_key(39, 1);                        /* key 1 (drum 0) */
        hook_key(41, 1);                        /* + key 5 (drum 4) */
        hook_key(39, 0);
        hook_key(41, 0);
        for (l = 0, nk = 0; l < DR_LANES; l++) {
            if (pt->lane[l].key == 0)
                kick = l;
            if (pt->lane[l].key == 4)
                snare = l;
            nk += pt->lane[l].s[0].on;
        }
        CHECK(20, kick >= 0 && snare >= 0 && pt->lane[kick].s[0].on && pt->lane[snare].s[0].on && nk == 2
                  && inputs == n);
        for (l = 0, on1 = 0; l < DR_LANES; l++)
            on1 += pt->lane[l].s[1].on;
        hook_key(TE_KEY_FORWARD, 1);
        hook_key(TE_KEY_FORWARD, 0);
        hook_key(TE_KEY_FORWARD, 1);            /* step 2 passed by: as it was */
        hook_key(TE_KEY_FORWARD, 0);
        for (l = 0, k = 0; l < DR_LANES; l++)
            k += pt->lane[l].s[1].on;
        CHECK(20, dr_cursor == 2 && k == on1);
        for (l = 0, k = -1; l < DR_LANES; l++)  /* a drum on no lane yet */
            if (pt->lane[l].key == 23)
                k = l;
        hook_key(53, 1);                        /* key 24 (drum 23) */
        hook_key(53, 0);
        for (l = 0, nk = -1; l < DR_LANES; l++)
            if (pt->lane[l].key == 23)
                nk = l;
        CHECK(20, k < 0 && nk >= 0 && pt->lane[nk].s[2].on);
        hook_key(TE_KEY_BACK, 1);
        hook_key(TE_KEY_BACK, 0);
        hook_key(TE_KEY_BACK, 1);
        hook_key(TE_KEY_BACK, 0);
        hook_key(39, 1);                        /* back on step 1: the kick alone */
        hook_key(39, 0);
        for (l = 0, k = 0; l < DR_LANES; l++)
            k += pt->lane[l].s[0].on;
        CHECK(20, dr_cursor == 0 && k == 1 && pt->lane[kick].s[0].on && inputs == n && (dr_owned >> 2 & 1));
        tick(100);                              /* the previews out */
        push(WHITE, 1);                         /* off: the keys are TE's again */
        hook_key(39, 1);
        hook_key(39, 0);
        CHECK(20, !dr_rec && inputs == n + 2 && violations == 0);
        /* back as check 9 wants it: the kick on steps 1 5 9 13 and the hat on 1, snare on 13 */
        for (l = 0; l < DR_LANES; l++)
            for (k = 0; k < DR_STEPS; k++)
                pt->lane[l].s[k].on = 0;
        for (k = 0; k < 16; k += 4)
            on(&pt->lane[kick], k, 100);
        dr_cursor = 0;
        dr_sel = 0;
    }

    /* 21: undo / redo (shift + BACK / FORWARD): presses are steps of their own, knob turns of one kind
     * within a second one step; a TE pattern's first edit undone: TE's again with its notes; TE's
     * own step record on the finger screen (editor closed) undone and redone */
    {
        static dr_pattern p0;
        int s5, u0;

        p0 = dr_pat[2];
        u0 = dr_undo_n;
        dr_page = 0;
        dr_sel = 0;
        dr_cursor = 5;
        s5 = dr_pat[2].lane[0].s[5].on;
        push(BLUE, 0);                          /* step 6 on / off */
        dr_cursor = 6;
        push(BLUE, 0);                          /* step 7 */
        turn(2, -5, 0);                         /* VEL x 5: one step */
        CHECK(21, dr_undo_n == u0 + 3 && dr_pat[2].lane[0].s[5].on != s5 && dr_pat[2].lane[0].s[6].p[DR_VEL] == 75);
        *TE_SHIFT_HELD = 1;
        hook_key(TE_KEY_BACK, 1);               /* undo: VEL 100 */
        hook_key(TE_KEY_BACK, 0);
        CHECK(21, dr_pat[2].lane[0].s[6].p[DR_VEL] == 100 && dr_redo_n == 1);
        hook_key(TE_KEY_BACK, 1);
        hook_key(TE_KEY_BACK, 0);
        hook_key(TE_KEY_BACK, 1);               /* undo x 3: as before */
        hook_key(TE_KEY_BACK, 0);
        *TE_SHIFT_HELD = 0;
        CHECK(21, !memcmp(&dr_pat[2], &p0, sizeof(p0)) && dr_undo_n == u0 && dr_redo_n == 3);
        *TE_SHIFT_HELD = 1;
        hook_key(TE_KEY_FORWARD, 1);            /* redo: step 6 again */
        hook_key(TE_KEY_FORWARD, 0);
        *TE_SHIFT_HELD = 0;
        CHECK(21, dr_pat[2].lane[0].s[5].on != s5 && dr_redo_n == 2);
        push(BLUE, 0);                          /* a new edit: no redo any more */
        CHECK(21, dr_redo_n == 0);
        *TE_SHIFT_HELD = 1;
        hook_key(TE_KEY_BACK, 1);
        hook_key(TE_KEY_BACK, 0);
        hook_key(TE_KEY_BACK, 1);
        hook_key(TE_KEY_BACK, 0);
        *TE_SHIFT_HELD = 0;
        CHECK(21, !memcmp(&dr_pat[2], &p0, sizeof(p0)));
        /* TE's pattern 9 (index 8), first edit in DRUM ROLL, undone: TE's again, TE's notes back */
        FS_NOTE(8, 3, 0) = 55;
        FS_SELECTED = 8;
        mains(1);
        dr_cursor = 0;
        push(BLUE, 0);
        CHECK(21, dr_cur == 8 && (dr_owned >> 8 & 1) && FS_NOTE(8, 0, 0) != -1);
        *TE_SHIFT_HELD = 1;
        hook_key(TE_KEY_BACK, 1);
        hook_key(TE_KEY_BACK, 0);
        *TE_SHIFT_HELD = 0;
        CHECK(21, !(dr_owned >> 8 & 1) && FS_NOTE(8, 0, 0) == -1 && FS_NOTE(8, 3, 0) == 55);
        /* the editor closed, TE's finger screen: TE's step record of key 5 on step 10, undone, redone */
        push(BLUE, 1);
        CHECK(21, !dr_open);
        te_step_record(8, 9, 4);
        CHECK(21, FS_NOTE(8, 9, 0) == 57);
        n = inputs;
        *TE_SHIFT_HELD = 1;
        hook_key(TE_KEY_BACK, 1);
        hook_key(TE_KEY_BACK, 0);
        *TE_SHIFT_HELD = 0;
        CHECK(21, FS_NOTE(8, 9, 0) == -1 && FS_NOTE(8, 3, 0) == 55 && inputs == n);
        hook_frame((uint16_t *)FB);             /* the UNDO message */
        *TE_SHIFT_HELD = 1;
        hook_key(TE_KEY_FORWARD, 1);
        hook_key(TE_KEY_FORWARD, 0);
        *TE_SHIFT_HELD = 0;
        CHECK(21, FS_NOTE(8, 9, 0) == 57);
        FS_NOTE(8, 9, 0) = FS_NOTE(8, 3, 0) = -1;
        FS_SELECTED = 2;
        push(BLUE, 1);                          /* open again on pattern 3 */
        mains(1);
        CHECK(21, dr_open && dr_cur == 2);
        dr_cursor = 0;
        dr_sel = 0;
    }

    /* 22: punch-in banks: green press on TE's finger screen (editor closed) cycles A -> B -> C (TE
     * never sees it). Bank B: 8-BIT crushes a voice; LASER rolls the kick at 1/64, 2 semitones down
     * each; COIN adds an octave-up hit a quarter step after each; REVERSE plays the bar's steps
     * backwards (the kick moves to the bar's end). Bank C: DOUBLE TIME = two kicks a bar; CUT = none */
    {
        int b0k, j, kb0, kb1, v;

        push(BLUE, 1);                          /* close */
        n = inputs;
        push(GREEN, 0);
        CHECK(22, dr_px_bank == 1 && inputs == n && !dr_open);
        hook_key(13, 1);                        /* 8-BIT */
        emu_on(&(te_note){ 53, -1, 1, { 0, 0, 0 }, 0 }, 0);
        v = (next_voice + 11) % 12;
        render_mode = 0;
        tick(2);
        dr_te_render(VOICE(v), out, PARAMS(0));
        for (i = 0, ok = 1; i < 128; i++)
            ok &= (out[i] & 0x7ff) == 0;
        hook_key(13, 0);
        emu_off(&(te_note){ 53, -1, 1, { 0, 0, 0 }, 0 });
        CHECK(22, ok && dr_px_held == 0);
        nlog = 0;
        FS_ARTIST(0) = 10;                      /* pattern 11: the kick on 1, 55 on 2..16 */
        tick(3);
        hook_key(14, 1);                        /* LASER */
        a = nlog;
        tick(2 * 5513 / 64);
        hook_key(14, 0);
        for (i = a, j = 0, b0k = 99, ok = 1; i < nlog; i++)
            if (lg[i].on && lg[i].note == 53) {         /* each 2 semitones under the one before */
                ok &= b0k == 99 ? lg[i].extra <= 0 : lg[i].extra == b0k - 2 || lg[i].extra == -24;
                b0k = lg[i].extra;
                j++;
            }
        CHECK(22, ok && j >= 7 && b0k <= -12);
        hook_key(32, 1);                        /* COIN */
        a = nlog;
        tick(16 * 5513 / 64);
        hook_key(32, 0);
        for (i = a, j = 0, b0k = 0; i < nlog; i++)
            if (lg[i].on && lg[i].note == 53)
                j += lg[i].extra == 12;
        CHECK(22, j >= 1 && count_on(a, 53, 0) >= 2);
        tick(400);
        a = nlog;                               /* a bar as it is, then a bar reversed */
        b0k = (int)block;
        tick(16 * 5513 / 64);
        i = find_on(a, 53);
        kb0 = i >= 0 ? (int)lg[i].block - b0k : -1;
        hook_key(26, 1);                        /* REVERSE */
        a = nlog;
        b0k = (int)block;
        tick(16 * 5513 / 64);
        hook_key(26, 0);
        i = find_on(a, 53);
        kb1 = i >= 0 ? (int)lg[i].block - b0k : -1;
        CHECK(22, kb0 >= 0 && kb1 >= 0 && (kb1 > kb0 + 4 || kb1 + 4 < kb0) && count_on(a, 53, 0) == 1);
        push(GREEN, 0);
        CHECK(22, dr_px_bank == 2);
        hook_key(14, 1);                        /* DOUBLE TIME */
        a = nlog;
        tick(16 * 5513 / 64);
        hook_key(14, 0);
        CHECK(22, count_on(a, 53, 0) == 2);
        hook_key(27, 1);                        /* CUT */
        a = nlog;
        tick(4 * 5513 / 64);
        CHECK(22, count_on(a, 53, 0) + count_on(a, 55, 0) == 0);
        hook_frame((uint16_t *)FB);             /* the window over TE's finger screen */
        hook_key(27, 0);
        push(GREEN, 0);
        CHECK(22, dr_px_bank == 0 && inputs == n);
        FS_ARTIST(0) = -1;
        tick(800);
        CHECK(22, active == 0 && violations == 0);
        FS_SELECTED = 2;
        push(BLUE, 1);                          /* open again on pattern 3 */
        mains(1);
        CHECK(22, dr_open && dr_cur == 2);
        dr_cursor = 0;
        dr_sel = 0;
        dr_page = 0;
    }

    /* 23: two patterns at once (artist 0: DRUM ROLL's pattern 11, kick on 1, 55 on 2..16; artist 1:
     * TE's pattern 7 with one note, 60, on step 1): RETRIG pressed on step 6 acts on both; TE's
     * pattern is taken over (TE plays nothing of it meanwhile) and loops its last step with a hit
     * (step 1: pressed on an empty step), DRUM ROLL's its step 6 (55); released: TE plays its own
     * pattern again */
    {
        int c60, c55, c53, r0 = rejected;

        te_clear(6, 16);
        FS_NOTE(6, 0, 0) = 60;
        dr_owned &= (uint16_t)~(1 << 6);
        nlog = 0;
        FS_ARTIST(0) = 10;
        FS_ARTIST(1) = 6;
        tick(STEPB(5) + 4);                     /* step 6 */
        CHECK(23, count_on(0, 60, 1) == 1 && count_on(0, 60, 0) == 0);  /* TE plays its pattern */
        hook_key(14, 1);                        /* RETRIG (bank A) */
        a = nlog;
        tick(8 * 5513 / 64);
        c60 = count_on(a, 60, 0);
        c55 = count_on(a, 55, 0);
        c53 = count_on(a, 53, 0);
        CHECK(23, c60 >= 7 && c60 <= 9 && count_on(a, 60, 1) == 0 && c55 >= 7 && c55 <= 9 && c53 == 0);
        hook_key(14, 0);
        a = nlog;
        tick(16 * 5513 / 64);
        CHECK(23, count_on(a, 60, 1) >= 1 && count_on(a, 60, 0) == 0 && !(dr_owned >> 6 & 1));
        FS_ARTIST(0) = FS_ARTIST(1) = -1;
        tick(400);
        CHECK(23, active == 0 && violations == 0 && rejected == r0);
        te_clear(6, 16);
        FS_SELECTED = 2;
        mains(1);
    }

    /* 9: VEL 50 + LEN 1/2 step on the kick's step 1: gain 1/2, gate 2756 * 2 voice samples, then a
     * 256-sample fade, the note-off half a step later */
    push(GREEN, 0);                             /* page 2 NOTE: white = LEN */
    turn(2, -10, 0);                            /* FULL (12) -> 1/2 (2) */
    for (i = 0; i < 5; i++)
        push(GREEN, 0);                         /* page 1 */
    turn(2, -10, 0);                            /* white = VEL 50 */
    CHECK(9, dr_page == 0 && dr_param == DR_VEL);
    CHECK(9, dr_pat[2].lane[0].s[0].p[DR_VEL] == 50 && dr_pat[2].lane[0].s[0].p[DR_LEN] == 2);
    tick(400);
    a = nlog;
    b0 = (int)block;
    FS_ARTIST(0) = 2;
    tick(1);
    i = find_on(a, 53);
    CHECK(9, i >= 0 && lg[i].block == (uint32_t)b0);
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
        CHECK(9, ok);
    }
    tick(60);
    for (a = i + 1; a < nlog && !(!lg[a].on && lg[a].note == 53); a++)
        ;
    CHECK(9, i >= 0 && a < nlog && lg[a].block >= (uint32_t)b0 + 43 - 1 && lg[a].block <= (uint32_t)b0 + 43 + 1);
    stop_all();

    /* 10: page SOUND: the kick's sample LP at ~25 Hz damps +-10000 at Nyquist (previewed: white
     * press), another sample stays; a key played by hand gets the filter too */
    for (i = 0; i < 4; i++)
        push(GREEN, 0);                         /* page 5 */
    CHECK(10, dr_page == 4);
    turn(0, 1, 0);                              /* blue = FILTER: LP */
    turn(1, -30, 0);                            /* green = CUTOFF 64 -> 4 */
    push(GREEN, 0);
    push(GREEN, 0);                             /* page 1 (past page 6 CLEAR) */
    push(WHITE, 0);
    CHECK(10, dr_page == 0 && (dr_owned >> 2 & 1));
    a = nlog;
    tick(1);
    i = find_on(a, 53);
    CHECK(10, i >= 0);
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
        CHECK(10, mx < 300);
    }
    tick(200);
    emu_on(&(te_note){ 54, -1, 1, { 0, 0, 0 }, 0 }, 0);       /* another sample: no filter */
    dr_te_render(VOICE((next_voice + 11) % 12), out, PARAMS(1));
    CHECK(10, out[0] == 10000 && out[1] == -10000);
    emu_off(&(te_note){ 54, -1, 1, { 0, 0, 0 }, 0 });
    emu_on(&(te_note){ 53, -1, 1, { 0, 0, 0 }, 0 }, 0);       /* by hand: the filter */
    dr_te_render(VOICE((next_voice + 11) % 12), out, PARAMS(0));
    CHECK(10, out[100] < 1000 && out[100] > -1000);
    emu_off(&(te_note){ 53, -1, 1, { 0, 0, 0 }, 0 });
    emu_on(&(te_note){ 53, -1, 1, { 0, 0, 0 }, 0 }, 0);       /* LP keeps DC */
    render_mode = 0;
    for (i = 0; i < 60; i++)
        dr_te_render(VOICE((next_voice + 11) % 12), out, PARAMS(0));
    CHECK(10, out[127] > 9950 && out[127] < 10050);
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
    push(GREEN, 0); push(GREEN, 0); push(GREEN, 0); push(GREEN, 0);
    turn(0, -1, 0);                             /* filter off */
    push(GREEN, 0); push(GREEN, 0);

    /* 11: synth playmode: no drum notes, no patterns; another drum sequencer: none either; the
     * get_note hook leaves the synth finger data alone */
    *TE_PLAYMODE = 0;
    *TE_PLAYMODE_TARGET = 0;
    a = nlog;
    FS_ARTIST(0) = 2;
    in_te = 1;
    CHECK(11, dr_fs_get_note((void *)0x01dd98bc, 2, 0, 0) == 0x5a5a && !dr_play[2].on);
    in_te = 0;
    for (i = 0; i < 1400; i++) {                /* TE's drum player does not run in synth playmode */
        hook_tick();
        block++;
    }
    CHECK(11, nlog == a && violations == 0 && !dr_play[2].on);
    *TE_PLAYMODE = 1;
    *TE_PLAYMODE_TARGET = 1;
    *TE_DRUM_SEQ_TYPE = TE_SEQ_PATTERN;
    tick(400);
    CHECK(11, !dr_play[2].on);
    *TE_DRUM_SEQ_TYPE = TE_SEQ_FINGER;
    stop_all();
    CHECK(11, active == 0);

    /* 15: screen draws, closes; closed while a DRUM ROLL pattern plays: only the finger screen note;
     * tick cost */
    {
        uint32_t t0;

        FS_ARTIST(0) = 2;
        tick(400);
        t0 = cycles();
        hook_frame((uint16_t *)FB);
        cycles_frame = cycles() - t0;
        push(GREEN, 0);
        hook_frame((uint16_t *)FB);
        push(BLUE, 1);                          /* close */
        CHECK(15, !dr_open);
        hook_frame((uint16_t *)FB);
        memset(FBN(4), 0, 320 * 512);
        hook_frame(FBN(4));                     /* TE's finger screen: the note, no badge */
        t0 = cycles();
        tick(1);
        cycles_tick = cycles() - t0;
        stop_all();
        demo();
        clear_dialog();
        store_save();
        stop_all();
        *TE_PLAYMODE = 0;                       /* the other features' frames only */
        t0 = cycles();
        hook_frame((uint16_t *)FB);
        cycles_frame -= cycles() - t0;
        *TE_PLAYMODE = 1;
    }
    CHECK(15, bad_calls == 0 && active == 0 && violations == 0);
    return fails;
}
