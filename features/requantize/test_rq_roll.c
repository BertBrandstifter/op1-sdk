/* Host tests for the piano roll core (plain cc; tools/test_all.py builds it with rq_roll.c, the
 * requantize and quantizer cores are included below as single-file host builds). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rq_roll.h"
#include "rq_core.c"
#include "../quantcore/quant_core.c"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static qz_state st;
static rq_event in[512], raw[512], tmp[512], out[512], ref[512];
static rq_note nt[256];

static void ev(rq_event *e, uint32_t pos, int kind, int key, int vel)
{
    memset(e, 0, sizeof(*e));
    e->pos = pos;
    e->ev.kind = (uint8_t)kind;
    e->ev.key = (uint8_t)key;
    e->ev.data[0] = (uint8_t)vel;
    e->ev.data[1] = 1;
}

#define STEP ((uint64_t)8000 << 16)             /* grid 8000 samples from tape position 0 */
#define TRIP ((uint64_t)((8000ull << 16) * 2 / 3))  /* fractional step (triplet) */

static int cmp(const void *a, const void *b)
{
    const rq_event *x = a, *y = b;

    if (x->pos != y->pos)
        return x->pos < y->pos ? -1 : 1;
    if (x->ev.kind != y->ev.kind)
        return x->ev.kind < y->ev.kind ? -1 : 1;
    if (x->ev.key != y->ev.key)
        return x->ev.key < y->ev.key ? -1 : 1;
    return memcmp(x->ev.data, y->ev.data, 6);
}

static int same(rq_event *a, int na, rq_event *b, int nb)
{
    int i;

    if (na != nb)
        return 0;
    qsort(a, (size_t)na, sizeof(*a), cmp);
    qsort(b, (size_t)nb, sizeof(*b), cmp);
    for (i = 0; i < na; i++)
        if (cmp(&a[i], &b[i]))
            return 0;
    return 1;
}

/* a pseudo-random take: n notes on distinct keys at a time, chords and runs, lengths 0.1..3 steps */
static int make_take(uint32_t seed, int notes)
{
    uint32_t pos = 30000, r = seed;
    int i, k = 0;

    for (i = 0; i < notes; i++) {
        r = r * 1103515245u + 12345u;
        pos += (r >> 8) % 9000;
        r = r * 1103515245u + 12345u;
        ev(&raw[k++], pos, QZ_ON, 40 + i % 48, 60 + i);
        ev(&raw[k++], pos + 800 + (r >> 8) % 24000, QZ_OFF, 40 + i % 48, 60 + i);
    }
    return k;
}

int main(void)
{
    int n, m, i, mode, seed, ok, ord[256];

    /* build: pairing, orphan note-off dropped, open note ends at `end`, minimum length,
     * the same key from two sources pairs first open first */
    ev(&in[0], 1000, QZ_OFF, 50, 0);            /* pressed before the take */
    ev(&in[1], 2000, QZ_ON, 60, 100);
    ev(&in[2], 2500, QZ_ON, 60, 90);            /* same key again (MIDI) */
    ev(&in[3], 3000, QZ_OFF, 60, 0);            /* closes the first */
    ev(&in[4], 3200, QZ_OFF, 60, 0);            /* closes the second */
    ev(&in[5], 5000, QZ_ON, 64, 80);
    ev(&in[6], 5010, QZ_OFF, 64, 0);            /* too short -> QZ_MIN_LEN */
    ev(&in[7], 6000, QZ_ON, 67, 70);            /* held past the take */
    n = rq_notes_build(in, 8, 9000, nt, 256);
    CHECK(n == 4);
    CHECK(nt[0].played == 2000 && nt[0].played_len == 1000 && nt[0].data[0] == 100);
    CHECK(nt[1].played == 2500 && nt[1].played_len == 700 && nt[1].data[0] == 90);
    CHECK(nt[2].played_len == QZ_MIN_LEN && nt[3].played_len == 3000);
    CHECK(nt[3].key == 67 && nt[3].key0 == 67 && nt[3].flags == 0 && nt[3].start == 6000);
    CHECK(rq_notes_build(in, 8, 9000, nt, 2) == 2);

    /* render of an unedited take == v2.5 (rq_quantize on the raw log), every mode, several takes */
    ok = 1;
    for (mode = RQ_SNAP; mode <= RQ_STRICT; mode++)
        for (seed = 1; seed < 20; seed++) {
            int k = make_take((uint32_t)seed, 60);

            memcpy(in, raw, sizeof(raw[0]) * (size_t)k);
            m = rq_quantize(in, k, ref, 512, &st, seed & 1 ? STEP : TRIP, mode, 1000, 1500);
            memcpy(in, raw, sizeof(raw[0]) * (size_t)k);
            n = rq_notes_build(in, k, 2000000, nt, 256);
            i = rq_render(nt, n, tmp, out, 512, &st, seed & 1 ? STEP : TRIP, mode, 1000, 1500);
            ok &= same(out, i, ref, m);
        }
    CHECK(ok);

    /* the render's output: sorted, note-off first at equal positions */
    ev(&in[0], 16100, QZ_ON, 60, 1);
    ev(&in[1], 23000, QZ_OFF, 60, 1);
    ev(&in[2], 24000, QZ_ON, 60, 1);
    ev(&in[3], 26000, QZ_OFF, 60, 1);
    n = rq_notes_build(in, 4, 0, nt, 256);
    rq_edit(nt, n, 0, RE_LEN, 1100, STEP);      /* FIXED 16100..24100 */
    rq_edit(nt, n, 0, RE_MOVE, -100, STEP);     /* 16000..24000 */
    m = rq_render(nt, n, tmp, out, 512, &st, STEP, RQ_SNAP, 1000, 500);
    CHECK(m == 4 && out[1].pos == 24000 && out[1].ev.kind == QZ_OFF && out[2].pos == 24000
          && out[2].ev.kind == QZ_ON);

    /* FIXED notes stay out of the quantize pass: QUEUE orders the AUTO notes around them */
    ev(&in[0], 16100, QZ_ON, 60, 1);            /* -> 16000 slot */
    ev(&in[1], 17000, QZ_ON, 62, 1);            /* -> 24000 */
    ev(&in[2], 18000, QZ_ON, 64, 1);            /* -> 32000; with 62 FIXED: 24000 */
    ev(&in[3], 19000, QZ_OFF, 60, 1);
    ev(&in[4], 19000, QZ_OFF, 62, 1);
    ev(&in[5], 19000, QZ_OFF, 64, 1);
    n = rq_notes_build(in, 6, 0, nt, 256);
    rq_render(nt, n, tmp, out, 512, &st, STEP, RQ_QUEUE, 1000, 500);
    CHECK(nt[0].start == 16100 && nt[1].start == 24000 && nt[2].start == 32000);
    CHECK(nt[2].len == 1000);                   /* held length kept */
    CHECK(rq_edit(nt, n, 1, RE_PLAYED, 0, STEP) && (nt[1].flags & RN_FIXED) && nt[1].start == 17000);
    m = rq_render(nt, n, tmp, out, 512, &st, STEP, RQ_QUEUE, 1000, 500);
    CHECK(m == 6 && nt[1].start == 17000 && nt[2].start == 24000);
    CHECK(rq_edit(nt, n, 1, RE_PLAYED, 0, STEP) && !(nt[1].flags & RN_FIXED));  /* back to AUTO */
    rq_render(nt, n, tmp, out, 512, &st, STEP, RQ_QUEUE, 1000, 500);
    CHECK(nt[1].start == 24000 && nt[2].start == 32000);

    /* all back to AUTO: the whole take quantized again; pitch and delete stay; no-op when all AUTO */
    rq_edit(nt, n, 0, RE_MOVE, 300, STEP);
    rq_edit(nt, n, 2, RE_PLAYED, 0, STEP);
    rq_edit(nt, n, 2, RE_PITCH, 1, STEP);
    CHECK(rq_edit(nt, n, 0, RE_AUTO_ALL, 0, STEP) && !(nt[0].flags & RN_FIXED)
          && !(nt[2].flags & RN_FIXED) && nt[2].key == 65);
    rq_render(nt, n, tmp, out, 512, &st, STEP, RQ_QUEUE, 1000, 500);
    CHECK(nt[0].start == 16100 && nt[1].start == 24000 && nt[2].start == 32000);
    CHECK(!rq_edit(nt, n, 0, RE_AUTO_ALL, 0, STEP));
    rq_edit(nt, n, 2, RE_PITCH, -1, STEP);

    /* delete: not played, out of the pass; restore */
    CHECK(rq_edit(nt, n, 1, RE_DELETE, 0, STEP) && (nt[1].flags & RN_DEL));
    m = rq_render(nt, n, tmp, out, 512, &st, STEP, RQ_QUEUE, 1000, 500);
    ok = m == 4;
    for (i = 0; i < m; i++)
        ok &= out[i].ev.key != 62;
    CHECK(ok && nt[2].start == 24000);
    rq_edit(nt, n, 1, RE_DELETE, 0, STEP);
    CHECK(!(nt[1].flags & RN_DEL));

    /* pitch: played with the new key, quantized with the played one; clamped */
    rq_render(nt, n, tmp, out, 512, &st, STEP, RQ_QUEUE, 1000, 500);
    rq_edit(nt, n, 0, RE_PITCH, 12, STEP);
    m = rq_render(nt, n, tmp, out, 512, &st, STEP, RQ_QUEUE, 1000, 500);
    CHECK(nt[0].key == 72 && nt[0].key0 == 60 && !(nt[0].flags & RN_FIXED) && out[0].ev.key == 72);
    rq_edit(nt, n, 0, RE_PITCH, 100, STEP);
    CHECK(nt[0].key == 127);
    rq_edit(nt, n, 0, RE_PITCH, -200, STEP);
    CHECK(nt[0].key == 0);

    /* moves and lengths: fine, by grid step (grid points stay grid points, also triplets),
     * off-grid keeps its offset, clamp at 0, minimum length */
    n = rq_notes_build((ev(&in[0], 16000, QZ_ON, 60, 1), ev(&in[1], 20000, QZ_OFF, 60, 1), in),
                       2, 0, nt, 256);
    rq_edit(nt, n, 0, RE_MOVE, RN_FINE, STEP);
    CHECK(nt[0].start == 16000 + RN_FINE && (nt[0].flags & RN_FIXED));
    rq_edit(nt, n, 0, RE_MOVE_STEP, 1, STEP);
    CHECK(nt[0].start == 24000 + RN_FINE);      /* offset kept */
    rq_edit(nt, n, 0, RE_SNAP, 0, STEP);
    CHECK(nt[0].start == 24000);
    rq_edit(nt, n, 0, RE_MOVE, 3999, STEP);
    rq_edit(nt, n, 0, RE_SNAP, 0, STEP);
    CHECK(nt[0].start == 24000);                /* 3999 past: nearest is still 24000 */
    rq_edit(nt, n, 0, RE_MOVE, 4001, STEP);
    rq_edit(nt, n, 0, RE_SNAP, 0, STEP);
    CHECK(nt[0].start == 32000);
    rq_edit(nt, n, 0, RE_MOVE_STEP, -10, STEP);
    CHECK(nt[0].start == 0);
    rq_edit(nt, n, 0, RE_MOVE, -5, STEP);
    CHECK(nt[0].start == 0);
    ok = 1;
    rq_edit(nt, n, 0, RE_MOVE, 7, TRIP);
    rq_edit(nt, n, 0, RE_SNAP, 0, TRIP);
    for (i = 0; i < 40; i++) {
        uint32_t k = rq_grid_floor(TRIP, nt[0].start);
        rq_edit(nt, n, 0, RE_MOVE_STEP, 1, TRIP);
        ok &= nt[0].start == rq_grid_pt(TRIP, k + 1);
    }
    for (i = 0; i < 40; i++)
        rq_edit(nt, n, 0, RE_MOVE_STEP, -1, TRIP);
    CHECK(ok && nt[0].start == 0);
    nt[0].len = 4000;
    rq_edit(nt, n, 0, RE_LEN_STEP, 1, STEP);    /* end 4000 off grid -> 12000 */
    CHECK(nt[0].len == 12000);
    rq_edit(nt, n, 0, RE_LEN, -4000, STEP);     /* end 8000 = grid point */
    rq_edit(nt, n, 0, RE_LEN_STEP, 1, STEP);
    CHECK(nt[0].len == 16000);
    rq_edit(nt, n, 0, RE_LEN_STEP, -5, STEP);
    CHECK(nt[0].len == QZ_MIN_LEN);
    rq_edit(nt, n, 0, RE_LEN, -1000, STEP);
    CHECK(nt[0].len == QZ_MIN_LEN);
    CHECK(!rq_edit(nt, n, 0, RE_SNAP, 0, 0) && !rq_edit(nt, n, 5, RE_MOVE, 1, STEP));

    /* selection: edits act on the selected notes, else on the cursor; select all / clear */
    for (i = 0; i < 4; i++) {
        ev(&in[2 * i], 10000 + 8000 * (uint32_t)i, QZ_ON, 60 + i, 1);
        ev(&in[2 * i + 1], 14000 + 8000 * (uint32_t)i, QZ_OFF, 60 + i, 1);
    }
    n = rq_notes_build(in, 8, 0, nt, 256);
    rq_edit(nt, n, 1, RE_SELECT, 0, STEP);
    rq_edit(nt, n, 3, RE_SELECT, 0, STEP);
    rq_edit(nt, n, 0, RE_PITCH, 1, STEP);       /* cursor 0 is not selected: selection wins */
    CHECK(nt[0].key == 60 && nt[1].key == 62 && nt[2].key == 62 && nt[3].key == 64);
    rq_edit(nt, n, 0, RE_DELETE, 0, STEP);
    CHECK((nt[1].flags & RN_DEL) && (nt[3].flags & RN_DEL) && !(nt[0].flags & RN_DEL));
    rq_edit(nt, n, 0, RE_SELECT_ALL, 0, STEP);  /* some selected -> clear */
    ok = 1;
    for (i = 0; i < n; i++)
        ok &= !(nt[i].flags & RN_SEL);
    rq_edit(nt, n, 0, RE_SELECT_ALL, 0, STEP);  /* none selected -> all */
    for (i = 0; i < n; i++)
        ok &= (nt[i].flags & RN_SEL) != 0;
    CHECK(ok);
    rq_edit(nt, n, 0, RE_DELETE, 0, STEP);      /* mixed -> all deleted */
    ok = 1;
    for (i = 0; i < n; i++)
        ok &= (nt[i].flags & RN_DEL) != 0;
    CHECK(ok && rq_render(nt, n, tmp, out, 512, &st, STEP, RQ_SNAP, 1000, 500) == 0);

    /* order: by start, then pitch */
    nt[0].start = 50000;
    nt[2].start = nt[1].start;
    nt[2].key = 50;
    rq_order(nt, n, ord);
    CHECK(ord[0] == 2 && ord[1] == 1 && ord[2] == 3 && ord[3] == 0);

    printf("%d/%d checks passed\n%s\n", checks - fails, checks, fails ? "FAIL" : "PASS");
    return fails != 0;
}
