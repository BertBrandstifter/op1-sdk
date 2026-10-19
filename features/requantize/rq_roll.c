/* Requantize piano roll core, see rq_roll.h. */
#include "rq_roll.h"

uint32_t rq_grid_pt(uint64_t step_q16, uint32_t k)
{
    return (uint32_t)(((uint64_t)k * step_q16) >> 16);    /* same truncation as quant_core */
}

/* index of the grid point at or before pos */
uint32_t rq_grid_floor(uint64_t step_q16, uint32_t pos)
{
    uint32_t k = (uint32_t)(((uint64_t)pos << 16) / step_q16);

    while (rq_grid_pt(step_q16, k + 1) <= pos)
        k++;
    while (k && rq_grid_pt(step_q16, k) > pos)
        k--;
    return k;
}

uint32_t rq_grid_nearest(uint64_t step_q16, uint32_t pos)
{
    uint32_t k = rq_grid_floor(step_q16, pos), a = rq_grid_pt(step_q16, k),
             b = rq_grid_pt(step_q16, k + 1);

    return pos - a > b - pos ? b : a;
}

/* ------------------------------------------------------------------ build */

int rq_notes_build(rq_event *ev, int n, uint32_t end, rq_note *nt, int max)
{
    int i, j, count = 0;

    rq_sort(ev, n, 1);
    for (i = 0; i < n; i++) {
        const qz_ev *e = &ev[i].ev;

        if (e->kind == QZ_ON) {
            rq_note *t;

            if (count >= max)
                continue;
            t = &nt[count++];
            t->played = t->start = ev[i].pos;
            t->played_len = t->len = 0;
            for (j = 0; j < 6; j++)
                t->data[j] = e->data[j];
            t->key = t->key0 = e->key;
            t->flags = RN_OPEN;
            continue;
        }
        for (j = 0; j < count; j++)             /* first open note of this key */
            if ((nt[j].flags & RN_OPEN) && nt[j].key0 == e->key)
                break;
        if (j == count)
            continue;                           /* its note-on was before the take */
        nt[j].flags = 0;
        nt[j].played_len = nt[j].len = ev[i].pos - nt[j].played;
    }
    for (j = 0; j < count; j++)
        if (nt[j].flags & RN_OPEN) {            /* held past the take */
            nt[j].flags = 0;
            nt[j].played_len = nt[j].len = end > nt[j].played ? end - nt[j].played : 0;
        }
    for (j = 0; j < count; j++)
        if (nt[j].played_len < QZ_MIN_LEN)
            nt[j].played_len = nt[j].len = QZ_MIN_LEN;
    return count;
}

/* ------------------------------------------------------------------ render */

static void put(rq_event *e, uint32_t pos, int kind, int key, const uint8_t *data)
{
    int j;

    e->pos = pos;
    e->ev.kind = (uint8_t)kind;
    e->ev.key = (uint8_t)key;
    for (j = 0; j < 6; j++)
        e->ev.data[j] = data[j];
}

int rq_render(rq_note *nt, int n, rq_event *tmp, rq_event *out, int max, qz_state *s,
              uint64_t step_q16, int mode, uint32_t window, uint32_t chord)
{
    int i, k = 0, m;

    /* AUTO notes through the quantizer, identified by their index in data[0..1] */
    for (i = 0; i < n; i++) {
        uint8_t id[6] = { (uint8_t)i, (uint8_t)(i >> 8), 0, 0, 0, 0 };

        if (nt[i].flags & (RN_FIXED | RN_DEL))
            continue;
        nt[i].start = nt[i].played;
        nt[i].len = nt[i].played_len;
        put(&tmp[k++], nt[i].played, QZ_ON, nt[i].key0, id);
        put(&tmp[k++], nt[i].played + nt[i].played_len, QZ_OFF, nt[i].key0, id);
    }
    m = rq_quantize(tmp, k, out, max < 2 * n ? max : 2 * n, s, step_q16, mode, window, chord);
    for (i = 0; i < m; i++) {                   /* note-on before its note-off: out is sorted */
        rq_note *t = &nt[out[i].ev.data[0] | out[i].ev.data[1] << 8];

        if (out[i].ev.kind == QZ_ON)
            t->start = out[i].pos;
        else
            t->len = out[i].pos > t->start + QZ_MIN_LEN ? out[i].pos - t->start : QZ_MIN_LEN;
    }
    /* every live note -> replay events */
    for (i = k = 0; i < n && k + 2 <= max; i++) {
        if (nt[i].flags & RN_DEL)
            continue;
        put(&out[k++], nt[i].start, QZ_ON, nt[i].key, nt[i].data);
        put(&out[k++], nt[i].start + nt[i].len, QZ_OFF, nt[i].key, nt[i].data);
    }
    rq_sort(out, k, 1);
    return k;
}

/* ------------------------------------------------------------------ edits */

static int any_selected(const rq_note *nt, int n)
{
    int i;

    for (i = 0; i < n; i++)
        if (nt[i].flags & RN_SEL)
            return 1;
    return 0;
}

/* pos moved by `steps` grid steps: grid points stay grid points, off-grid notes keep their offset */
static uint32_t step_from(uint64_t step_q16, uint32_t pos, int32_t steps)
{
    uint32_t k = rq_grid_floor(step_q16, pos);
    int64_t d, to;

    if (rq_grid_pt(step_q16, k) == pos) {
        if (steps < 0 && (uint32_t)-steps > k)
            return 0;
        return rq_grid_pt(step_q16, (uint32_t)((int32_t)k + steps));
    }
    d = (int64_t)steps * (int64_t)step_q16;
    to = (int64_t)pos + (d >= 0 ? (d + 0x8000) >> 16 : -((-d + 0x8000) >> 16));
    return to < 0 ? 0 : (uint32_t)to;
}

static uint32_t add_clamped(uint32_t v, int32_t d, uint32_t min)
{
    int64_t r = (int64_t)v + d;

    return r < (int64_t)min ? min : (uint32_t)r;
}

int rq_edit(rq_note *nt, int n, int cur, int op, int32_t amount, uint64_t step_q16)
{
    int i, sel, all = 1, changed = 0;

    if (cur >= n || (cur < 0 && op == RE_SELECT))
        return 0;                               /* cur -1 (an empty cursor): the selection only */
    if (op == RE_SELECT) {
        nt[cur].flags ^= RN_SEL;
        return 1;
    }
    sel = any_selected(nt, n);
    if (op == RE_SELECT_ALL) {
        for (i = 0; i < n; i++)
            nt[i].flags = sel ? nt[i].flags & ~RN_SEL : nt[i].flags | RN_SEL;
        return 1;
    }
    if (op == RE_AUTO_ALL) {
        for (i = 0; i < n; i++)
            if (nt[i].flags & RN_FIXED) {
                nt[i].flags &= ~RN_FIXED;
                changed = 1;
            }
        return changed;
    }
    if ((op == RE_MOVE_STEP || op == RE_LEN_STEP || op == RE_SNAP) && !step_q16)
        return 0;
    for (i = 0; i < n; i++)                     /* toggles: decided over all targets */
        if (sel ? nt[i].flags & RN_SEL : i == cur) {
            if (op == RE_PLAYED)
                all &= !(nt[i].flags & RN_FIXED);
            if (op == RE_DELETE)
                all &= (nt[i].flags & RN_DEL) != 0;
        }
    for (i = 0; i < n; i++) {
        rq_note *t = &nt[i];
        uint32_t a, end;

        if (!(sel ? t->flags & RN_SEL : i == cur))
            continue;
        a = t->start;
        end = t->start + t->len;
        switch (op) {
        case RE_PITCH:
            t->key = (uint8_t)(t->key + amount < 0 ? 0 : t->key + amount > 127 ? 127 : t->key + amount);
            break;
        case RE_VEL: {
            int v = (int8_t)t->data[0];

            v = (v < 0 ? 127 : v) + amount;     /* logged -1 = 127 */
            t->data[0] = (uint8_t)(v < 1 ? 1 : v > 127 ? 127 : v);
            break;
        }
        case RE_DELETE:
            t->flags = all ? t->flags & ~RN_DEL : t->flags | RN_DEL;
            break;
        case RE_PLAYED:
            if (all) {
                t->flags |= RN_FIXED;
                t->start = t->played;
                t->len = t->played_len;
            } else {
                t->flags &= ~RN_FIXED;
            }
            break;
        case RE_MOVE:
            t->start = add_clamped(a, amount, 0);
            break;
        case RE_MOVE_STEP:
            t->start = step_from(step_q16, a, amount);
            break;
        case RE_SNAP:                           /* as CLEAN: start and length on the grid */
            {
                uint32_t k = rq_grid_floor(step_q16, rq_grid_nearest(step_q16, a)), steps;

                steps = (uint32_t)((((uint64_t)t->len << 16) + step_q16 / 2) / step_q16);
                t->start = rq_grid_pt(step_q16, k);
                t->len = rq_grid_pt(step_q16, k + (steps ? steps : 1)) - t->start;
            }
            break;
        case RE_LEN:
            t->len = add_clamped(t->len, amount, QZ_MIN_LEN);
            break;
        case RE_LEN_STEP:
            end = step_from(step_q16, end, amount);
            t->len = end > a + QZ_MIN_LEN ? end - a : QZ_MIN_LEN;
            break;
        default:
            return 0;
        }
        if (op != RE_PITCH && op != RE_DELETE && op != RE_PLAYED && op != RE_VEL)
            t->flags |= RN_FIXED;               /* timing now the user's */
        changed = 1;
    }
    return changed;
}

int rq_note_add(rq_note *nt, int n, int max, uint32_t start, uint32_t len, int key, const uint8_t data[6])
{
    rq_note *t;
    int i;

    if (n >= max || key < 0 || key > 127)
        return n;
    t = &nt[n];
    t->played = t->start = start;
    t->played_len = t->len = len < QZ_MIN_LEN ? QZ_MIN_LEN : len;
    for (i = 0; i < 6; i++)
        t->data[i] = data[i];
    t->key = t->key0 = (uint8_t)key;
    t->flags = RN_FIXED | RN_ADDED;
    return n + 1;
}

int rq_note_remove(rq_note *nt, int n, int i)
{
    if (i < 0 || i >= n)
        return n;
    for (; i < n - 1; i++)
        nt[i] = nt[i + 1];
    return n - 1;
}

/* ------------------------------------------------------------------ order */

static int before(const rq_note *nt, int a, int b)
{
    if (nt[a].start != nt[b].start)
        return nt[a].start < nt[b].start;
    if (nt[a].key != nt[b].key)
        return nt[a].key < nt[b].key;
    return a < b;
}

void rq_order(const rq_note *nt, int n, int *ord)
{
    int i, j, t;

    for (i = 0; i < n; i++) {
        t = i;
        for (j = i; j > 0 && before(nt, t, ord[j - 1]); j--)
            ord[j] = ord[j - 1];
        ord[j] = t;
    }
}
