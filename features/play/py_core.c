/* py_core: PLAY's logic without TE calls (py_core.h). */
#include "py_core.h"

#define BARRIER() __asm__ volatile("" ::: "memory")    /* data before the counter that publishes it */

static int up(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

static void scopy(char *d, const char *s, int max)     /* copy, cut to max - 1, NUL */
{
    int i;

    for (i = 0; i < max - 1 && s[i]; i++)
        d[i] = s[i];
    d[i] = 0;
}

static int slen(const char *s)
{
    int n = 0;

    while (s[n])
        n++;
    return n;
}

static void scat(char *d, const char *s, int max)
{
    int n = slen(d);

    scopy(d + n, s, max - n);
}

/* ------------------------------------------------------------------ set.txt */

typedef struct tok { const char *p; int n; } tok;

/* next token of the line [*at, end): a word or a "quoted string" (quotes stripped) */
static int next_tok(const char **at, const char *end, tok *t)
{
    const char *p = *at;

    while (p < end && (*p == ' ' || *p == '\t'))
        p++;
    if (p >= end)
        return 0;
    if (*p == '"') {
        const char *q = ++p;

        while (q < end && *q != '"')
            q++;
        if (q >= end)
            return -1;                                  /* unterminated */
        t->p = p;
        t->n = (int)(q - p);
        *at = q + 1;
        return 2;
    }
    t->p = p;
    while (p < end && *p != ' ' && *p != '\t')
        p++;
    t->n = (int)(p - t->p);
    *at = p;
    return 1;
}

static int tok_is(const tok *t, const char *w)
{
    int i;

    for (i = 0; i < t->n && w[i]; i++)
        if (up((unsigned char)t->p[i]) != up((unsigned char)w[i]))
            return 0;
    return i == t->n && !w[i];
}

/* "120", "98.5", "98.25" -> x 100; 0 = not a number */
static uint32_t parse_x100(const tok *t)
{
    uint32_t v = 0, frac = 0, fd = 0;
    int i, dot = 0;

    for (i = 0; i < t->n; i++) {
        char c = t->p[i];

        if (c == '.' && !dot) {
            dot = 1;
        } else if (c >= '0' && c <= '9') {
            if (dot) {
                if (fd < 2) {
                    frac = frac * 10 + (uint32_t)(c - '0');
                    fd++;
                }
            } else if (v < 100000) {
                v = v * 10 + (uint32_t)(c - '0');
            }
        } else {
            return 0;
        }
    }
    while (fd < 2) {
        frac *= 10;
        fd++;
    }
    return t->n ? v * 100 + frac : 0;
}

static int parse_line(const char *p, const char *end, py_setfile *s)
{
    tok key, a, b;

    if (next_tok(&p, end, &key) <= 0 || key.p[0] == '#')
        return 0;                                       /* empty or comment */
    if (tok_is(&key, "bpm")) {
        uint32_t v;

        if (next_tok(&p, end, &a) != 1)
            return -1;
        v = parse_x100(&a);
        if (v < 4000 || v > 22000 || next_tok(&p, end, &b))
            return -1;                                  /* TE's range 40..220 */
        s->bpm_x100 = v;
        return 0;
    }
    if (tok_is(&key, "audio") || tok_is(&key, "pattern") || tok_is(&key, "loop"))
        return 0;                                       /* earlier versions' lines: ignored */
    return -1;
}

void py_setfile_parse(const char *txt, uint32_t n, py_setfile *s)
{
    uint32_t i = 0, line = 0;

    s->bpm_x100 = 0;
    s->bad_line = 0;
    while (i < n && line < 1000) {
        uint32_t j = i;

        while (j < n && txt[j] != '\n')
            j++;
        line++;
        {
            uint32_t e = j;

            if (e > i && txt[e - 1] == '\r')
                e--;
            if (parse_line(txt + i, txt + e, s) && !s->bad_line)
                s->bad_line = (uint16_t)line;
        }
        i = j + 1;
    }
}

/* ------------------------------------------------------------------ the set */

uint32_t py_bar_frames(uint32_t bpm_x100)
{
    if (!bpm_x100)
        return 0;
    return (1058400000u + bpm_x100 / 2) / bpm_x100;     /* 4 beats x 60 s x 44100 x 100 */
}

static int name_eq(const char *a, const char *b)     /* case-insensitive */
{
    while (*a && up((unsigned char)*a) == up((unsigned char)*b))
        a++, b++;
    return up((unsigned char)*a) == up((unsigned char)*b);
}

void py_set_init(py_set *s, const py_setfile *sf, uint32_t te_bpm_x100)
{
    int k;

    s->bpm_x100 = sf && sf->bpm_x100 ? sf->bpm_x100 : te_bpm_x100 ? te_bpm_x100 : 12000;
    s->bar_frames = py_bar_frames(s->bpm_x100);
    for (k = 0; k < PY_TRACKS; k++) {
        s->t[k].used = 0;
        s->t[k].file = -1;
        s->t[k].err[0] = 0;
        s->t[k].song = s->t[k].pending = 0;
    }
}

static void segs_fill(const py_set *s, const py_track *t, py_segs *x, const uint8_t *seg, int n, int row0,
                      uint32_t origin)
{
    uint32_t sum = 0;
    int i;

    x->n = 0;
    x->row0 = (uint8_t)row0;
    x->origin = origin;
    for (i = 0; i < n && i < PY_SEGS; i++) {
        uint32_t len = seg[i] ? t->frames : s->bar_frames;

        if (sum + len < sum)
            break;                              /* past 2^32 frames (27 h): never reached */
        sum += len;
        x->seg[i] = seg[i];
        x->n = (uint8_t)(i + 1);
    }
    x->period = sum;
}

void py_set_song(py_set *s, int k, const uint8_t *seg, int n, int row0)
{
    py_track *t = &s->t[k];

    t->song = (uint8_t)(n >= 0);
    t->pending = 0;
    segs_fill(s, t, &t->cur, seg, n, row0, 0);
}

/* the segments track t plays at global frame g */
static const py_segs *segs_of(const py_track *t, uint32_t g)
{
    return t->pending && (int32_t)(g - t->sw) >= 0 ? &t->nxt : &t->cur;
}

/* segments x at global frame g: the segment, the frame inside it and what is left of it */
static int seg_at(const py_set *s, const py_track *t, const py_segs *x, uint32_t g, uint32_t *at,
                  uint32_t *left)
{
    uint32_t f, i;

    if (!x->n || !x->period)
        return -1;
    f = (g - x->origin) % x->period;
    for (i = 0; i < x->n; i++) {
        uint32_t len = x->seg[i] ? t->frames : s->bar_frames;

        if (f < len) {
            *at = f;
            *left = len - f;
            return (int)i;
        }
        f -= len;
    }
    return -1;
}

int py_set_row(const py_set *s, int k, uint32_t g)
{
    const py_track *t = &s->t[k];
    uint32_t at, left;
    int i;

    if (!t->used || !t->frames || !t->song)
        return -1;
    i = seg_at(s, t, segs_of(t, g), g, &at, &left);
    return i < 0 ? -1 : segs_of(t, g)->row0 + i;
}

int py_set_cued(const py_set *s, int k, uint32_t g)
{
    const py_track *t = &s->t[k];

    return t->used && t->song && t->pending && (int32_t)(g - t->sw) < 0 ? t->nxt.row0 : -1;
}

uint32_t py_set_cue(py_set *s, py_mixer *m, int k, const uint8_t *seg, int n, int row0, uint32_t guard)
{
    py_track *t = &s->t[k];
    py_ring *r = &m->ring[k];
    uint32_t g = m->pos + guard, at, left, sw;

    if (!t->used || !t->frames || !t->song)
        return 0;
    if (t->pending && (int32_t)(g - t->sw) >= 0) {      /* the last cue has happened (or is too close) */
        t->cur = t->nxt;
        t->pending = 0;
    }
    if (t->pending) {
        sw = t->sw;                                     /* still ahead: the new cue takes its place */
    } else if (seg_at(s, t, &t->cur, g, &at, &left) >= 0) {
        sw = g + left;                                  /* the end of the segment playing then */
    } else {
        uint32_t bar = s->bar_frames;

        sw = bar ? g - g % bar + bar : g;               /* silent: the next bar */
    }
    t->pending = 0;                                     /* (nxt is not read while it is rebuilt) */
    segs_fill(s, t, &t->nxt, seg, n < 0 ? 0 : n, row0, sw);
    t->sw = sw;
    t->pending = 1;
    if ((int32_t)(r->wr - sw) > 0)
        r->wr = sw;                                     /* read from there again, with the new segments */
    return sw;
}

int py_set_assign(py_set *s, int k, const py_file *files, int n, const char *name, int loop)
{
    py_track *t = &s->t[k];
    int i;

    t->used = 0;
    t->file = -1;
    t->err[0] = 0;
    t->song = t->pending = 0;
    t->loop = (uint8_t)(loop != 0);
    if (!name || !name[0]) {
        scopy(t->err, "NO FILE", sizeof(t->err));
        return 0;
    }
    for (i = 0; i < n; i++)
        if (name_eq(files[i].name, name))
            break;
    if (i == n) {
        scopy(t->err, name, sizeof(t->err));
        scat(t->err, ": GONE", sizeof(t->err));
        return 0;
    }
    if (files[i].a.err != AUD_OK) {
        scopy(t->err, aud_err_text(files[i].a.err), sizeof(t->err));
        return 0;
    }
    t->file = (int8_t)i;
    t->channels = files[i].a.channels;
    t->big_endian = files[i].a.big_endian;
    t->data_off = files[i].a.data_off;
    t->frames = files[i].a.frames;
    t->used = 1;
    return 1;
}

int32_t py_map(const py_set *s, int k, uint32_t g, uint32_t *run)
{
    const py_track *t = &s->t[k];
    uint32_t f;

    *run = 0xffffffffu;
    if (!t->used || !t->frames)
        return -1;
    if (t->song) {
        const py_segs *x = segs_of(t, g);
        uint32_t at, left = 0xffffffffu;
        int i = seg_at(s, t, x, g, &at, &left);

        if (x == &t->cur && t->pending && left > t->sw - g)
            left = t->sw - g;                   /* up to the cue's switch */
        *run = left;
        return i >= 0 && x->seg[i] ? (int32_t)at : -1;
    }
    if (!t->loop) {
        if (g >= t->frames)
            return -1;
        *run = t->frames - g;
        return (int32_t)g;
    }
    f = g % t->frames;
    *run = t->frames - f;
    return (int32_t)f;
}

/* ------------------------------------------------------------------ streaming */

void py_seek(py_mixer *m, uint32_t g)
{
    int k;

    m->pos = g;
    m->ended = 0;
    for (k = 0; k < PY_TRACKS; k++)
        m->ring[k].wr = g;
}

uint32_t py_ahead(const py_mixer *m, int k)
{
    int32_t a = (int32_t)(m->ring[k].wr - m->pos);

    return a > 0 ? (uint32_t)a : 0;
}

/* one read into ring k (up to a chunk); returns bytes read, 0 = nothing needed */
static uint32_t fill_one(py_mixer *m, int k, py_read_fn rd, void *ctx, uint8_t *scratch, int *did)
{
    py_ring *r = &m->ring[k];
    const py_track *t = &m->set->t[k];
    uint32_t pos = m->pos, ahead, room, n, run, i, mask = r->size - 1, ch = r->ch, bytes = 0;
    int32_t f;

    *did = 0;
    if ((int32_t)(pos - r->wr) > 0)                     /* fell behind (underrun): skip ahead */
        r->wr = pos;
    ahead = r->wr - pos;
    room = r->size - ahead;
    if (room < m->chunk)
        return 0;
    f = py_map(m->set, k, r->wr, &run);
    n = m->chunk;
    if (n > run)
        n = run;
    if (f < 0) {                                        /* past the end: silence */
        for (i = 0; i < n; i++) {
            uint32_t j = ((r->wr + i) & mask) * ch;

            r->buf[j] = 0;
            if (ch == 2)
                r->buf[j + 1] = 0;
        }
    } else {
        bytes = n * ch * 2;
        if (rd(ctx, k, t->data_off + (uint32_t)f * ch * 2, scratch, bytes) != (int)bytes) {
            m->read_errors++;
            for (i = 0; i < n * ch; i++)
                scratch[2 * i] = scratch[2 * i + 1] = 0;
        }
        for (i = 0; i < n; i++) {
            uint32_t j = ((r->wr + i) & mask) * ch, c;

            for (c = 0; c < ch; c++) {
                const uint8_t *b = scratch + (i * ch + c) * 2;

                r->buf[j + c] = (int16_t)(t->big_endian ? (b[0] << 8 | b[1]) : (b[1] << 8 | b[0]));
            }
        }
    }
    BARRIER();
    r->wr += n;
    *did = 1;
    return bytes;
}

uint32_t py_fill(py_mixer *m, py_read_fn rd, void *ctx, uint32_t budget, uint8_t *scratch)
{
    uint32_t done = 0;
    int round, any = 1;

    if (!m->set)
        return 0;
    for (round = 0; any && done < budget && round < 64; round++) {
        int i;

        any = 0;
        for (i = 0; i < PY_TRACKS && done < budget; i++) {
            int k = (m->next + i) % PY_TRACKS, did;

            if (!m->set->t[k].used || !m->ring[k].buf)
                continue;
            done += fill_one(m, k, rd, ctx, scratch, &did);
            if (did) {
                any = 1;
                m->next = (uint8_t)((k + 1) % PY_TRACKS);
            }
        }
    }
    return done;
}

/* ------------------------------------------------------------------ mix (audio interrupt) */

static int32_t acc_l[PY_BLOCK], acc_r[PY_BLOCK];

static int32_t sat_add(int32_t a, int32_t b)
{
    int32_t r = (int32_t)((uint32_t)a + (uint32_t)b);

    if (((a ^ r) & (b ^ r)) < 0)
        r = a < 0 ? (int32_t)0x80000000 : 0x7fffffff;
    return r;
}

static int32_t soft(int32_t x, volatile uint32_t *clips)
{
    int32_t a = x < 0 ? -x : x;

    if (a > 24576) {                                    /* soft knee above -2.5 dBFS */
        a = 24576 + ((a - 24576) >> 2);
        if (a > 32767) {
            a = 32767;
            (*clips)++;
        }
    }
    return x < 0 ? -a : a;
}

void py_mix(py_mixer *m, int32_t *lo, int32_t *ro, int stride)
{
    const py_set *s = m->set;
    uint32_t pos = m->pos;
    int32_t mv;
    int k, f;

    if (!m->playing || !s)
        return;
    for (f = 0; f < PY_BLOCK; f++)
        acc_l[f] = acc_r[f] = 0;
    for (k = 0; k < PY_TRACKS; k++) {
        py_ring *r = &m->ring[k];
        int32_t g = m->gain[k];
        uint32_t mask = r->size - 1, pk = 0;

        if (!s->t[k].used || !r->buf)
            continue;
        if ((int32_t)(r->wr - pos) < PY_BLOCK) {        /* late: silence, position still moves */
            m->underruns[k]++;
            continue;
        }
        if (r->ch == 2) {
            for (f = 0; f < PY_BLOCK; f++) {
                const int16_t *x = r->buf + ((pos + (uint32_t)f) & mask) * 2;
                int32_t l = (x[0] * g) >> 15, rr = (x[1] * g) >> 15;

                acc_l[f] += l;
                acc_r[f] += rr;
                l = l < 0 ? -l : l;
                rr = rr < 0 ? -rr : rr;
                if ((uint32_t)l > pk)
                    pk = (uint32_t)l;
                if ((uint32_t)rr > pk)
                    pk = (uint32_t)rr;
            }
        } else {
            for (f = 0; f < PY_BLOCK; f++) {
                int32_t v = (r->buf[(pos + (uint32_t)f) & mask] * g) >> 15;

                acc_l[f] += v;
                acc_r[f] += v;
                v = v < 0 ? -v : v;
                if ((uint32_t)v > pk)
                    pk = (uint32_t)v;
            }
        }
        if (pk > m->peak[k])
            m->peak[k] = (uint16_t)pk;
    }
    for (k = 0; k < PY_TRACKS; k++) {           /* pattern tracks: mono into both channels */
        int32_t g = m->gain[k], *x = m->pin[k];
        uint32_t pk = 0;

        if (!((m->pin_used >> k) & 1))
            continue;
        for (f = 0; f < PY_BLOCK; f++) {
            int32_t v = (x[f] * g) >> 15;

            acc_l[f] += v;
            acc_r[f] += v;
            x[f] = 0;
            v = v < 0 ? -v : v;
            if ((uint32_t)v > pk)
                pk = (uint32_t)v;
        }
        if (pk > m->peak[k])
            m->peak[k] = (uint16_t)(pk > 65535 ? 65535 : pk);
    }
    m->pin_used = 0;
    if (m->ext_used) {                          /* SYNTH tracks, gains and peaks already applied */
        for (f = 0; f < PY_BLOCK; f++) {
            acc_l[f] += m->ext[f];
            acc_r[f] += m->ext[f];
            m->ext[f] = 0;
        }
        m->ext_used = 0;
    }
    mv = (m->master * m->te_volume) >> 15;
    {
        uint32_t pk = 0;

        for (f = 0; f < PY_BLOCK; f++) {
            int32_t l = soft((acc_l[f] * mv) >> 15, &m->clips);
            int32_t r = soft((acc_r[f] * mv) >> 15, &m->clips);

            lo[f * stride] = sat_add(lo[f * stride], (int32_t)((uint32_t)l << 16));
            ro[f * stride] = sat_add(ro[f * stride], (int32_t)((uint32_t)r << 16));
            l = l < 0 ? -l : l;
            r = r < 0 ? -r : r;
            if ((uint32_t)l > pk)
                pk = (uint32_t)l;
            if ((uint32_t)r > pk)
                pk = (uint32_t)r;
        }
        if (pk > m->peak_out)
            m->peak_out = (uint16_t)pk;
    }
    pos += PY_BLOCK;
    m->pos = pos;

}

static const uint16_t db_table[61] = {
    32767, 29205, 26029, 23198, 20675, 18427, 16423, 14637, 13045, 11627, 10362, 9235, 8231, 7336,
    6538, 5827, 5193, 4629, 4125, 3677, 3277, 2920, 2603, 2320, 2068, 1843, 1642, 1464, 1305, 1163,
    1036, 924, 823, 734, 654, 583, 519, 463, 413, 368, 328, 292, 260, 232, 207, 184, 164, 146, 130,
    116, 104, 92, 82, 73, 65, 58, 52, 46, 41, 37, 33,
};

int16_t py_db_q15(int db10)
{
    int d;

    if (db10 >= 0)
        return 32767;
    if (db10 < -600)
        return 0;
    d = (-db10 + 5) / 10;
    return (int16_t)db_table[d];
}
