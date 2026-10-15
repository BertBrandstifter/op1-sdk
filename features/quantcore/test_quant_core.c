/* Host unit tests for quant_core (plain cc, runs on the Mac):
 *     cc -std=c99 -Wall -Wextra -O2 -o /tmp/tq test_quant_core.c quant_core.c && /tmp/tq
 * tools/test_all.py runs this too. */
#include <stdio.h>
#include <string.h>
#include "quant_core.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define SR 44100u

typedef struct { qz_ev ev[64]; uint32_t at[64]; int n; uint32_t now; } log_t;

static void rec(const qz_ev *ev, void *ctx)
{
    log_t *l = ctx;
    l->ev[l->n] = *ev;
    l->at[l->n] = l->now;
    l->n++;
}

static qz_ev ev(int kind, int key)
{
    qz_ev e;
    memset(&e, 0, sizeof(e));
    e.kind = (uint8_t)kind;
    e.key = (uint8_t)key;
    e.data[0] = (uint8_t)(key + 100);
    return e;
}

/* pointer to a fresh event (qz_input copies it) */
static const qz_ev *E(int kind, int key)
{
    static qz_ev ring[8];
    static int i;
    i = (i + 1) & 7;
    ring[i] = ev(kind, key);
    return &ring[i];
}

/* advance time sample by sample (in coarse 64-sample blocks like the device) and poll */
static void run_until(qz_state *s, log_t *l, uint32_t until)
{
    while ((int32_t)(until - l->now) > 0) {
        l->now += 64;
        qz_poll(s, l->now, rec, l);
    }
}

static void test_step(void)
{
    /* 120 BPM: quarter = 22050 samples, 1/16 = 5512.5, 1/8T = 7350 */
    CHECK(qz_step_q16(12000, SR, 1, 1) == (uint64_t)22050 << 16);
    CHECK(qz_step_q16(12000, SR, 1, 4) == ((uint64_t)55125 << 16) / 10);
    CHECK(qz_step_q16(12000, SR, 1, 3) == (uint64_t)7350 << 16);
    CHECK(qz_step_q16(12000, SR, 1, 12) == ((uint64_t)3675 << 16) / 2);   /* 1/32T: 1837.5 */
    CHECK(qz_step_q16(4000, SR, 1, 1) == (uint64_t)66150 << 16);  /* 1/4 at 40 BPM > 16 bits */
    CHECK(qz_step_q16(0, SR, 1, 4) == 0);
}

static void test_grid(void)
{
    qz_state s;
    uint32_t p, n;

    qz_init(&s);
    qz_set_grid(&s, (uint64_t)1000 << 16, 500);
    qz_grid_around(&s, 500, &p, &n);   CHECK(p == 500 && n == 1500);
    qz_grid_around(&s, 1499, &p, &n);  CHECK(p == 500 && n == 1500);
    qz_grid_around(&s, 1500, &p, &n);  CHECK(p == 1500 && n == 2500);
    qz_grid_around(&s, 499, &p, &n);   CHECK(p == -500u && n == 500);   /* before origin */
    qz_grid_around(&s, 0, &p, &n);     CHECK(p == -500u && n == 500);
    /* across uint32 wrap */
    qz_set_grid(&s, (uint64_t)1000 << 16, 0xfffffc18u);   /* origin = -1000 */
    qz_grid_around(&s, 10, &p, &n);    CHECK(p == 0 && n == 1000);
}

static void test_triplets_no_drift(void)
{
    qz_state s;
    uint32_t p, n, t;
    int k;

    /* 1/16T at 97 BPM: non-integer step; after 1000 bars (24000 steps) grid must still be exact */
    qz_init(&s);
    qz_set_grid(&s, qz_step_q16(9700, SR, 1, 6), 0);
    t = 0;
    for (k = 0; k < 24000; k++) {
        qz_grid_around(&s, t, &p, &n);
        CHECK(n > t);
        t = n;
    }
    /* exact: 24000 steps * (44100*60/97/6) samples = 1000 bars of 4/4 */
    {
        uint64_t exact = (uint64_t)24000 * SR * 6000 / (9700 * 6);
        CHECK(t >= exact - 1 && t <= exact + 1);
    }
}

static void test_window_rule(void)
{
    qz_state s;
    log_t l;
    qz_ev a = ev(QZ_ON, 1);

    memset(&l, 0, sizeof(l));
    qz_init(&s);
    qz_set_grid(&s, (uint64_t)8000 << 16, 0);    /* step 8000, window capped at 2000 */
    qz_set_window(&s, 1000);
    CHECK(qz_input(&s, 8000 + 1000, &a) == QZ_NOW);   /* just late: play now */
    CHECK(qz_input(&s, 8000 + 1001, &a) == QZ_HELD);  /* later: next grid 16000 */
    CHECK(qz_input(&s, 15990, E(QZ_ON, 2)) == QZ_HELD);  /* early: held to 16000 */
    l.now = 8000;
    run_until(&s, &l, 15936);         /* last 64-sample block before the grid point */
    CHECK(l.n == 0);
    run_until(&s, &l, 16064);
    CHECK(l.n == 2 && l.at[0] >= 16000 && l.at[0] < 16064);
    CHECK(l.ev[0].key == 1 && l.ev[1].key == 2);      /* stable order at equal due */
    CHECK(l.ev[0].data[0] == 101);                    /* payload kept */

    /* window cap: window 5000 > step/4 = 2000 */
    qz_init(&s);
    qz_set_grid(&s, (uint64_t)8000 << 16, 0);
    qz_set_window(&s, 5000);
    CHECK(qz_input(&s, 2000, &a) == QZ_NOW);
    CHECK(qz_input(&s, 2001, &a) == QZ_HELD);
}

#define AT(i, t) (l.at[i] >= (t) && l.at[i] < (t) + 64)   /* played in the block containing t */

static void test_note_off_order(void)
{
    qz_state s;
    log_t l;

    /* each note-off moves exactly as far as its note-on: held lengths are kept */
    memset(&l, 0, sizeof(l));
    qz_init(&s);
    qz_set_grid(&s, (uint64_t)8000 << 16, 0);
    qz_set_window(&s, 1000);
    CHECK(qz_input(&s, 6000, E(QZ_ON, 5)) == QZ_HELD);      /* -> 8000, moved 2000 */
    CHECK(qz_input(&s, 7000, E(QZ_OFF, 5)) == QZ_HELD);     /* held 1000 -> 9000 */
    CHECK(qz_input(&s, 7100, E(QZ_ON, 6)) == QZ_HELD);      /* -> 8000, moved 900 */
    l.now = 7100;
    run_until(&s, &l, 8064);
    CHECK(qz_input(&s, 8100, E(QZ_OFF, 6)) == QZ_HELD);     /* held 1000 -> 9000, not now */
    run_until(&s, &l, 30000);
    CHECK(l.n == 4);
    CHECK(l.ev[0].key == 5 && l.ev[0].kind == QZ_ON && AT(0, 8000));
    CHECK(l.ev[1].key == 6 && l.ev[1].kind == QZ_ON && AT(1, 8000));
    CHECK(l.ev[2].key == 5 && l.ev[2].kind == QZ_OFF && AT(2, 9000));
    CHECK(l.ev[3].key == 6 && l.ev[3].kind == QZ_OFF && AT(3, 9000));

    /* the device bug: 1/16 at 120 BPM (5512.5), pressed 40 ms (1764) after a grid point, held
     * 100 ms (4410). Before: off snapped into the window of the next point -> 15 ms note. Now:
     * on at 5512, off at 5512 + 4410 = 9922 -> sounds exactly 100 ms from the grid. */
    memset(&l, 0, sizeof(l));
    qz_init(&s);
    qz_set_grid(&s, qz_step_q16(12000, SR, 1, 4), 0);
    qz_set_window(&s, 1102);
    CHECK(qz_input(&s, 1764, E(QZ_ON, 60)) == QZ_HELD);
    CHECK(qz_input(&s, 1764 + 4410, E(QZ_OFF, 60)) == QZ_HELD);
    l.now = 1764;
    run_until(&s, &l, 20000);
    CHECK(l.n == 2 && AT(0, 5512) && AT(1, 9922));

    /* a note played on time (in the window) is not moved, so neither is its off */
    qz_init(&s);
    qz_set_grid(&s, (uint64_t)8000 << 16, 0);
    qz_set_window(&s, 1000);
    CHECK(qz_input(&s, 8300, E(QZ_ON, 7)) == QZ_NOW);
    CHECK(qz_input(&s, 9000, E(QZ_OFF, 7)) == QZ_NOW);

    /* very short press while the on is still held: minimum length one block */
    memset(&l, 0, sizeof(l));
    qz_init(&s);
    qz_set_grid(&s, (uint64_t)8000 << 16, 0);
    qz_set_window(&s, 1000);
    qz_input(&s, 5000, E(QZ_ON, 8));
    qz_input(&s, 5000, E(QZ_OFF, 8));
    l.now = 5000;
    run_until(&s, &l, 20000);
    CHECK(l.n == 2 && AT(0, 8000) && AT(1, 8000 + QZ_MIN_LEN));

    /* fast retrigger: on, off, on of one key all held: emitted in that order */
    memset(&l, 0, sizeof(l));
    qz_init(&s);
    qz_set_grid(&s, (uint64_t)8000 << 16, 0);
    qz_input(&s, 5000, E(QZ_ON, 9));
    qz_input(&s, 5100, E(QZ_OFF, 9));
    qz_input(&s, 5200, E(QZ_ON, 9));
    l.now = 5200;
    run_until(&s, &l, 40000);
    CHECK(l.n == 3 && l.ev[0].kind == QZ_ON && l.ev[1].kind == QZ_OFF && l.ev[2].kind == QZ_ON);
    CHECK(l.at[0] < l.at[1] && l.at[1] <= l.at[2]);
}

static void test_overflow_and_flush(void)
{
    qz_state s;
    log_t l;
    int i, held = 0;

    memset(&l, 0, sizeof(l));
    qz_init(&s);
    qz_set_grid(&s, (uint64_t)8000 << 16, 0);
    for (i = 0; i < QZ_QUEUE + 5; i++)
        held += qz_input(&s, 5000, E(QZ_ON, i)) == QZ_HELD;
    CHECK(held == QZ_QUEUE / 2);                /* note-ons use at most half the queue */
    CHECK(qz_flush(&s, rec, &l) == QZ_QUEUE / 2);
    CHECK(s.count == 0 && l.ev[0].key == 0 && l.ev[QZ_QUEUE / 2 - 1].key == QZ_QUEUE / 2 - 1);
    /* no grid = pass-through */
    qz_init(&s);
    CHECK(qz_input(&s, 123, E(QZ_ON, 1)) == QZ_NOW);
}

static void test_wrap(void)
{
    qz_state s;
    log_t l;
    uint32_t t0 = 0xffffff00u;

    memset(&l, 0, sizeof(l));
    qz_init(&s);
    qz_set_grid(&s, (uint64_t)1000 << 16, t0 - 300);   /* next grid point at t0 + 700 (wraps) */
    CHECK(qz_input(&s, t0, E(QZ_ON, 1)) == QZ_HELD);
    l.now = t0;
    run_until(&s, &l, t0 + 640);
    CHECK(l.n == 0);
    run_until(&s, &l, t0 + 800);
    CHECK(l.n == 1 && (int32_t)(l.at[0] - (t0 + 700)) >= 0);
}

static void test_queue(void)
{
    qz_state s;
    log_t l;

    /* step 8000, window 1000, chord window 500. Run C,E,G fast inside one step: three slots. */
    memset(&l, 0, sizeof(l));
    qz_init(&s);
    qz_set_grid(&s, (uint64_t)8000 << 16, 0);
    qz_set_window(&s, 1000);
    qz_set_queue(&s, 1, 500);
    CHECK(qz_input(&s, 8200, E(QZ_ON, 60)) == QZ_NOW);      /* in window of 8000: now */
    CHECK(qz_input(&s, 9000, E(QZ_ON, 64)) == QZ_HELD);     /* slot 8000 taken -> 16000 */
    CHECK(qz_input(&s, 9600, E(QZ_ON, 67)) == QZ_HELD);     /* 600 later (> chord) -> 24000 */
    CHECK(qz_input(&s, 9800, E(QZ_ON, 72)) == QZ_HELD);     /* 200 after 67: chord -> 24000 */
    /* released at 9900: each off keeps its held length, shifted like its on */
    CHECK(qz_input(&s, 9900, E(QZ_OFF, 60)) == QZ_NOW);      /* 60 was not moved */
    qz_input(&s, 9900, E(QZ_OFF, 64));                      /* moved 7000 -> 16900 */
    qz_input(&s, 9900, E(QZ_OFF, 67));                      /* moved 14400 -> 24300 */
    qz_input(&s, 9900, E(QZ_OFF, 72));                      /* moved 14200 -> 24100 */
    l.now = 9600;
    run_until(&s, &l, 40000);
    CHECK(l.n == 6);
    CHECK(l.ev[0].key == 64 && l.ev[0].kind == QZ_ON && l.at[0] >= 16000 && l.at[0] < 16064);
    CHECK(l.ev[1].key == 64 && l.ev[1].kind == QZ_OFF && l.at[1] >= 16900 && l.at[1] < 16964);
    CHECK(l.ev[2].key == 67 && l.ev[2].kind == QZ_ON && l.at[2] >= 24000 && l.at[2] < 24064);
    CHECK(l.ev[3].key == 72 && l.ev[3].kind == QZ_ON && l.at[3] == l.at[2]);
    CHECK(l.ev[4].key == 72 && l.ev[4].kind == QZ_OFF && l.at[4] >= 24100 && l.at[4] < 24164);
    CHECK(l.ev[5].key == 67 && l.ev[5].kind == QZ_OFF && l.at[5] >= 24300 && l.at[5] < 24364);

    /* chord window counts from the chord's first note: 0/300 together, 600 is a new slot */
    {
        qz_state c;
        qz_init(&c);
        qz_set_grid(&c, (uint64_t)8000 << 16, 0);
        qz_set_window(&c, 1000);
        qz_set_queue(&c, 1, 500);
        qz_input(&c, 2000, E(QZ_ON, 1));                    /* -> 8000 */
        qz_input(&c, 2300, E(QZ_ON, 2));                    /* chord -> 8000 */
        qz_input(&c, 2600, E(QZ_ON, 3));                    /* 600 after the chord start -> 16000 */
        CHECK(c.count == 3 && c.q[0].due == 8000 && c.q[1].due == 8000 && c.q[2].due == 16000);
    }

    /* after the queue has drained, the next note snaps normally again */
    CHECK(qz_input(&s, 40100, E(QZ_ON, 50)) == QZ_NOW);     /* 100 after 40000, slot free */

    /* queue off = snap mode: fast notes share the next grid point */
    qz_init(&s);
    qz_set_grid(&s, (uint64_t)8000 << 16, 0);
    qz_set_window(&s, 1000);
    qz_input(&s, 9100, E(QZ_ON, 1));
    qz_input(&s, 9300, E(QZ_ON, 2));
    CHECK(s.count == 2 && s.q[0].due == 16000 && s.q[1].due == 16000);
}

static void test_phrase_start(void)
{
    qz_state s;
    log_t l;
    uint32_t p, n;

    /* grid 8000 from origin 0, window 1000, phrase gap 8000 */
    memset(&l, 0, sizeof(l));
    qz_init(&s);
    qz_set_grid(&s, (uint64_t)8000 << 16, 0);
    qz_set_window(&s, 1000);
    qz_set_idle(&s, 8000);
    /* first note at 3000 (3000 after a grid point: would wait 5000) plays now and anchors */
    CHECK(qz_input(&s, 3000, E(QZ_ON, 60)) == QZ_NOW);
    qz_grid_around(&s, 3500, &p, &n);
    CHECK(p == 3000 && n == 11000);
    /* while it is held, the next note is quantized against the anchored grid: 11000 */
    CHECK(qz_input(&s, 5000, E(QZ_ON, 62)) == QZ_HELD);
    CHECK(s.q[0].due == 11000);
    /* its release keeps the held length (2000 -> 3000 + 2000 = 5000 shifted 0): now */
    CHECK(qz_input(&s, 5000, E(QZ_OFF, 60)) == QZ_NOW);
    l.now = 5000;
    run_until(&s, &l, 11064);
    CHECK(l.n == 1 && l.ev[0].key == 62 && AT(0, 11000));
    CHECK(qz_input(&s, 12000, E(QZ_OFF, 62)) == QZ_HELD);   /* moved like its on: 6000 -> 18000 */
    run_until(&s, &l, 18064);
    CHECK(l.n == 2 && AT(1, 18000));

    /* no new phrase before a full gap without input has passed */
    CHECK(qz_input(&s, 19000, E(QZ_ON, 64)) == QZ_NOW);     /* 1000 after 18000: window */
    CHECK(qz_input(&s, 30000, E(QZ_OFF, 64)) == QZ_NOW);
    CHECK(qz_input(&s, 33000, E(QZ_ON, 65)) == QZ_HELD);    /* only 3000 of silence */
    CHECK(s.anchored && s.anchor == 3000);
    qz_flush(&s, rec, &l);
    qz_input(&s, 33100, E(QZ_OFF, 65));         /* moved like its on: waits until 35100 */
    CHECK(qz_input(&s, 34000, E(QZ_ON, 66)) == QZ_HELD);    /* queue not empty: no phrase */
    qz_input(&s, 34100, E(QZ_OFF, 66));
    qz_flush(&s, rec, &l);
    /* 40000 of silence later: new phrase, re-anchored on the note */
    CHECK(qz_input(&s, 80000, E(QZ_ON, 67)) == QZ_NOW);
    CHECK(s.anchor == 80000);

    /* queue mode: the phrase note owns its slot, the next fast note goes one step later */
    qz_init(&s);
    qz_set_grid(&s, (uint64_t)8000 << 16, 0);
    qz_set_window(&s, 1000);
    qz_set_queue(&s, 1, 500);
    qz_set_idle(&s, 8000);
    CHECK(qz_input(&s, 3000, E(QZ_ON, 60)) == QZ_NOW);
    CHECK(qz_input(&s, 3800, E(QZ_ON, 64)) == QZ_HELD && s.q[0].due == 11000);

    /* idle 0 = off: first note waits for the grid as before */
    qz_init(&s);
    qz_set_grid(&s, (uint64_t)8000 << 16, 0);
    qz_set_window(&s, 1000);
    CHECK(qz_input(&s, 3000, E(QZ_ON, 60)) == QZ_HELD);
}

int main(void)
{
    test_step();
    test_grid();
    test_triplets_no_drift();
    test_window_rule();
    test_note_off_order();
    test_overflow_and_flush();
    test_wrap();
    test_queue();
    test_phrase_start();
    printf("%d/%d checks passed\n%s\n", checks - fails, checks, fails ? "FAIL" : "PASS");
    return fails != 0;
}
