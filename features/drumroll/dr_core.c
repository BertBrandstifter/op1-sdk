/* DRUM ROLL core: data model + scheduler (dr_core.h). */
#include "dr_core.h"

const int8_t dr_pmin[DR_PARAMS] = { 1, -24, 0, -50, 0, 1 };
const int8_t dr_pmax[DR_PARAMS] = { 100, 24, DR_LENS - 1, 50, 100, 4 };
const int8_t dr_pdef[DR_PARAMS] = { 100, 0, DR_LEN_FULL, 0, 100, 1 };
const char *const dr_pname[DR_PARAMS] = { "VEL", "TONE", "LEN", "NUDGE", "PROB", "RATCH" };

const uint8_t dr_rate_num[DR_RATES] = { 1, 1, 1, 1, 1, 1 };
const uint8_t dr_rate_den[DR_RATES] = { 1, 2, 3, 4, 6, 8 };
const char *const dr_rate_name[DR_RATES] = { "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32" };

const uint8_t dr_len8[DR_LENS] = { 1, 2, 4, 6, 8, 12, 16, 24, 32, 48, 64, 128, 8 };
const char *const dr_len_name[DR_LENS] = {
    "1/8", "1/4", "1/2", "3/4", "1", "1.5", "2", "3", "4", "6", "8", "16", "FULL",
};

void dr_lane_init(dr_lane *l, int key)
{
    int i, j;

    l->key = (uint8_t)key;
    l->len = 16;
    l->rate = DR_RATE_16;
    l->level = 100;
    l->mute = 0;
    l->pad[0] = l->pad[1] = l->pad[2] = 0;
    for (i = 0; i < DR_STEPS; i++) {
        l->s[i].on = 0;
        for (j = 0; j < DR_PARAMS; j++)
            l->s[i].p[j] = dr_pdef[j];
    }
}

void dr_pattern_init(dr_pattern *p)
{
    int i;

    for (i = 0; i < DR_LANES; i++)
        dr_lane_init(&p->lane[i], i);
}

int dr_param_clamp(int param, int v)
{
    return v < dr_pmin[param] ? dr_pmin[param] : v > dr_pmax[param] ? dr_pmax[param] : v;
}

uint64_t dr_step_q16(uint32_t bpm_x100, int rate)
{
    /* samples per quarter = sr * 60 / bpm = sr * 6000 / bpm_x100 */
    if (!bpm_x100 || rate < 0 || rate >= DR_RATES)
        return 0;
    return ((uint64_t)DR_SR * 6000 * dr_rate_num[rate] << 16)
           / ((uint64_t)bpm_x100 * dr_rate_den[rate]);
}

static uint32_t mix(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

int dr_schedule(const dr_pattern *p, uint32_t from, uint32_t n, const uint64_t step[DR_LANES],
                int swing, uint32_t seed, dr_emit_fn emit, void *ctx)
{
    int l, count = 0;

    /* runs once per audio block: 32-bit maths except k * step (no 64-bit divides) */
    for (l = 0; l < DR_LANES; l++) {
        const dr_lane *ln = &p->lane[l];
        uint64_t L = step[l];
        uint32_t ls = (uint32_t)(L >> 16), k, k0, k1, len, si, hq;
        int32_t sw;

        if (ln->mute || ls == 0)
            continue;
        len = ln->len < 1 ? 1 : ln->len > DR_STEPS ? DR_STEPS : ln->len;
        sw = (int32_t)(ls * (uint32_t)swing / 200);         /* swing 100 = half a step late */
        /* a hit lands at most half a step early (nudge) and under two steps late (swing + nudge +
         * ratchet) */
        k0 = from / ls;                         /* clock < 2^31: one 32-bit divide per lane */
        k1 = k0 + 1;
        for (hq = from - k0 * ls + n; hq >= ls; hq -= ls)
            k1++;                               /* (from + n) / ls + 1 */
        k0 = k0 < 2 ? 0 : k0 - 2;
        for (k = k0, si = k0 % len; k <= k1; k++, si = si + 1 == len ? 0 : si + 1) {
            const dr_step *s = &ln->s[si];
            int32_t base, off;
            uint32_t hold;
            int r, h, li;

            if (!s->on)
                continue;
            if (s->p[DR_PROB] < 100
                && (int)(mix(seed ^ k * 0x9e3779b1u ^ (uint32_t)l * 0x85ebca77u) % 100) >= s->p[DR_PROB])
                continue;
            r = s->p[DR_RATCH] < 1 ? 1 : s->p[DR_RATCH];
            li = s->p[DR_LEN] < 0 || s->p[DR_LEN] >= DR_LENS ? DR_LEN_FULL : s->p[DR_LEN];
            hq = ls * dr_len8[li] / 8;
            if (li != DR_LEN_FULL && r > 1 && hq > ls / (uint32_t)r)
                hq = ls / (uint32_t)r;          /* a ratchet hit ends before the next one */
            hold = hq < 1 ? 1 : hq;
            base = (int32_t)(((uint64_t)k * L) >> 16);
            off = (k & 1 ? sw : 0) + (int32_t)ls * s->p[DR_NUDGE] / 100;
            for (h = 0; h < r; h++) {
                int32_t t = base + off + (int32_t)(ls * (uint32_t)h / (uint32_t)r);
                dr_hit hit;

                if (t < (int32_t)from || t - (int32_t)from >= (int32_t)n)
                    continue;
                hit.lane = l;
                hit.step = (int)si;
                hit.hit = h;
                hit.k = k;
                hit.at = (uint32_t)t;
                hit.hold = hold;
                hit.full = li == DR_LEN_FULL;
                hit.s = s;
                emit(ctx, &hit);
                count++;
            }
        }
    }
    return count;
}

int dr_playhead(const dr_lane *l, uint32_t t, uint64_t step)
{
    uint32_t len = l->len < 1 ? 1 : l->len > DR_STEPS ? DR_STEPS : l->len, ls = (uint32_t)(step >> 16);

    if (!ls)
        return -1;
    return (int)((t / ls) % len);               /* whole samples per step, as dr_schedule */
}
