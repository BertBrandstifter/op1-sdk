/* host tests for dr_fs.c: cc -std=c99 test_dr_fs.c dr_fs.c && ./a.out
 * (dr_core.c is included below: the import builds lanes with dr_lane_init) */
#include <stdio.h>
#include <string.h>
#include "dr_fs.h"
#include "dr_core.c"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static dr_fs_notes notes;

static void clear_notes(void)
{
    int s;

    for (s = 0; s < DR_FS_STEPS; s++)
        notes[s][0] = notes[s][1] = -1;
}

static void key_tests(void)
{
    int oct = 9;

    CHECK(dr_fs_key(53, &oct) == 0 && oct == 0);
    CHECK(dr_fs_key(76, &oct) == 23 && oct == 0);
    CHECK(dr_fs_key(77, &oct) == 0 && oct == 1);        /* one octave up (TE: +24 per offset) */
    CHECK(dr_fs_key(29 + 3, &oct) == 3 && oct == -1);
    CHECK(dr_fs_key(-1, 0) == -1 && dr_fs_key(4, 0) == -1 && dr_fs_key(200, 0) == -1);
}

static void import_tests(void)
{
    static dr_pattern p;
    int l, s, n;

    /* kick (key 0) on 0 4 8 12, snare (key 4) on 4 12 in hand 1, hat (key 9) every 2nd step,
     * one hat an octave up; length 16 */
    clear_notes();
    for (s = 0; s < 16; s += 4)
        notes[s][0] = 53;
    notes[4][1] = 57;
    notes[12][1] = 57;
    for (s = 2; s < 16; s += 4)
        notes[s][0] = 62;
    notes[14][0] = 62 + 24;
    notes[20][0] = 70;                          /* past the length: not imported */
    dr_fs_import(notes, 16, &p);
    CHECK(p.lane[0].key == 0 && p.lane[1].key == 9 && p.lane[2].key == 4);   /* order of first use */
    CHECK(p.lane[3].key == 1 && p.lane[4].key == 2 && p.lane[5].key == 3 && p.lane[6].key == 5
          && p.lane[7].key == 6);               /* the rest: lowest unused keys */
    for (l = 0, n = 0; l < DR_LANES; l++) {
        CHECK(p.lane[l].len == 16 && p.lane[l].rate == DR_RATE_16 && p.lane[l].level == 100);
        for (s = 0; s < DR_STEPS; s++)
            n += p.lane[l].s[s].on;
    }
    CHECK(n == 4 + 2 + 4);
    CHECK(p.lane[0].s[0].on && p.lane[0].s[12].on && !p.lane[0].s[1].on);
    CHECK(p.lane[2].s[4].on && p.lane[2].s[12].on);
    CHECK(p.lane[1].s[2].on && p.lane[1].s[2].p[DR_TONE] == 0 && p.lane[1].s[14].p[DR_TONE] == 12);
    CHECK(p.lane[0].s[0].p[DR_VEL] == 100 && p.lane[0].s[0].p[DR_LEN] == DR_LEN_FULL);

    /* more than 8 keys: the first 8 only; length clamped */
    clear_notes();
    for (s = 0; s < 10; s++)
        notes[s][0] = 53 + 2 * s;
    dr_fs_import(notes, 40, &p);
    CHECK(p.lane[0].len == 32 && p.lane[7].key == 14 && p.lane[7].s[7].on);
    for (l = 0, n = 0; l < DR_LANES; l++)
        for (s = 0; s < DR_STEPS; s++)
            n += p.lane[l].s[s].on;
    CHECK(n == 8);

    /* empty pattern: keys 1..8 as a new pattern, length as TE's */
    clear_notes();
    dr_fs_import(notes, 8, &p);
    CHECK(p.lane[0].key == 0 && p.lane[7].key == 7 && p.lane[3].len == 8);
}

static void summary_tests(void)
{
    static dr_pattern p;
    int s;

    dr_pattern_init(&p);
    clear_notes();
    p.lane[0].s[0].on = 1;                      /* key 0: 0 4 (len 8 loops: 8 12 ... 28) */
    p.lane[0].s[4].on = 1;
    p.lane[0].len = 8;
    p.lane[1].s[4].on = 1;                      /* key 1 on 4 */
    p.lane[2].s[4].on = 1;                      /* key 2 on 4 too: a third hit, not in TE's copy */
    p.lane[3].rate = 5;                         /* 1/32 lane, key 3: step 1 starts in sixteenth 0 */
    p.lane[3].s[1].on = 1;
    p.lane[3].s[6].on = 1;                      /* sixteenth 3 */
    p.lane[3].len = 32;
    p.lane[4].rate = 1;                         /* 1/8 lane, key 4: step 3 = sixteenth 6 */
    p.lane[4].s[3].on = 1;
    p.lane[5].s[9].on = 1;
    p.lane[5].mute = 1;                         /* muted: not in the summary */
    p.lane[6].key = 0;                          /* a second lane on key 0: one note per key */
    p.lane[6].s[0].on = 1;
    dr_fs_summary(&p, notes);
    CHECK(notes[0][0] == 53 && notes[0][1] == 56);
    CHECK(notes[4][0] == 53 && notes[4][1] == 54);
    CHECK(notes[8][0] == 53 && notes[28][0] == 53 && notes[12][0] == 53 && notes[12][1] == -1);
    CHECK(notes[3][0] == 56 && notes[6][0] == 57 && notes[9][0] == -1);
    for (s = 0; s < DR_FS_STEPS; s++)
        CHECK(notes[s][0] == -1 || (notes[s][0] >= 53 && notes[s][0] <= 76));
    CHECK(notes[16 + 3][0] == 56);              /* the 1/32 lane (32 steps = 16 sixteenths) loops */
}

static void edit_tests(void)
{
    static dr_pattern p;
    int l;

    dr_pattern_init(&p);
    /* step record key 2 at step 5: lane 2 (key 2, 1/16) */
    CHECK(dr_fs_te_edit(&p, 5, -1, 55, 16) == 1 && p.lane[2].s[5].on);
    /* delete it again */
    CHECK(dr_fs_te_edit(&p, 5, 55, -1, 16) == 1 && !p.lane[2].s[5].on);
    CHECK(dr_fs_te_edit(&p, 5, 55, -1, 16) == 0);
    /* key 20 has no lane: the first empty lane (0) takes it, length up to TE's, step past it grows */
    p.lane[1].s[0].on = 1;
    p.lane[0].s[3].on = 1;
    p.lane[0].len = 4;
    CHECK(dr_fs_te_edit(&p, 2, -1, 73, 12) == 1);
    CHECK(p.lane[2].key == 20 && p.lane[2].s[2].on && p.lane[2].len == 16 && p.lane[0].key == 0);
    CHECK(dr_fs_te_edit(&p, 20, -1, 73, 12) == 1 && p.lane[2].len == 21 && p.lane[2].s[20].on);
    /* an octave-up note: TONE +12 */
    CHECK(dr_fs_te_edit(&p, 1, -1, 53 + 24 + 7, 16) == 1);
    for (l = 0; l < DR_LANES && p.lane[l].key != 7; l++)
        ;
    CHECK(l < DR_LANES && p.lane[l].s[1].on && p.lane[l].s[1].p[DR_TONE] == 12);
    /* a 1/32 lane on a key is not TE's grid: another lane takes the note */
    dr_pattern_init(&p);
    p.lane[0].rate = 5;
    CHECK(dr_fs_te_edit(&p, 3, -1, 53, 16) == 1 && !p.lane[0].s[3].on);
    for (l = 1; l < DR_LANES && p.lane[l].key != 0; l++)
        ;
    CHECK(l == 1 && p.lane[1].s[3].on && p.lane[1].rate == DR_RATE_16);
    /* all 8 lanes busy: ignored */
    dr_pattern_init(&p);
    for (l = 0; l < DR_LANES; l++)
        p.lane[l].s[0].on = 1;
    CHECK(dr_fs_te_edit(&p, 3, -1, 53 + 15, 16) == 0);
    /* replace: old off, new on */
    CHECK(dr_fs_te_edit(&p, 0, 53, 54, 16) == 1 && !p.lane[0].s[0].on && p.lane[1].s[0].on);
    CHECK(dr_fs_te_edit(&p, 40, -1, 53, 16) == 0);
    /* TE emptied step 0: every 1/16 lane's step 0 off, other rates and steps stay */
    p.lane[5].rate = 5;
    p.lane[3].s[1].on = 1;
    CHECK(dr_fs_te_clear_step(&p, 0) == 1);
    CHECK(!p.lane[2].s[0].on && !p.lane[7].s[0].on && p.lane[5].s[0].on && p.lane[3].s[1].on);
    CHECK(dr_fs_te_clear_step(&p, 0) == 0 && dr_fs_te_clear_step(&p, 32) == 0);
}

static void length_tests(void)
{
    dr_pattern p;
    int l;

    /* 1/4 1/8 1/8T 1/16 1/16T 1/32 at 16 sixteenths = 1 bar */
    CHECK(dr_fs_lane_len(0, 16) == 4 && dr_fs_lane_len(1, 16) == 8 && dr_fs_lane_len(2, 16) == 12
          && dr_fs_lane_len(3, 16) == 16 && dr_fs_lane_len(4, 16) == 24 && dr_fs_lane_len(5, 16) == 32);
    CHECK(dr_fs_lane_len(0, 1) == 1 && dr_fs_lane_len(0, 2) == 1 && dr_fs_lane_len(2, 5) == 4);  /* rounded, >= 1 */
    CHECK(dr_fs_lane_len(5, 64) == DR_STEPS && dr_fs_lane_len(3, 64) == 64);                     /* <= 64 */
    dr_pattern_init(&p);
    CHECK(dr_fs_length(&p) == 16);
    p.lane[2].len = 3;                          /* a short polymeter lane: the longest counts */
    p.lane[5].rate = 2;
    p.lane[5].len = 12;
    CHECK(dr_fs_length(&p) == 16);
    p.lane[1].len = 20;
    CHECK(dr_fs_length(&p) == 20);
    p.lane[0].s[25].on = 1;                     /* past the length: kept */
    CHECK(dr_fs_set_length(&p, 32) == 1 && dr_fs_length(&p) == 32);
    for (l = 0; l < DR_LANES; l++)
        CHECK(p.lane[l].len == (l == 5 ? 24 : 32));
    CHECK(dr_fs_set_length(&p, 32) == 0);
    CHECK(dr_fs_set_length(&p, 8) == 1 && p.lane[0].len == 8 && p.lane[5].len == 6 && p.lane[0].s[25].on);
    CHECK(dr_fs_set_length(&p, 0) == 1 && dr_fs_length(&p) == 1);
    CHECK(dr_fs_set_length(&p, 99) == 1 && dr_fs_length(&p) == 64 && p.lane[0].len == 64);
    for (l = 1; l <= 64; l++)                   /* with a 1/16 lane: every length exactly, triplets too */
        CHECK(dr_fs_set_length(&p, l) >= 0 && dr_fs_length(&p) == l);
    for (l = 0; l < DR_LANES; l++)
        p.lane[l].rate = 2;                     /* only 1/8T: the longest lane, rounded */
    CHECK(dr_fs_set_length(&p, 24) == 1 && p.lane[0].len == 18 && dr_fs_length(&p) == 24);
}

static void clock_tests(void)
{
    dr_fs_clock c = { 0 };
    uint64_t st = (uint64_t)(5512.5 * 65536);   /* 1/16 at 120 BPM */
    uint32_t from, to, b = 64, t;
    int i, ev;

    CHECK(dr_fs_window(&c, 0, st, b, &from, &to) == 0 && !c.on);
    CHECK(dr_fs_window(&c, 1, st, b, &from, &to) == 1 && from == 0 && to == 64 && c.on);
    /* steady: TE's events in the block where its step starts; windows contiguous, on real time */
    for (t = 64, i = 1; i < 2000; i++, t += 64) {
        uint32_t prev = c.next, due = (uint32_t)((uint64_t)(c.k + 1) * st >> 16);

        ev = due >= t && due < t + 64;
        dr_fs_window(&c, ev, st, b, &from, &to);
        CHECK(from == prev && to == t + 64);
    }
    /* TE is 100 samples ahead of us at an even step: catch up in one window */
    c.k = 9;
    c.next = (uint32_t)((10 * st) >> 16) - 100;
    dr_fs_window(&c, 1, st, b, &from, &to);
    CHECK(c.k == 10 && from == (uint32_t)((10 * st) >> 16) - 100 && to == (uint32_t)((10 * st) >> 16) + 64);
    /* we are 100 samples ahead: hold (empty window), then go on */
    c.k = 11;
    c.next = (uint32_t)((12 * st) >> 16) + 100;
    dr_fs_window(&c, 1, st, b, &from, &to);
    CHECK(from == to && to == c.next);
    dr_fs_window(&c, 0, st, b, &from, &to);
    CHECK(to - from == 64);
    /* odd steps never re-sync (swing moves them) */
    c.k = 12;
    c.next = 1000;
    dr_fs_window(&c, 1, st, b, &from, &to);
    CHECK(from == 1000 && to == 1064);
    /* far off: jump */
    c.k = 13;
    c.next = 5;
    dr_fs_window(&c, 1, st, b, &from, &to);
    CHECK(from == (uint32_t)((14 * st) >> 16) && to == from + 64);
    /* no events for 3 steps: off */
    for (i = 0; i < 3 * 5512 / 64 - 2; i++)
        CHECK(dr_fs_window(&c, 0, st, b, &from, &to) == 1);
    for (i = 0; i < 4 && c.on; i++)
        dr_fs_window(&c, 0, st, b, &from, &to);
    CHECK(!c.on && dr_fs_window(&c, 0, st, b, &from, &to) == 0);
    /* restart at 0 */
    CHECK(dr_fs_window(&c, 1, st, b, &from, &to) == 1 && from == 0 && c.k == 0);
}

int main(void)
{
    key_tests();
    import_tests();
    summary_tests();
    edit_tests();
    length_tests();
    clock_tests();
    printf(fails ? "dr_fs: %d FAILED\n" : "dr_fs: all passed\n", fails);
    return fails != 0;
}
