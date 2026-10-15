/* DRUM ROLL core (spec: README.md): patterns, lanes, steps and the scheduler. Pure C, no TE
 * calls; host-tested by test_dr_core.c.
 *
 * Time is the sequencer clock in samples at 44.1 kHz (tape position while the tape runs, < 2^31).
 * Lanes start together at clock 0 and loop on their own length; a step lasts one note value of the
 * lane's rate. */
#ifndef DR_CORE_H
#define DR_CORE_H

#include <stdint.h>

#define DR_LANES        8
#define DR_STEPS        64
#define DR_PATTERNS     4
#define DR_KEYS         24
#define DR_RATES        6
#define DR_RATE_16      3               /* default 1/16 */
#define DR_LENS         13
#define DR_LEN_FULL     12              /* the sample plays out; note-off one step later */
#define DR_SR           44100

enum { DR_VEL, DR_TONE, DR_LEN, DR_NUDGE, DR_PROB, DR_RATCH, DR_PARAMS };

typedef struct dr_step {
    uint8_t on;
    int8_t p[DR_PARAMS];                /* VEL 1..100, TONE -24..24, LEN 0..12, NUDGE -50..50,
                                         * PROB 0..100, RATCH 1..4 */
} dr_step;

typedef struct dr_lane {
    uint8_t key;                        /* drum key 0..23 (shown 1..24) */
    uint8_t len;                        /* steps 1..64 */
    uint8_t rate;                       /* 0..DR_RATES-1 */
    uint8_t level;                      /* 0..100 % */
    uint8_t mute;
    uint8_t pad[3];
    dr_step s[DR_STEPS];
} dr_lane;

typedef struct dr_pattern {
    dr_lane lane[DR_LANES];
} dr_pattern;

extern const int8_t dr_pmin[DR_PARAMS], dr_pmax[DR_PARAMS], dr_pdef[DR_PARAMS];
extern const char *const dr_pname[DR_PARAMS];
extern const uint8_t dr_rate_num[DR_RATES], dr_rate_den[DR_RATES];   /* step = num/den quarter */
extern const char *const dr_rate_name[DR_RATES];
extern const uint8_t dr_len8[DR_LENS];  /* held length in eighths of a step (FULL: 8) */
extern const char *const dr_len_name[DR_LENS];

void dr_lane_init(dr_lane *l, int key);
void dr_pattern_init(dr_pattern *p);    /* lanes on keys 1..8, empty */
int dr_param_clamp(int param, int v);

/* samples per step (16.16) at bpm_x100 / 100 BPM; 0 if the tempo is 0 */
uint64_t dr_step_q16(uint32_t bpm_x100, int rate);

typedef struct dr_hit {
    int lane, step, hit;                /* hit 0 .. RATCH-1 */
    uint32_t k;                         /* step count since clock 0 */
    uint32_t at;                        /* clock samples */
    uint32_t hold;                      /* samples until the note-off */
    uint8_t full;                       /* LEN FULL: no gate, the sample plays out */
    const dr_step *s;
} dr_hit;

typedef void (*dr_emit_fn)(void *ctx, const dr_hit *h);

/* every hit of p with from <= at < from + n, in lane order (muted lanes skipped, probability by a
 * hash of seed, lane and k, so every hit of a step agrees and repeats deterministically).
 * step[l] = dr_step_q16 of lane l (0: lane silent). Returns the number of hits. */
int dr_schedule(const dr_pattern *p, uint32_t from, uint32_t n, const uint64_t step[DR_LANES],
                int swing, uint32_t seed, dr_emit_fn emit, void *ctx);

/* step index of lane l at clock t (its playhead), -1 if the lane has no tempo */
int dr_playhead(const dr_lane *l, uint32_t t, uint64_t step);

#endif
