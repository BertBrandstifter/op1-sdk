/* Input quantizer core, see quant_core.h and features/quantize/README.md. */
#include "quant_core.h"

#define AFTER(a, b)     ((int32_t)((a) - (b)) > 0)     /* a later than b (wrap-safe) */

void qz_init(qz_state *s)
{
    int i;

    s->step_q16 = 0;
    s->origin = 0;
    s->window = 0;
    s->seq = 0;
    s->count = 0;
    s->queue = 0;
    s->chord = 0;
    s->have_on = 0;
    for (i = 0; i < QZ_KEYS; i++)
        s->on_valid[i] = 0;
    s->idle = 0;
    s->last_input = 0;
    s->anchored = 0;
    s->anchor = 0;
}

void qz_set_grid(qz_state *s, uint64_t step_q16, uint32_t origin)
{
    s->step_q16 = step_q16;
    s->origin = origin;
}

void qz_set_window(qz_state *s, uint32_t samples)
{
    s->window = samples;
}

void qz_set_idle(qz_state *s, uint32_t samples)
{
    s->idle = samples;
}

void qz_set_chord(qz_state *s, uint32_t chord)
{
    s->chord = chord;
}

void qz_set_queue(qz_state *s, int on, uint32_t chord)
{
    s->queue = on;
    s->chord = chord;
    s->have_on = 0;
}

/* grid point k (signed) as an integer sample offset from origin: floor(k * step) */
static int64_t grid_off(const qz_state *s, int64_t k)
{
    int64_t x = k * (int64_t)s->step_q16;

    return x >= 0 ? x >> 16 : -((-x + 0xffff) >> 16);
}

void qz_grid_around(const qz_state *s, uint32_t t, uint32_t *prev, uint32_t *next)
{
    uint32_t org = s->anchored ? s->anchor : s->origin;
    int64_t d = (int32_t)(t - org);           /* t relative to origin, wrap-safe */
    int64_t st = (int64_t)s->step_q16, k;

    /* k = floor(d / step), then settle on the truncated grid points: g(k) <= d < g(k+1) */
    k = d >= 0 ? (d << 16) / st : -((((-d) << 16) + st - 1) / st);
    while (grid_off(s, k + 1) <= d)
        k++;
    while (grid_off(s, k) > d)
        k--;
    *prev = org + (uint32_t)grid_off(s, k);
    *next = org + (uint32_t)grid_off(s, k + 1);
}

/* latest due time already queued for this key, if any */
static int last_due_for_key(const qz_state *s, uint8_t key, uint32_t *due, uint8_t *kind)
{
    int i, found = 0;

    for (i = 0; i < s->count; i++)
        if (s->q[i].ev.key == key && (!found || !AFTER(*due, s->q[i].due))) {
            *due = s->q[i].due;
            *kind = s->q[i].ev.kind;
            found = 1;
        }
    return found;
}

static int push(qz_state *s, const qz_ev *ev, uint32_t due)
{
    s->q[s->count].ev = *ev;
    s->q[s->count].due = due;
    s->q[s->count].seq = s->seq++;
    s->count++;
    return QZ_HELD;
}

int qz_input(qz_state *s, uint32_t now, const qz_ev *ev)
{
    uint32_t prev, next, due, slot, step, win, pdue;
    uint8_t pkind;
    int pending, in_window, ons = 0, i;

    int phrase;

    /* phrase start: first note-on after a silence plays now and anchors the grid */
    phrase = ev->kind == QZ_ON && s->idle && s->step_q16 && !s->count
             && (!s->last_input || (uint32_t)(now - s->last_input) >= s->idle);
    s->last_input = now ? now : 1;
    if (phrase) {
        s->anchored = 1;
        s->anchor = now;
        s->shift[ev->key] = 0;
        s->on_due[ev->key] = now;
        s->on_valid[ev->key] = 1;
        s->have_on = 1;                         /* queue mode: this note owns the anchor slot */
        s->last_on_in = now;
        s->last_on_due = now;
        s->last_on_slot = now;
        return QZ_NOW;
    }

    pending = last_due_for_key(s, ev->key, &pdue, &pkind);
    for (i = 0; i < s->count; i++)
        ons += s->q[i].ev.kind == QZ_ON;
    if (!s->step_q16 || s->count >= QZ_QUEUE || (ev->kind == QZ_ON && ons >= QZ_QUEUE / 2)) {
        /* unquantized; still never ahead of a queued event of the same key */
        if (ev->kind == QZ_ON)
            s->on_valid[ev->key] = 0;
        if (!pending || s->count >= QZ_QUEUE)
            return QZ_NOW;
        return push(s, ev, pdue);
    }

    if (ev->kind == QZ_OFF) {
        /* keep the held length: move the off exactly as far as its note-on was moved */
        due = now;
        if (s->on_valid[ev->key]) {
            due = now + (uint32_t)s->shift[ev->key];
            if (AFTER(s->on_due[ev->key] + QZ_MIN_LEN, due))
                due = s->on_due[ev->key] + QZ_MIN_LEN;
            s->on_valid[ev->key] = 0;
        }
        if (pending && AFTER(pdue, due))
            due = pdue;
        if (due == now && !pending)
            return QZ_NOW;
        return push(s, ev, due);
    }

    qz_grid_around(s, now, &prev, &next);
    step = (uint32_t)(s->step_q16 >> 16);
    win = s->window < step / 4 ? s->window : step / 4;
    in_window = (uint32_t)(now - prev) <= win;
    due = in_window ? now : next;
    slot = in_window ? prev : next;             /* the grid point this note belongs to */

    if (s->queue) {
        if (s->have_on && (uint32_t)(now - s->last_on_in) <= s->chord) {
            /* chord: within the window of the chord's FIRST note -> same slot (a fast run of
             * notes closer than the window still splits once the window has passed) */
            slot = s->last_on_slot;
            due = AFTER(s->last_on_due, now) ? s->last_on_due : now;
        } else {
            if (s->have_on && !AFTER(slot, s->last_on_slot)) {
                qz_grid_around(s, s->last_on_slot, &prev, &next);
                slot = due = next;                      /* slot taken: next free one */
            }
            s->last_on_in = now;                        /* this note starts a new chord */
        }
        s->have_on = 1;
        s->last_on_due = due;
        s->last_on_slot = slot;
    }
    if (pending && AFTER(pdue, due))            /* never overtake the same key */
        due = pdue;
    s->shift[ev->key] = (int32_t)(due - now);
    s->on_due[ev->key] = due;
    s->on_valid[ev->key] = 1;
    if (due == now && !pending)
        return QZ_NOW;
    return push(s, ev, due);
}

/* index of the earliest queued event (stable: lowest seq among equal due) */
static int earliest(const qz_state *s)
{
    int i, best = 0;

    for (i = 1; i < s->count; i++)
        if (AFTER(s->q[best].due, s->q[i].due)
            || (s->q[i].due == s->q[best].due && AFTER(s->q[best].seq, s->q[i].seq)))
            best = i;
    return best;
}

static void take(qz_state *s, int i, qz_ev *out)
{
    *out = s->q[i].ev;
    for (; i < s->count - 1; i++)
        s->q[i] = s->q[i + 1];
    s->count--;
}

int qz_poll(qz_state *s, uint32_t now, qz_emit_fn emit, void *ctx)
{
    int i, n = 0;
    qz_ev ev;

    while (s->count) {
        i = earliest(s);
        if (AFTER(s->q[i].due, now))
            break;
        s->emit_due = s->q[i].due;
        take(s, i, &ev);
        emit(&ev, ctx);
        n++;
    }
    return n;
}

int qz_flush(qz_state *s, qz_emit_fn emit, void *ctx)
{
    int n = 0, i;
    qz_ev ev;

    while (s->count) {
        i = earliest(s);
        s->emit_due = s->q[i].due;
        take(s, i, &ev);
        emit(&ev, ctx);
        n++;
    }
    return n;
}

uint64_t qz_step_q16(uint32_t bpm_x100, uint32_t sr, uint32_t beats_num, uint32_t beats_den)
{
    /* samples per quarter = sr * 60 / bpm = sr * 6000 / bpm_x100; step = that * num / den */
    if (!bpm_x100 || !beats_den)
        return 0;
    return ((uint64_t)sr * 6000 * beats_num << 16) / ((uint64_t)bpm_x100 * beats_den);
}
