/* host test for py_song.c (tools/test_all.py builds it with its sibling py_song.c).
 *
 * Per-track playheads through song rows and chains: chain rows in order with their transpose,
 * an empty chain row ends the chain, unplayable cells are skipped, an empty song cell loops back
 * to the start row, AUDIO tracks (ON = the file's length, OF = a bar), tracks drifting apart with
 * different lengths, seeking, new / clone chain, save / load with CRC. */
#include <stdio.h>
#include <string.h>
#include "py_song.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define BAR 1000u

/* pattern p is (p + 1) x 250 frames; A track t's loop: 3 bars (A5 = track 7: none) */
static uint32_t lens(void *ctx, int t, int p)
{
    (void)ctx;
    if (t < 4)
        return p == 13 ? 0 : (uint32_t)(p + 1) * 250u;
    return t == 7 ? 0 : 3 * BAR;
}

static int at(const ps_head *h, int row, int crow, int pat, int tsp, uint32_t start, uint32_t len)
{
    int ok = h->active && h->row == row && h->crow == crow && h->pat == pat && h->tsp == tsp
             && h->start == start && h->len == len;

    if (!ok)
        printf("  head: active %d row %d crow %d pat %d tsp %d start %u len %u\n", h->active, h->row,
               h->crow, h->pat, h->tsp, h->start, h->len);
    return ok;
}

static ps_song s;

static void test_walk(void)
{
    ps_head h;

    py_song_clear(&s);
    s.trk[0].type = PS_SYNTH;
    s.trk[1].type = PS_DRUM;
    CHECK(!py_song_start(&s, 0, 0, 0, BAR, lens, 0, &h));                    /* empty song */

    /* P1: row 0 chain 1 (pat 0, pat 0 +5), row 1 chain 2 (pat 3, then an empty row ends it
     * even though row 2 holds pat 1), row 2 chain 9 (no rows: skipped), row 3 empty */
    s.cell[0][0] = 1;
    s.cell[1][0] = 2;
    s.cell[2][0] = 9;
    s.chain[1][0].pat = 0;
    s.chain[1][1].pat = 0;
    s.chain[1][1].tsp = 5;
    s.chain[2][0].pat = 3;
    s.chain[2][2].pat = 1;
    CHECK(py_song_start(&s, 0, 0, 0, BAR, lens, 0, &h) && at(&h, 0, 0, 0, 0, 0, 250));
    py_song_next(&s, 0, BAR, lens, 0, &h);
    CHECK(at(&h, 0, 1, 0, 5, 250, 250));
    py_song_next(&s, 0, BAR, lens, 0, &h);
    CHECK(at(&h, 1, 0, 3, 0, 500, 1000));
    py_song_next(&s, 0, BAR, lens, 0, &h);                                   /* row 2 skipped, row 3 empty */
    CHECK(at(&h, 0, 0, 0, 0, 1500, 250));                              /* back to the start row */

    /* starting at row 1: the loop goes back to row 1, not 0 */
    CHECK(py_song_start(&s, 0, 1, 7000, BAR, lens, 0, &h) && at(&h, 1, 0, 3, 0, 7000, 1000));
    py_song_next(&s, 0, BAR, lens, 0, &h);
    CHECK(at(&h, 1, 0, 3, 0, 8000, 1000));

    /* starting on an empty cell (row 3) with nothing below: nothing plays */
    CHECK(!py_song_start(&s, 0, 3, 0, BAR, lens, 0, &h));

    /* an empty pattern (13: length 0) still takes a bar */
    s.cell[5][1] = 4;
    s.chain[4][0].pat = 13;
    CHECK(py_song_start(&s, 1, 5, 0, BAR, lens, 0, &h) && at(&h, 5, 0, 13, 0, 0, BAR));

    /* seek: a P1 cycle is 1500 frames; 9999 = 6 x 1500 + 999 -> row 1 (pat 3) from 9500 */
    CHECK(py_song_start(&s, 0, 0, 0, BAR, lens, 0, &h) && py_song_seek(&s, 0, 9999, BAR, lens, 0, &h));
    CHECK(at(&h, 1, 0, 3, 0, 9500, 1000));
}

static void test_a_tracks(void)
{
    ps_head a, b;

    py_song_clear(&s);
    s.trk[4].type = PS_AUDIO;
    s.trk[7].type = PS_AUDIO;
    s.trk[0].type = PS_SYNTH;
    s.trk[1].type = PS_SYNTH;
    /* audio track 4: ON, OF, ON, empty; audio track 7 (no file): ON = a silent bar */
    s.cell[0][4] = PS_ON;
    s.cell[1][4] = PS_OF;
    s.cell[2][4] = PS_ON;
    s.cell[0][7] = PS_ON;
    CHECK(py_song_start(&s, 4, 0, 0, BAR, lens, 0, &a) && at(&a, 0, 0, PS_ON, 0, 0, 3000));
    py_song_next(&s, 4, BAR, lens, 0, &a);
    CHECK(at(&a, 1, 0, PS_OF, 0, 3000, BAR));
    py_song_next(&s, 4, BAR, lens, 0, &a);
    CHECK(at(&a, 2, 0, PS_ON, 0, 4000, 3000));
    py_song_next(&s, 4, BAR, lens, 0, &a);
    CHECK(at(&a, 0, 0, PS_ON, 0, 7000, 3000));
    CHECK(py_song_start(&s, 7, 0, 0, BAR, lens, 0, &b) && at(&b, 0, 0, PS_ON, 0, 0, BAR));

    /* polymeter: P1 plays a 250-frame pattern, P2 a 500-frame one, both loop row 0 */
    s.cell[0][0] = 0;
    s.cell[0][1] = 1;
    s.chain[0][0].pat = 0;
    s.chain[1][0].pat = 1;
    CHECK(py_song_start(&s, 0, 0, 0, BAR, lens, 0, &a) && py_song_start(&s, 1, 0, 0, BAR, lens, 0, &b));
    py_song_seek(&s, 0, 1700, BAR, lens, 0, &a);
    py_song_seek(&s, 1, 1700, BAR, lens, 0, &b);
    CHECK(a.start == 1500 && b.start == 1500);
    py_song_seek(&s, 0, 1800, BAR, lens, 0, &a);
    py_song_seek(&s, 1, 1800, BAR, lens, 0, &b);
    CHECK(a.start == 1750 && b.start == 1500);
}

static void test_edit_store(void)
{
    static uint8_t buf[PS_FILE_BYTES];
    static ps_song t;
    int c;

    py_song_clear(&s);
    s.trk[0].type = PS_DRUM;
    CHECK(py_song_new_chain(&s) == 0);
    s.cell[0][0] = 0;                                                   /* used, still empty */
    CHECK(py_song_new_chain(&s) == 1);
    s.chain[1][0].pat = 2;                                              /* holds rows, unused */
    CHECK(py_song_new_chain(&s) == 2);
    s.chain[1][1].pat = 5;
    s.chain[1][1].tsp = -12;
    c = py_song_clone_chain(&s, 1);
    CHECK(c == 2 && s.chain[2][1].pat == 5 && s.chain[2][1].tsp == -12 && py_song_new_chain(&s) == 3);
    CHECK(py_song_chain_used(&s, 0) && !py_song_chain_used(&s, 1) && py_song_chain_empty(&s, 0) && !py_song_chain_empty(&s, 1));

    py_song_save(&s, buf);
    CHECK(py_song_load(&t, buf, PS_FILE_BYTES) == 0 && !memcmp(&s, &t, sizeof(s)));
    buf[100] ^= 1;
    CHECK(py_song_load(&t, buf, PS_FILE_BYTES) < 0);
    CHECK(py_song_load(&t, buf, PS_FILE_BYTES - 1) < 0);
}

int main(void)
{
    test_walk();
    test_a_tracks();
    test_edit_store();
    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
