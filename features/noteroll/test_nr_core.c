/* Host tests for the NOTE ROLL core (plain cc, test_nr_core.c + nr_core.c). */
#include <stdio.h>
#include <string.h>
#include "nr_core.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static nr_hit got[256];
static int ngot;

static void collect(void *ctx, const nr_hit *h)
{
    (void)ctx;
    if (ngot < 256)
        got[ngot++] = *h;
}

static void step_tests(void)
{
    nr_step s;
    int v;

    nr_step_init(&s);
    CHECK(nr_voices(&s) == 0 && s.prob == 100 && s.ratch == 1);
    CHECK(nr_add(&s, 64, 80, 2) == 0 && nr_add(&s, 60, 100, NR_LEN_DEF) == 0 && nr_add(&s, 67, 90, 4) == 2);
    CHECK(s.n[0].key == 60 && s.n[1].key == 64 && s.n[1].vel == 80 && s.n[1].len == 2 && nr_voices(&s) == 3);
    CHECK(nr_add(&s, 64, 100, 4) == -1 && nr_add(&s, 128, 100, 4) == -1);     /* there / no MIDI note */
    v = nr_set_key(&s, 0, 70);                  /* 60 -> 70: sorts to the top */
    CHECK(v == 2 && s.n[2].key == 70 && s.n[0].key == 64);
    CHECK(nr_set_key(&s, 2, 67) == 2 && s.n[2].key == 70);      /* 67 is taken: unchanged */
    CHECK(nr_set_key(&s, 0, -5) == 0 && s.n[0].key == 0);
    nr_remove(&s, 0);
    CHECK(nr_voices(&s) == 2 && s.n[0].key == 67 && s.n[2].key == NR_NONE);
    for (v = 0; v < 10; v++)
        nr_add(&s, 40 + v, 100, 4);
    CHECK(nr_voices(&s) == NR_VOICES);          /* full */
}

static void chord_tests(void)
{
    nr_step s;

    nr_step_init(&s);
    nr_add(&s, 60, 70, 6);
    CHECK(nr_shape_of(&s) == 0);
    CHECK(nr_chord(&s, 1) == 1 && nr_voices(&s) == 3 && s.n[1].key == 64 && s.n[2].key == 67
          && s.n[2].vel == 70 && s.n[2].len == 6 && nr_shape_of(&s) == 1);
    CHECK(nr_chord(&s, 1) == 0);                /* already */
    CHECK(nr_chord(&s, 7) == 1 && s.n[1].key == 63 && s.n[3].key == 70 && nr_shape_of(&s) == 7);
    CHECK(nr_invert(&s, 1) == 1 && s.n[0].key == 63 && s.n[3].key == 72 && nr_shape_of(&s) == -1);
    CHECK(nr_invert(&s, 0) == 1 && s.n[0].key == 60 && nr_shape_of(&s) == 7);
    CHECK(nr_chord(&s, 0) == 1 && nr_voices(&s) == 1 && s.n[0].key == 60);
    nr_step_init(&s);
    CHECK(nr_chord(&s, 1) == 0 && nr_invert(&s, 1) == 0);      /* empty */
    nr_add(&s, 125, 100, 4);
    nr_chord(&s, 1);
    CHECK(nr_voices(&s) == 1);                  /* 129, 132: no MIDI notes */
}

static void schedule_tests(void)
{
    nr_pattern p;
    uint64_t L = nr_step_q16(12000);            /* 5512.5 samples */
    int i, ok;

    CHECK(L == (uint64_t)(5512.5 * 65536));
    nr_pattern_init(&p);
    nr_add(&p.s[0], 60, 100, 4);
    nr_add(&p.s[0], 64, 100, 8);                /* 4 steps */
    nr_add(&p.s[4], 67, 50, 2);                 /* half a step */
    p.s[8].ratch = 2;
    nr_add(&p.s[8], 72, 100, 4);
    p.s[12].nudge = 50;
    nr_add(&p.s[12], 48, 100, NR_LENS - 1);     /* TIE: the pattern's length */
    ngot = 0;
    CHECK(nr_schedule(&p, 0, 88200, L, 0, 1, collect, 0) == 6);
    CHECK(got[0].at == 0 && got[0].n->key == 60 && got[0].hold == 5512 && got[1].n->key == 64 && got[1].hold == 22048);
    CHECK(got[2].at == 22050 && got[2].hold == 2756 && got[2].n->vel == 50);
    CHECK(got[3].at == 44100 && got[3].hold == 2756 && got[4].at == 44100 + 2756 && got[4].hit == 1);
    CHECK(got[5].at == 66150 + 2756 && got[5].hold == 5512 * 16 && got[5].n->key == 48);
    /* loops on its length; block by block = all at once */
    p.len = 8;
    ngot = 0;
    for (i = 0; i < 16 * 5513 / 64 + 1; i++)
        nr_schedule(&p, (uint32_t)i * 64, 64, L, 0, 1, collect, 0);
    ok = ngot == 8;                             /* steps 0 (2), 4 (1), 8 = 0 again ..., 16 */
    CHECK(ok && got[3].at == 44100 && got[3].step == 0 && got[7].at == 88200);
    /* swing moves the odd steps; probability 0 never */
    nr_pattern_init(&p);
    nr_add(&p.s[1], 60, 100, 4);
    ngot = 0;
    nr_schedule(&p, 0, 4 * 5513, L, 50, 1, collect, 0);
    CHECK(ngot == 1 && got[0].at == 5512 + 5512 * 50 / 200);
    p.s[1].prob = 0;
    ngot = 0;
    nr_schedule(&p, 0, 4 * 5513, L, 0, 1, collect, 0);
    CHECK(ngot == 0);
    CHECK(nr_playhead(&p, 5513 * 3, L) == 3 && nr_playhead(&p, 5513 * 17, L) == 1 && nr_playhead(&p, 0, 0) == -1);
}

int main(void)
{
    step_tests();
    chord_tests();
    schedule_tests();
    printf(fails ? "nr_core: %d FAILED\n" : "nr_core: all passed (%d)\n", fails ? fails : checks);
    return fails != 0;
}
