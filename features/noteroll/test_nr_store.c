/* Host tests for the NOTE ROLL store (plain cc; nr_core.c is included below). */
#include <stdio.h>
#include <string.h>
#include "nr_store.h"
#include "nr_core.c"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static char row[NR_ROW_TEXT_MAX];

int main(void)
{
    nr_pattern p, q;
    nr_settings s, t;
    int n, i;

    nr_pattern_init(&p);
    p.len = 37;
    nr_add(&p.s[3], 60, 77, 9);
    nr_chord(&p.s[3], 6);
    p.s[3].nudge = -20;
    p.s[3].ratch = 3;
    p.s[40].prob = 30;
    n = nr_store_pattern(5, &p, row);
    CHECK(n == (int)strlen(row) && n == (int)NR_B64_LEN(NR_HDR + sizeof(nr_pattern)) && n < (int)NR_ROW_TEXT_MAX);
    for (i = 0; i < n; i++)
        CHECK(row[i] != '\'' && row[i] != '%');
    memset(&q, 0, sizeof(q));
    CHECK(nr_load_pattern(5, row, &q) == 0 && !memcmp(&p, &q, sizeof(p)));
    CHECK(nr_load_pattern(4, row, &q) == -1);           /* another row's */
    row[10] = '#';
    CHECK(nr_load_pattern(5, row, &q) == -1);           /* damaged */
    CHECK(nr_load_pattern(5, "", &q) == -1 && nr_load_pattern(5, 0, &q) == -1);

    /* out-of-range fields come back in range */
    p.len = 200;
    p.s[0].n[0].key = 150;
    p.s[0].n[1].key = 61; p.s[0].n[1].vel = 0; p.s[0].n[1].len = 99;
    p.s[0].nudge = 120; p.s[0].prob = 250; p.s[0].ratch = 0;
    nr_store_pattern(0, &p, row);
    CHECK(nr_load_pattern(0, row, &q) == 0 && q.len == NR_STEPS && q.s[0].n[0].key == 61 && q.s[0].n[0].vel == 1
          && q.s[0].n[0].len == NR_LEN_DEF && q.s[0].n[1].key == NR_NONE && q.s[0].nudge == 50
          && q.s[0].prob == 100 && q.s[0].ratch == 1);

    memset(&s, 0, sizeof(s));
    s.cur = 9; s.swing = 30; s.zoom = 1; s.owned[0] = 0x81; s.owned[1] = 0x23;
    nr_store_settings(&s, row);
    CHECK(nr_load_settings(row, &t) == 0 && !memcmp(&s, &t, sizeof(s)));
    s.cur = 40; s.swing = 180; s.zoom = 7; s.owned[1] = 0xff;
    nr_store_settings(&s, row);
    CHECK(nr_load_settings(row, &t) == 0 && t.cur == 40 % NR_PATTERNS && t.swing == 100 && t.zoom == 4
          && t.owned[1] == 0x3f);
    CHECK(nr_load_pattern(NR_ROW_SETTINGS, row, &q) == -1);
    CHECK(nr_hash(&p, sizeof(p)) != nr_hash(&q, sizeof(q)));

    printf(fails ? "nr_store: %d FAILED\n" : "nr_store: all passed (%d)\n", fails ? fails : checks);
    return fails != 0;
}
