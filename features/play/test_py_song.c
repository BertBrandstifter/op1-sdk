/* host test for py_song.c (tools/test_all.py builds it with its sibling py_song.c).
 *
 * Per-track playheads through song rows and chains: chain rows in order with their transpose,
 * an empty chain row ends the chain, unplayable cells are skipped, an empty song cell loops back
 * to the start row, AUDIO tracks (ON = the file's length, OF = a bar), tracks drifting apart with
 * different lengths, seeking, new / clone chain, save / load with CRC; for the views: a chain
 * played alone, chain users, the list's quick-edit rule, song / chain row insert and delete,
 * AUDIO segments from a start row. */
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

/* what the song / chain views use: a chain alone, users, the list's quick rule, rows, AUDIO segments */
static void test_views(void)
{
    uint8_t seg[PS_ROWS];
    ps_head h;
    int r;

    py_song_clear(&s);
    s.trk[0].type = PS_SYNTH;
    s.trk[1].type = PS_DRUM;
    s.trk[2].type = PS_AUDIO;

    /* chain 5 alone: rows 0 (pat 1), 1 (pat 0, -12), then again; the song (empty) does not matter */
    s.chain[5][0].pat = 1;
    s.chain[5][1].pat = 0;
    s.chain[5][1].tsp = -12;
    CHECK(py_song_start_chain(&s, 0, 5, 100, BAR, lens, 0, &h) && at(&h, 0, 0, 1, 0, 100, 500) && h.solo == 5);
    py_song_next(&s, 0, BAR, lens, 0, &h);
    CHECK(at(&h, 0, 1, 0, -12, 600, 250));
    py_song_next(&s, 0, BAR, lens, 0, &h);
    CHECK(at(&h, 0, 0, 1, 0, 850, 500));
    CHECK(py_song_seek(&s, 0, 100 + 3 * 750 + 600, BAR, lens, 0, &h) && at(&h, 0, 1, 0, -12, 2850, 250));
    s.chain[5][0].pat = PS_EMPTY;                                       /* emptied while it plays: it ends */
    py_song_next(&s, 0, BAR, lens, 0, &h);
    CHECK(!h.active);
    CHECK(!py_song_start_chain(&s, 0, 6, 0, BAR, lens, 0, &h));         /* an empty chain */
    CHECK(!py_song_start(&s, 0, 0, 0, BAR, lens, 0, &h) && h.solo == PS_EMPTY);
    s.chain[5][0].pat = 1;

    /* users and the quick rule */
    CHECK(py_song_quick(&s, 0) && !py_song_col_used(&s, 0));            /* an empty column */
    s.cell[0][0] = 7;
    s.chain[7][0].pat = 2;
    CHECK(py_song_quick(&s, 0) && py_song_chain_users(&s, 7) == 1 && py_song_col_used(&s, 0));
    s.cell[3][1] = 7;                                                   /* shared with track 1 */
    CHECK(!py_song_quick(&s, 0) && py_song_chain_users(&s, 7) == 3);
    s.cell[3][1] = PS_EMPTY;
    s.chain[7][2].pat = 4;                                              /* a row behind a gap: kept safe */
    CHECK(!py_song_quick(&s, 0));
    s.chain[7][2].pat = PS_EMPTY;
    s.cell[1][0] = 7;                                                   /* a second song row */
    CHECK(!py_song_quick(&s, 0));
    s.cell[1][0] = PS_EMPTY;
    s.cell[0][2] = 7;                                                   /* an AUDIO cell is no chain user */
    CHECK(py_song_quick(&s, 0) && py_song_chain_users(&s, 7) == 1);
    s.cell[0][2] = PS_EMPTY;

    /* song rows: insert at 0, refused while row 7F is used, delete */
    s.cell[1][1] = 9;
    CHECK(py_song_row_insert(&s, 0) == 0 && s.cell[0][0] == PS_EMPTY && s.cell[1][0] == 7
          && s.cell[2][1] == 9 && s.cell[1][1] == PS_EMPTY);
    s.cell[PS_ROWS - 1][1] = 3;
    CHECK(py_song_row_insert(&s, 0) < 0 && s.cell[1][0] == 7);
    py_song_row_delete(&s, 0);
    CHECK(s.cell[0][0] == 7 && s.cell[1][1] == 9 && s.cell[PS_ROWS - 2][1] == 3
          && s.cell[PS_ROWS - 1][1] == PS_EMPTY);
    py_song_row_delete(&s, PS_ROWS - 2);
    CHECK(s.cell[PS_ROWS - 2][1] == PS_EMPTY && s.cell[1][1] == 9);

    /* chain rows */
    CHECK(py_song_crow_insert(&s, 5, 1) == 0 && s.chain[5][0].pat == 1 && s.chain[5][1].pat == PS_EMPTY
          && s.chain[5][1].tsp == 0 && s.chain[5][2].pat == 0 && s.chain[5][2].tsp == -12);
    py_song_crow_delete(&s, 5, 1);
    CHECK(s.chain[5][1].pat == 0 && s.chain[5][1].tsp == -12 && s.chain[5][2].pat == PS_EMPTY);
    s.chain[5][PS_CROWS - 1].pat = 3;
    CHECK(py_song_crow_insert(&s, 5, 0) < 0 && s.chain[5][0].pat == 1);
    py_song_crow_delete(&s, 5, 0);
    CHECK(s.chain[5][0].pat == 0 && s.chain[5][PS_CROWS - 2].pat == 3 && s.chain[5][PS_CROWS - 1].pat == PS_EMPTY);

    /* AUDIO segments: no cell = free; else the cells from the start row to the first empty one */
    CHECK(py_song_audio_segs(&s, 2, 0, seg) == -1);
    s.cell[0][2] = PS_ON;
    s.cell[1][2] = PS_OF;
    s.cell[2][2] = PS_ON;
    s.cell[4][2] = PS_ON;
    CHECK(py_song_audio_segs(&s, 2, 0, seg) == 3 && seg[0] == PS_ON && seg[1] == PS_OF && seg[2] == PS_ON);
    CHECK(py_song_audio_segs(&s, 2, 1, seg) == 2 && seg[0] == PS_OF && seg[1] == PS_ON);
    CHECK(py_song_audio_segs(&s, 2, 3, seg) == 0);                      /* cells, none at the start row */
    for (r = 0; r < PS_ROWS; r++)
        s.cell[r][2] = PS_ON;
    CHECK(py_song_audio_segs(&s, 2, 0, seg) == PS_ROWS && py_song_audio_segs(&s, 2, 127, seg) == 1);
}

/* live cues: the column jumps to the cued row when its current chain ends (not the pattern), and
 * loops back there from then on; a cue to an empty cell stops the column */
static void test_cue(void)
{
    ps_head h;

    py_song_clear(&s);
    s.trk[0].type = PS_SYNTH;
    s.cell[0][0] = 1;                                                   /* chain 1: pat 0, pat 1 */
    s.cell[1][0] = 2;                                                   /* chain 2: pat 2 */
    s.cell[5][0] = 3;                                                   /* chain 3: pat 3; row 6 empty */
    s.chain[1][0].pat = 0;
    s.chain[1][1].pat = 1;
    s.chain[2][0].pat = 2;
    s.chain[3][0].pat = 3;
    CHECK(py_song_start(&s, 0, 0, 0, BAR, lens, 0, &h) && h.cue == PS_EMPTY);
    h.cue = 5;
    py_song_next(&s, 0, BAR, lens, 0, &h);                              /* still inside chain 1 */
    CHECK(at(&h, 0, 1, 1, 0, 250, 500) && h.cue == 5);
    py_song_next(&s, 0, BAR, lens, 0, &h);                              /* the chain ended: row 5 */
    CHECK(at(&h, 5, 0, 3, 0, 750, 1000) && h.cue == PS_EMPTY && h.start_row == 5);
    py_song_next(&s, 0, BAR, lens, 0, &h);                              /* row 6 empty: back to 5 */
    CHECK(at(&h, 5, 0, 3, 0, 1750, 1000));
    h.cue = 9;                                                          /* an empty cell: stop */
    py_song_next(&s, 0, BAR, lens, 0, &h);
    CHECK(!h.active && h.start_row == 9);
}

/* a rest cell in a pattern column: one bar without a pattern, then the column goes on */
static void test_rest(void)
{
    ps_head h;

    py_song_clear(&s);
    s.trk[0].type = PS_DRUM;
    s.cell[0][0] = 1;
    s.cell[1][0] = PS_REST;
    s.cell[2][0] = PS_REST;
    s.cell[3][0] = 1;
    s.chain[1][0].pat = 0;
    CHECK(py_song_start(&s, 0, 0, 0, BAR, lens, 0, &h) && at(&h, 0, 0, 0, 0, 0, 250));
    py_song_next(&s, 0, BAR, lens, 0, &h);
    CHECK(at(&h, 1, 0, PS_EMPTY, 0, 250, BAR));
    py_song_next(&s, 0, BAR, lens, 0, &h);
    CHECK(at(&h, 2, 0, PS_EMPTY, 0, 1250, BAR));
    py_song_next(&s, 0, BAR, lens, 0, &h);
    CHECK(at(&h, 3, 0, 0, 0, 2250, 250));
    py_song_next(&s, 0, BAR, lens, 0, &h);                              /* row 4 empty: back to row 0 */
    CHECK(at(&h, 0, 0, 0, 0, 2500, 250));
    CHECK(py_song_start(&s, 0, 1, 0, BAR, lens, 0, &h) && at(&h, 1, 0, PS_EMPTY, 0, 0, BAR));   /* starts on a rest */
    CHECK(!py_song_quick(&s, 0) && py_song_col_used(&s, 0));
    h.cue = 1;                                                          /* live: cued to a rest */
    py_song_next(&s, 0, BAR, lens, 0, &h);
    CHECK(at(&h, 1, 0, PS_EMPTY, 0, BAR, BAR) && h.start_row == 1);
}

/* a chain row's sound reaches the head; a "PSN2" file (chain rows without a sound) is still read */
static void test_sound_v2(void)
{
    static uint8_t buf[PS_FILE_BYTES], old[PS_FILE_BYTES_V2];
    static ps_song t;
    ps_head h;
    uint32_t i, c = 0xffffffffu, body = PS_FILE_BYTES_V2 - 8;
    int k;

    py_song_clear(&s);
    s.trk[0].type = PS_SYNTH;
    s.trk[0].sound = 4;
    s.trk[0].level = -20;
    s.cell[0][0] = 3;
    s.cell[1][0] = PS_REST;
    s.chain[3][0].pat = 2;
    s.chain[3][0].tsp = -7;
    s.chain[3][0].snd = 6;
    s.chain[3][1].pat = 5;
    CHECK(s.chain[3][1].snd == PS_EMPTY && sizeof(ps_crow) == 3);
    CHECK(py_song_start(&s, 0, 0, 0, BAR, lens, 0, &h) && h.snd == 6);
    py_song_next(&s, 0, BAR, lens, 0, &h);
    CHECK(h.pat == 5 && h.snd == PS_EMPTY);
    py_song_next(&s, 0, BAR, lens, 0, &h);                              /* the rest */
    CHECK(h.pat == PS_EMPTY && h.snd == PS_EMPTY);
    CHECK(py_song_start_chain(&s, 0, 3, 0, BAR, lens, 0, &h) && h.snd == 6);
    CHECK(py_song_crow_insert(&s, 3, 0) == 0 && s.chain[3][0].snd == PS_EMPTY && s.chain[3][1].snd == 6);
    py_song_crow_delete(&s, 3, 0);
    CHECK(s.chain[3][0].snd == 6 && s.chain[3][PS_CROWS - 1].snd == PS_EMPTY);
    py_song_save(&s, buf);
    CHECK(buf[3] == '3' && py_song_load(&t, buf, PS_FILE_BYTES) == 0 && !memcmp(&s, &t, sizeof(s)));

    /* the same song as v0.6 wrote it: cells, { pat, tsp } rows, tracks; CRC over that */
    old[0] = 'P'; old[1] = 'S'; old[2] = 'N'; old[3] = '2';
    memcpy(old + 8, s.cell, sizeof(s.cell));
    for (i = 0; i < PS_CHAINS * PS_CROWS; i++) {
        old[8 + sizeof(s.cell) + 2 * i] = s.chain[i / PS_CROWS][i % PS_CROWS].pat;
        old[8 + sizeof(s.cell) + 2 * i + 1] = (uint8_t)s.chain[i / PS_CROWS][i % PS_CROWS].tsp;
    }
    memcpy(old + 8 + sizeof(s.cell) + 2 * PS_CHAINS * PS_CROWS, s.trk, sizeof(s.trk));
    for (i = 0; i < body; i++) {
        c ^= old[8 + i];
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    c = ~c;
    old[4] = (uint8_t)c; old[5] = (uint8_t)(c >> 8); old[6] = (uint8_t)(c >> 16); old[7] = (uint8_t)(c >> 24);
    memset(&t, 0x55, sizeof(t));
    CHECK(py_song_load(&t, old, PS_FILE_BYTES_V2) == 0);
    CHECK(t.cell[0][0] == 3 && t.cell[1][0] == PS_REST && t.chain[3][0].pat == 2 && t.chain[3][0].tsp == -7
          && t.chain[3][0].snd == PS_EMPTY && t.chain[3][1].pat == 5 && t.chain[100][7].pat == PS_EMPTY
          && t.chain[100][7].snd == PS_EMPTY && t.trk[0].type == PS_SYNTH && t.trk[0].sound == 4
          && t.trk[0].level == -20 && t.trk[7].type == PS_NONE);
    old[40] ^= 1;
    CHECK(py_song_load(&t, old, PS_FILE_BYTES_V2) < 0);                 /* CRC */
    CHECK(py_song_load(&t, buf, PS_FILE_BYTES_V2) < 0);                 /* a PSN3 file cut short */
}

int main(void)
{
    test_walk();
    test_a_tracks();
    test_edit_store();
    test_views();
    test_cue();
    test_rest();
    test_sound_v2();
    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
