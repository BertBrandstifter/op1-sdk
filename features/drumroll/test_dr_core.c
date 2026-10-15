/* host tests for dr_core.c: cc -std=c99 test_dr_core.c dr_core.c && ./a.out */
#include <stdio.h>
#include <string.h>
#include "dr_core.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

#define MAXH 4096
static dr_hit hits[MAXH];
static int nh;

static void emit(void *ctx, const dr_hit *h)
{
    (void)ctx;
    if (nh < MAXH)
        hits[nh++] = *h;
}

/* run the scheduler in consecutive 64-sample windows over [0, end) */
static void run(const dr_pattern *p, uint32_t end, const uint64_t *step, int swing, uint32_t seed)
{
    uint32_t t;

    nh = 0;
    for (t = 0; t < end; t += 64)
        dr_schedule(p, t, end - t < 64 ? end - t : 64, step, swing, seed, emit, 0);
}

static void steps_all(uint64_t *step, uint32_t bpm_x100, const dr_pattern *p)
{
    int l;

    for (l = 0; l < DR_LANES; l++)
        step[l] = dr_step_q16(bpm_x100, p->lane[l].rate);
}

int main(void)
{
    static dr_pattern p;
    uint64_t step[DR_LANES];
    int i, n;

    /* tempo: 120 BPM, 1/16 = 5512.5 samples, 1/8T = 7350, 1/4 = 22050 */
    CHECK(dr_step_q16(12000, DR_RATE_16) == (uint64_t)(5512.5 * 65536));
    CHECK(dr_step_q16(12000, 2) == (uint64_t)7350 << 16);
    CHECK(dr_step_q16(12000, 0) == (uint64_t)22050 << 16);
    CHECK(dr_step_q16(0, 0) == 0 && dr_step_q16(12000, DR_RATES) == 0);

    /* defaults */
    dr_pattern_init(&p);
    CHECK(p.lane[3].key == 3 && p.lane[3].len == 16 && p.lane[3].rate == DR_RATE_16
          && p.lane[3].level == 100 && !p.lane[3].s[5].on && p.lane[3].s[5].p[DR_VEL] == 100
          && p.lane[3].s[5].p[DR_LEN] == DR_LEN_FULL && p.lane[3].s[5].p[DR_RATCH] == 1);
    CHECK(dr_param_clamp(DR_TONE, 30) == 24 && dr_param_clamp(DR_RATCH, 0) == 1
          && dr_param_clamp(DR_VEL, 50) == 50);

    /* 1: four on the floor, one bar at 120 BPM: steps 0 4 8 12 at 0, 22050, 44100, 66150 */
    for (i = 0; i < 16; i += 4)
        p.lane[0].s[i].on = 1;
    steps_all(step, 12000, &p);
    run(&p, 88200, step, 0, 1);
    CHECK(nh == 4);
    for (i = 0; i < nh && i < 4; i++)
        CHECK(hits[i].lane == 0 && hits[i].step == i * 4 && hits[i].at == (uint32_t)(i * 22050)
              && hits[i].full && hits[i].hold == 5512);
    /* second bar loops */
    run(&p, 176400, step, 0, 1);
    CHECK(nh == 8 && hits[4].at == 88200 && hits[4].step == 0 && hits[4].k == 16);

    /* 2: windows of other sizes give the same hits exactly once */
    {
        uint32_t t = 0;
        int m = 0;
        static const uint32_t sizes[] = { 1, 63, 64, 65, 5000, 7 };

        nh = 0;
        for (i = 0; t < 176400; i++) {
            uint32_t w = sizes[i % 6];

            if (t + w > 176400)
                w = 176400 - t;
            dr_schedule(&p, t, w, step, 0, 1, emit, 0);
            t += w;
        }
        CHECK(nh == 8);
        for (i = 0; i < nh; i++)
            m += hits[i].at == (uint32_t)(i * 22050);
        CHECK(m == 8);
    }

    /* 3: polymeter: lane 1 = 3 steps (all on) against lane 0's 16; at 1/16 lane 1 hits every step */
    p.lane[1].len = 3;
    for (i = 0; i < 3; i++)
        p.lane[1].s[i].on = 1;
    p.lane[1].s[1].on = 0;                      /* x . x x . x ... */
    run(&p, 88200, step, 0, 1);
    for (n = 0, i = 0; i < nh; i++)
        if (hits[i].lane == 1) {
            CHECK(hits[i].step == (int)(hits[i].k % 3) && hits[i].k % 3 != 1);
            n++;
        }
    CHECK(n == 11);                             /* k 0..15 without k % 3 == 1 (5 of them) */

    /* 4: rate: lane 2 at 1/8T: 12 per bar, 7350 apart */
    dr_lane_init(&p.lane[1], 1);
    p.lane[2].rate = 2;
    p.lane[2].len = 12;
    for (i = 0; i < 12; i++)
        p.lane[2].s[i].on = 1;
    steps_all(step, 12000, &p);
    run(&p, 88200, step, 0, 1);
    for (n = 0, i = 0; i < nh; i++)
        if (hits[i].lane == 2) {
            CHECK(hits[i].at == (uint32_t)(hits[i].k * 7350));
            n++;
        }
    CHECK(n == 12);
    dr_lane_init(&p.lane[2], 2);

    /* 5: swing 50 = odd steps a quarter step late; nudge -50 on step 4 = half a step early */
    dr_lane_init(&p.lane[0], 0);
    p.lane[0].rate = 0;                         /* quarters: 22050 */
    p.lane[0].len = 4;
    p.lane[0].s[1].on = 1;
    p.lane[0].s[2].on = 1;
    p.lane[0].s[2].p[DR_NUDGE] = -50;
    steps_all(step, 12000, &p);
    run(&p, 88200, step, 50, 1);
    CHECK(nh == 2 && hits[0].at == 22050 + 5512 && hits[1].at == 44100 - 11025);
    /* the early step 2 of the second bar falls into the first bar's window range */
    run(&p, 88200 + 22050 * 2, step, 50, 1);
    CHECK(nh == 4 && hits[3].at == 88200 + 44100 - 11025 && hits[3].k == 6);
    /* nudge -50 on step 0: the very first one can not be early (clock starts at 0) */
    dr_lane_init(&p.lane[0], 0);
    p.lane[0].rate = 0;
    p.lane[0].len = 4;
    p.lane[0].s[0].on = 1;
    p.lane[0].s[0].p[DR_NUDGE] = -50;
    run(&p, 88200 * 2, step, 0, 1);
    CHECK(nh == 2 && hits[0].at == 88200 - 11025 && hits[0].k == 4 && hits[1].k == 8);

    /* 6: ratchet 3 on a quarter: 3 hits 7350 apart; LEN 1 step is cut to the hit spacing */
    dr_lane_init(&p.lane[0], 0);
    p.lane[0].rate = 0;
    p.lane[0].len = 4;
    p.lane[0].s[0].on = 1;
    p.lane[0].s[0].p[DR_RATCH] = 3;
    p.lane[0].s[0].p[DR_LEN] = 4;               /* 1 step */
    run(&p, 88200, step, 0, 1);
    CHECK(nh == 3 && hits[0].at == 0 && hits[1].at == 7350 && hits[2].at == 14700
          && hits[2].hit == 2 && hits[0].hold == 7350 && !hits[0].full);
    /* LEN 1/2 on quarters without ratchet: hold 11025 */
    p.lane[0].s[0].p[DR_RATCH] = 1;
    p.lane[0].s[0].p[DR_LEN] = 2;
    run(&p, 88200, step, 0, 1);
    CHECK(nh == 1 && hits[0].hold == 11025);

    /* 7: probability 0 never, 100 always, 50 about half and the same for a seed */
    dr_lane_init(&p.lane[0], 0);
    for (i = 0; i < 16; i++)
        p.lane[0].s[i].on = 1;
    steps_all(step, 12000, &p);
    p.lane[0].s[3].p[DR_PROB] = 0;
    run(&p, 88200 * 4, step, 0, 7);
    CHECK(nh == 60);
    for (i = 0; i < 16; i++)
        p.lane[0].s[i].p[DR_PROB] = 50;
    run(&p, 88200 * 16, step, 0, 7);
    n = nh;
    CHECK(n > 256 * 35 / 100 && n < 256 * 65 / 100);
    run(&p, 88200 * 16, step, 0, 7);
    CHECK(nh == n);
    /* a ratchet step with probability: all hits or none */
    dr_lane_init(&p.lane[0], 0);
    p.lane[0].rate = 0;
    p.lane[0].len = 1;
    p.lane[0].s[0].on = 1;
    p.lane[0].s[0].p[DR_PROB] = 50;
    p.lane[0].s[0].p[DR_RATCH] = 4;
    steps_all(step, 12000, &p);
    run(&p, 22050 * 64, step, 0, 3);
    {
        uint32_t k;
        int per[64];

        memset(per, 0, sizeof per);
        for (i = 0; i < nh; i++)
            per[hits[i].k]++;
        for (k = 0, n = 0; k < 64; k++) {
            CHECK(per[k] == 0 || per[k] == 4);
            n += per[k] == 4;
        }
        CHECK(n > 16 && n < 48);
    }

    /* 8: mute and no tempo */
    p.lane[0].mute = 1;
    run(&p, 88200, step, 0, 1);
    CHECK(nh == 0);
    p.lane[0].mute = 0;
    step[0] = 0;
    run(&p, 88200, step, 0, 1);
    CHECK(nh == 0);

    /* 9: playhead */
    dr_lane_init(&p.lane[0], 0);
    p.lane[0].len = 3;
    CHECK(dr_playhead(&p.lane[0], 0, dr_step_q16(12000, DR_RATE_16)) == 0);
    CHECK(dr_playhead(&p.lane[0], 5513, dr_step_q16(12000, DR_RATE_16)) == 1);
    CHECK(dr_playhead(&p.lane[0], 5512 * 3 + 2, dr_step_q16(12000, DR_RATE_16)) == 0);
    CHECK(dr_playhead(&p.lane[0], 100, 0) == -1);

    /* 10: a late clock (tape at 5 minutes) */
    dr_pattern_init(&p);
    p.lane[0].s[0].on = 1;
    steps_all(step, 9000, &p);                  /* 90 BPM: bar = 117600 */
    nh = 0;
    {
        uint32_t t;

        for (t = 13230000; t < 13230000 + 117600; t += 64)
            dr_schedule(&p, t, 64, step, 0, 1, emit, 0);
    }
    CHECK(nh == 1 && hits[0].at % 117600 == 0);

    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
