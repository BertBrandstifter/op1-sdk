/* DRUM ROLL <-> TE's finger sequencer (dr_fs.h). */
#include "dr_fs.h"

int dr_fs_key(int32_t note, int *octave)
{
    int32_t n = note - 5;

    if (note < 5 || note > 127)
        return -1;
    if (octave)
        *octave = (int)(n / 24) - 2;            /* 53..76 -> 0 */
    return (int)(n % 24);
}

static void step_on(dr_step *s, int tone)
{
    int j;

    for (j = 0; j < DR_PARAMS; j++)
        s->p[j] = dr_pdef[j];
    s->p[DR_TONE] = (int8_t)dr_param_clamp(DR_TONE, tone);
    s->on = 1;
}

static int clamp_len(int len)
{
    return len < 1 ? 1 : len > DR_FS_STEPS ? DR_FS_STEPS : len;
}

void dr_fs_import(const dr_fs_notes notes, int len, dr_pattern *out)
{
    uint8_t used[DR_KEYS] = { 0 };
    int keys[DR_LANES], n = 0, s, h, k, l;

    len = clamp_len(len);
    for (s = 0; s < len; s++)
        for (h = 0; h < DR_FS_HANDS; h++) {
            k = dr_fs_key(notes[s][h], 0);
            if (k >= 0 && !used[k] && n < DR_LANES) {
                used[k] = 1;
                keys[n++] = k;
            }
        }
    for (k = 0; n < DR_LANES && k < DR_KEYS; k++)
        if (!used[k]) {
            used[k] = 1;
            keys[n++] = k;
        }
    for (l = 0; l < DR_LANES; l++) {
        dr_lane_init(&out->lane[l], keys[l]);
        out->lane[l].len = (uint8_t)len;
    }
    for (s = 0; s < len; s++)
        for (h = 0; h < DR_FS_HANDS; h++) {
            int oct;

            k = dr_fs_key(notes[s][h], &oct);
            if (k < 0)
                continue;
            for (l = 0; l < DR_LANES; l++)
                if (out->lane[l].key == k) {
                    step_on(&out->lane[l].s[s], 12 * oct);
                    break;
                }
        }
}

void dr_fs_summary(const dr_pattern *p, dr_fs_notes out)
{
    int s, h, l;

    for (s = 0; s < DR_FS_STEPS; s++)
        for (h = 0; h < DR_FS_HANDS; h++)
            out[s][h] = -1;
    for (l = 0; l < DR_LANES; l++) {
        const dr_lane *ln = &p->lane[l];
        int den = dr_rate_den[ln->rate < DR_RATES ? ln->rate : DR_RATE_16];
        int len = ln->len < 1 ? 1 : ln->len > DR_STEPS ? DR_STEPS : ln->len;
        int32_t note = DR_FS_NOTE0 + ln->key % DR_KEYS;
        int j;

        if (ln->mute)
            continue;
        /* lane step j starts at j / den quarters = 4 j / den sixteenths */
        for (j = 0; 4 * j < DR_FS_STEPS * den; j++) {
            if (!ln->s[j % len].on)
                continue;
            s = 4 * j / den;
            if (out[s][0] == note || out[s][1] == note)
                continue;
            if (out[s][0] < 0)
                out[s][0] = note;
            else if (out[s][1] < 0)
                out[s][1] = note;
        }
    }
}

/* first lane on key at rate 1/16, -1 none */
static int lane16(const dr_pattern *p, int key)
{
    int l;

    for (l = 0; l < DR_LANES; l++)
        if (p->lane[l].key == key && p->lane[l].rate == DR_RATE_16)
            return l;
    return -1;
}

static int lane_empty(const dr_lane *ln)
{
    int i;

    for (i = 0; i < DR_STEPS; i++)
        if (ln->s[i].on)
            return 0;
    return 1;
}

int dr_fs_te_edit(dr_pattern *p, int step, int32_t was, int32_t now, int te_len)
{
    int changed = 0, k, l, oct;

    if (step < 0 || step >= DR_FS_STEPS || was == now)
        return 0;
    k = dr_fs_key(was, 0);
    if (k >= 0) {
        l = lane16(p, k);
        if (l >= 0 && step < p->lane[l].len && p->lane[l].s[step].on) {
            p->lane[l].s[step].on = 0;
            changed = 1;
        }
    }
    k = dr_fs_key(now, &oct);
    if (k < 0)
        return changed;
    l = lane16(p, k);
    if (l < 0)                                  /* an empty lane, 1/16 ones first */
        for (l = 0; l < DR_LANES && !(lane_empty(&p->lane[l]) && p->lane[l].rate == DR_RATE_16); l++)
            ;
    if (l >= DR_LANES)
        for (l = 0; l < DR_LANES && !lane_empty(&p->lane[l]); l++)
            ;
    if (l >= DR_LANES)
        return changed;                         /* 8 lanes in use: TE's note stays TE's alone */
    if (p->lane[l].key != k || p->lane[l].rate != DR_RATE_16) {
        p->lane[l].key = (uint8_t)k;
        p->lane[l].rate = DR_RATE_16;
        if (p->lane[l].len < clamp_len(te_len))
            p->lane[l].len = (uint8_t)clamp_len(te_len);
    }
    if (p->lane[l].len <= step)
        p->lane[l].len = (uint8_t)(step + 1);
    step_on(&p->lane[l].s[step], 12 * oct);
    return 1;
}

int dr_fs_te_clear_step(dr_pattern *p, int step)
{
    int l, changed = 0;

    if (step < 0 || step >= DR_FS_STEPS)
        return 0;
    for (l = 0; l < DR_LANES; l++)
        if (p->lane[l].rate == DR_RATE_16 && step < p->lane[l].len && p->lane[l].s[step].on) {
            p->lane[l].s[step].on = 0;
            changed = 1;
        }
    return changed;
}

int dr_fs_lane_len(int rate, int len16)
{
    int num = dr_rate_num[rate % DR_RATES], den = dr_rate_den[rate % DR_RATES];
    int n = (2 * len16 * den + 4 * num) / (8 * num);           /* len16 / 4 quarters / (num / den), rounded */

    return n < 1 ? 1 : n > DR_STEPS ? DR_STEPS : n;
}

int dr_fs_length(const dr_pattern *p)
{
    int l, m = 0, mx = 1;

    for (l = 0; l < DR_LANES; l++) {
        const dr_lane *ln = &p->lane[l];
        int num = dr_rate_num[ln->rate % DR_RATES], den = dr_rate_den[ln->rate % DR_RATES];
        int n = (8 * ln->len * num + den) / (2 * den);

        if (n > mx)
            mx = n;
        if (num == 1 && (den == 4 || den == 8) && n > m)
            m = n;                              /* 1/16, 1/32: any length in 1/16 exactly */
    }
    m = m ? m : mx;
    return m > DR_STEPS ? DR_STEPS : m;
}

int dr_fs_set_length(dr_pattern *p, int len16)
{
    int l, changed = 0;

    len16 = len16 < 1 ? 1 : len16 > DR_STEPS ? DR_STEPS : len16;
    for (l = 0; l < DR_LANES; l++) {
        int n = dr_fs_lane_len(p->lane[l].rate, len16);

        changed |= p->lane[l].len != n;
        p->lane[l].len = (uint8_t)n;
    }
    return changed;
}

int dr_fs_window(dr_fs_clock *c, int event, uint64_t step, uint32_t block, uint32_t *from, uint32_t *to)
{
    uint32_t ls = (uint32_t)(step >> 16);

    if (event && !c->on) {
        c->on = 1;
        c->k = 0;
        c->idle = 0;
        *from = 0;
    } else if (!c->on) {
        *from = *to = 0;
        return 0;
    } else if (event) {
        c->k++;
        c->idle = 0;
        *from = c->next;
        if (!(c->k & 1)) {                      /* the swing pair grid: TE's step k starts now */
            uint32_t want = (uint32_t)(((uint64_t)c->k * step) >> 16);
            int32_t d = (int32_t)(want - c->next);

            if (d > (int32_t)(2 * block) || d < -(int32_t)(2 * block))
                *from = want;                   /* too far off: jump */
            else if (d < 0) {                   /* ahead: hold until TE is there */
                *to = want + block > c->next ? want + block : c->next;
                c->next = *to;
                return 1;
            } else if (d >= (int32_t)block) {   /* behind: catch up */
                *to = want + block;
                c->next = *to;
                return 1;
            }                                   /* step k in this window: in time, keep the clock */
        }
    } else {
        c->idle += block;
        if (c->idle > 3 * ls) {
            c->on = 0;
            *from = *to = 0;
            return 0;
        }
        *from = c->next;
    }
    *to = *from + block;
    c->next = *to;
    return 1;
}
