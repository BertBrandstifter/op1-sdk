/* NOTE ROLL <-> TE's synth finger sequencer (nr_fs.h). */
#include "nr_fs.h"

void nr_fs_import(const nr_fs_notes notes, int len, nr_pattern *out)
{
    int s, h;

    nr_pattern_init(out);
    out->len = (uint8_t)(len < 1 ? 1 : len > NR_FS_STEPS ? NR_FS_STEPS : len);
    for (s = 0; s < NR_FS_STEPS; s++)
        for (h = 0; h < NR_FS_HANDS; h++)
            if (notes[s][h] >= 0 && notes[s][h] <= 127)
                nr_add(&out->s[s], notes[s][h], 100, NR_LEN_DEF);
}

void nr_fs_summary(const nr_pattern *p, nr_fs_notes out)
{
    int s, n;

    for (s = 0; s < NR_FS_STEPS; s++) {
        const nr_step *st = &p->s[s];

        out[s][0] = out[s][1] = -1;
        if (s >= p->len || !(n = nr_voices(st)))
            continue;
        out[s][0] = st->n[0].key;
        if (n > 1)
            out[s][1] = st->n[n - 1].key;
    }
}

static int find(const nr_step *s, int key)
{
    int v;

    for (v = 0; v < NR_VOICES; v++)
        if (s->n[v].key == key)
            return v;
    return -1;
}

int nr_fs_te_edit(nr_pattern *p, const nr_fs_notes was, const nr_fs_notes now)
{
    int s, h, v, changed = 0;

    for (s = 0; s < NR_FS_STEPS; s++) {
        if ((was[s][0] >= 0 || was[s][1] >= 0) && now[s][0] < 0 && now[s][1] < 0) {
            if (nr_voices(&p->s[s])) {          /* TE emptied the step (delete, clear): all of it */
                nr_step_init(&p->s[s]);
                changed = 1;
            }
            continue;
        }
        for (h = 0; h < NR_FS_HANDS; h++) {
            if (was[s][h] == now[s][h])
                continue;
            if (was[s][h] >= 0 && was[s][h] <= 127 && (v = find(&p->s[s], was[s][h])) >= 0) {
                nr_remove(&p->s[s], v);
                changed = 1;
            }
        }
    }
    for (s = 0; s < NR_FS_STEPS; s++)               /* adds after removes: a moved hand stays */
        for (h = 0; h < NR_FS_HANDS; h++)
            if (was[s][h] != now[s][h] && now[s][h] >= 0 && now[s][h] <= 127
                && nr_add(&p->s[s], now[s][h], 100, NR_LEN_DEF) >= 0)
                changed = 1;
    return changed;
}

int nr_fs_window(nr_fs_clock *c, int event, uint64_t step, uint32_t block, uint32_t *from, uint32_t *to)
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
