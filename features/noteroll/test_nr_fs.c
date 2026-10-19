/* Host tests for NOTE ROLL's finger glue (plain cc; nr_core.c is included below). */
#include <stdio.h>
#include <string.h>
#include "nr_fs.h"
#include "nr_core.c"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static nr_fs_notes te, te2;

static void clear(nr_fs_notes n)
{
    int s;

    for (s = 0; s < NR_FS_STEPS; s++)
        n[s][0] = n[s][1] = -1;
}

int main(void)
{
    nr_pattern p;
    nr_fs_clock c = { 0 };
    uint64_t st = (uint64_t)(5512.5 * 65536);
    uint32_t from, to, t;
    int i, ev;

    /* import: notes as stored, one step long, two per step, TE's length */
    clear(te);
    te[0][0] = 65; te[0][1] = 60;
    te[3][1] = 77;
    te[20][0] = 200;                            /* not a MIDI note: dropped */
    nr_fs_import(te, 12, &p);
    CHECK(p.len == 12 && nr_voices(&p.s[0]) == 2 && p.s[0].n[0].key == 60 && p.s[0].n[1].key == 65
          && p.s[0].n[0].len == NR_LEN_DEF && p.s[0].n[0].vel == 100);
    CHECK(nr_voices(&p.s[3]) == 1 && p.s[3].n[0].key == 77 && nr_voices(&p.s[20]) == 0);
    nr_fs_import(te, 40, &p);
    CHECK(p.len == 32);

    /* summary: lowest + highest, single note in hand 0, nothing past the length */
    nr_pattern_init(&p);
    p.len = 8;
    nr_add(&p.s[0], 60, 100, 4); nr_add(&p.s[0], 64, 100, 4); nr_add(&p.s[0], 67, 100, 4);
    nr_add(&p.s[2], 50, 100, 4);
    nr_add(&p.s[9], 70, 100, 4);
    nr_fs_summary(&p, te);
    CHECK(te[0][0] == 60 && te[0][1] == 67 && te[2][0] == 50 && te[2][1] == -1 && te[9][0] == -1
          && te[1][0] == -1);

    /* TE edits: step record adds, delete removes (the chord's other notes stay), clear */
    memcpy(te2, te, sizeof(te));
    te2[5][0] = 72;
    CHECK(nr_fs_te_edit(&p, te, te2) == 1 && p.s[5].n[0].key == 72 && p.s[5].n[0].len == NR_LEN_DEF);
    memcpy(te, te2, sizeof(te));
    te2[0][1] = -1;                             /* TE deleted hand 1 (67) */
    CHECK(nr_fs_te_edit(&p, te, te2) == 1 && nr_voices(&p.s[0]) == 2 && p.s[0].n[1].key == 64);
    memcpy(te, te2, sizeof(te));
    CHECK(nr_fs_te_edit(&p, te, te2) == 0);
    nr_add(&p.s[0], 67, 100, 4);                /* a 3-note chord, TE shows 60 + 67 */
    nr_fs_summary(&p, te);
    memcpy(te2, te, sizeof(te));
    te2[0][0] = te2[0][1] = -1;                 /* TE deletes the step: the whole chord goes */
    CHECK(nr_fs_te_edit(&p, te, te2) == 1 && !nr_voices(&p.s[0]) && nr_voices(&p.s[5]) == 1);
    memcpy(te, te2, sizeof(te));
    clear(te2);
    CHECK(nr_fs_te_edit(&p, te, te2) == 1 && !nr_voices(&p.s[5]) && !nr_voices(&p.s[2]));
    memcpy(te, te2, sizeof(te));
    te2[1][0] = 64; te2[1][1] = 64;             /* the same note twice: one */
    CHECK(nr_fs_te_edit(&p, te, te2) == 1 && nr_voices(&p.s[1]) == 1);

    /* clock: steady on real time, catch up, hold, jump, stop, restart */
    CHECK(nr_fs_window(&c, 0, st, 64, &from, &to) == 0);
    CHECK(nr_fs_window(&c, 1, st, 64, &from, &to) == 1 && from == 0 && to == 64);
    for (t = 64, i = 1; i < 2000; i++, t += 64) {
        uint32_t due = (uint32_t)((uint64_t)(c.k + 1) * st >> 16), prev = c.next;

        ev = due >= t && due < t + 64;
        nr_fs_window(&c, ev, st, 64, &from, &to);
        CHECK(from == prev && to == t + 64);
    }
    c.k = 9;
    c.next = (uint32_t)((10 * st) >> 16) - 100;
    nr_fs_window(&c, 1, st, 64, &from, &to);
    CHECK(to == (uint32_t)((10 * st) >> 16) + 64);
    c.k = 11;
    c.next = (uint32_t)((12 * st) >> 16) + 100;
    nr_fs_window(&c, 1, st, 64, &from, &to);
    CHECK(from == to);
    c.k = 13;
    c.next = 5;
    nr_fs_window(&c, 1, st, 64, &from, &to);
    CHECK(from == (uint32_t)((14 * st) >> 16));
    for (i = 0; i < 3 * 5512 / 64 + 4 && c.on; i++)
        nr_fs_window(&c, 0, st, 64, &from, &to);
    CHECK(!c.on && i > 3 * 5512 / 64 - 2);
    CHECK(nr_fs_window(&c, 1, st, 64, &from, &to) == 1 && from == 0 && c.k == 0);

    printf(fails ? "nr_fs: %d FAILED\n" : "nr_fs: all passed (%d)\n", fails ? fails : checks);
    return fails != 0;
}
