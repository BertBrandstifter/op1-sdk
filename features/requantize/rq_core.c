/* Requantize core, see rq_core.h. */
#include "rq_core.h"

typedef struct { rq_event *out; int n, max; qz_state *s; uint32_t now; } sink;

static void put(sink *k, uint32_t pos, const qz_ev *ev)
{
    if (k->n < k->max) {
        k->out[k->n].pos = pos;
        k->out[k->n].ev = *ev;
        k->n++;
    }
}

static void emit(const qz_ev *ev, void *ctx)
{
    sink *k = ctx;
    put(k, k->s->emit_due, ev);                 /* the event's own due position */
}

void rq_sort(rq_event *e, int n, int offs_first)
{
    int i, j;
    rq_event t;

    for (i = 1; i < n; i++) {
        t = e[i];
        for (j = i; j > 0 && (e[j - 1].pos > t.pos
                              || (offs_first && e[j - 1].pos == t.pos && e[j - 1].ev.kind == QZ_ON
                                  && t.ev.kind != QZ_ON)); j--)
            e[j] = e[j - 1];
        e[j] = t;
    }
}

/* grid point k from tape position 0 (same truncation as quant_core) */
static uint32_t grid_pt(uint64_t step_q16, uint32_t k)
{
    return (uint32_t)(((uint64_t)k * step_q16) >> 16);
}

/* STRICT: per key, the note-on waiting for its note-off */
static uint32_t st_in[QZ_KEYS], st_out[QZ_KEYS], st_cap[QZ_KEYS];
static uint8_t st_on[QZ_KEYS], st_capped[QZ_KEYS];

/* in is sorted. Pass 1 counts the chords (the last one is not cut), pass 2 writes. */
static int strict(const rq_event *in, int n, rq_event *out, int max, uint64_t step_q16,
                  uint32_t window, uint32_t chord)
{
    sink k = { out, 0, max, 0, 0 };
    uint32_t step = (uint32_t)(step_q16 >> 16), k0 = 0, first = 0, c, chords = 0, pass, pos, due;
    int i, have;

    if (window > step / 4)
        window = step / 4;
    if (chord > step / 2)
        chord = step / 2;                       /* as live quantize: fast runs still split */
    for (pass = 0; pass < 2; pass++) {
        have = 0;
        c = 0;
        for (i = 0; i < QZ_KEYS; i++)
            st_on[i] = 0;
        for (i = 0; i < n; i++) {
            const qz_ev *e = &in[i].ev;

            pos = in[i].pos;
            if (e->kind == QZ_ON) {
                if (!have) {                    /* first chord: its grid point, late-window rule */
                    k0 = (uint32_t)(((uint64_t)pos << 16) / step_q16);
                    while (grid_pt(step_q16, k0 + 1) <= pos)
                        k0++;
                    while (k0 && grid_pt(step_q16, k0) > pos)
                        k0--;
                    if (pos - grid_pt(step_q16, k0) > window)
                        k0++;
                    first = pos;
                    have = 1;
                } else if (pos - first > chord) {
                    c++;                        /* a new chord: the next grid step */
                    first = pos;
                }
                if (pass) {
                    due = grid_pt(step_q16, k0 + c);
                    st_in[e->key] = pos;
                    st_out[e->key] = due;
                    st_cap[e->key] = grid_pt(step_q16, k0 + c + 1);
                    st_capped[e->key] = c + 1 < chords;
                    st_on[e->key] = 1;
                    put(&k, due, e);
                }
                continue;
            }
            if (!pass)
                continue;
            if (!st_on[e->key]) {               /* its note-on was before the take: as played */
                put(&k, pos, e);
                continue;
            }
            due = st_out[e->key] + (pos - st_in[e->key]);   /* held length kept */
            if (st_capped[e->key] && due > st_cap[e->key])
                due = st_cap[e->key];           /* ends where the next chord starts */
            if (due < st_out[e->key] + QZ_MIN_LEN)
                due = st_out[e->key] + QZ_MIN_LEN;
            st_on[e->key] = 0;
            put(&k, due, e);
        }
        chords = c + have;
    }
    rq_sort(out, k.n, 1);
    return k.n;
}

int rq_quantize(rq_event *in, int n, rq_event *out, int max, qz_state *s,
                uint64_t step_q16, int mode, uint32_t window, uint32_t chord)
{
    sink k = { out, 0, max, s, 0 };
    int i;

    rq_sort(in, n, 0);
    if (mode == RQ_STRICT && step_q16)
        return strict(in, n, out, max, step_q16, window, chord);
    qz_init(s);
    qz_set_grid(s, step_q16, 0);
    qz_set_window(s, window);
    qz_set_queue(s, mode == RQ_QUEUE, chord);
    for (i = 0; i < n; i++) {
        qz_poll(s, in[i].pos, emit, &k);        /* everything due up to here first */
        if (qz_input(s, in[i].pos, &in[i].ev) == QZ_NOW)
            put(&k, in[i].pos, &in[i].ev);
    }
    qz_flush(s, emit, &k);
    rq_sort(out, k.n, 0);
    return k.n;
}
