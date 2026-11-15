/* py_song: PLAY's tracker song (py_song.h). */
#include "py_song.h"

void py_song_clear(ps_song *s)
{
    uint8_t *p = (uint8_t *)s;
    uint32_t i;

    for (i = 0; i < sizeof(*s); i++)
        p[i] = PS_EMPTY;
    for (i = 0; i < PS_CHAINS * PS_CROWS; i++)
        s->chain[i / PS_CROWS][i % PS_CROWS].tsp = 0;       /* (pat and snd stay PS_EMPTY) */
    for (i = 0; i < PS_TRACKS; i++) {
        ps_track *t = &s->trk[i];
        uint32_t k;

        for (k = 0; k < sizeof(*t); k++)
            ((uint8_t *)t)[k] = 0;
        t->type = PS_NONE;
        t->sound = PS_EMPTY;
        t->loop = 1;
        t->level = -9;
    }
}

#define is_p(t) py_song_is_pattern(s, t)

/* chain row r if it holds a pattern, else PS_CROWS: an empty row ends the chain */
static int chain_row(const ps_song *s, int c, int r)
{
    return r < PS_CROWS && s->chain[c][r].pat < PS_PATTERNS ? r : PS_CROWS;
}

/* place h on song row `row` (chain row `cr` for P tracks) if it plays something there:
 * 1 = placed, 0 = the cell is empty (end of the column), -1 = nothing playable (skip the row) */
static int place(const ps_song *s, int t, int row, int cr, uint32_t bar, ps_len_fn len, void *ctx,
                 ps_head *h)
{
    uint8_t v;

    if (row >= PS_ROWS)
        return 0;
    v = s->cell[row][t];
    if (v == PS_EMPTY)
        return 0;
    h->row = (uint8_t)row;
    if (is_p(t)) {
        uint32_t n;

        if (v == PS_REST) {                     /* a bar of silence: no pattern */
            h->crow = 0;
            h->pat = PS_EMPTY;
            h->tsp = 0;
            h->snd = PS_EMPTY;
            h->len = bar;
            return 1;
        }
        if (v >= PS_CHAINS)
            return -1;
        cr = chain_row(s, v, cr);
        if (cr >= PS_CROWS)
            return -1;
        h->crow = (uint8_t)cr;
        h->pat = s->chain[v][cr].pat;
        h->tsp = s->chain[v][cr].tsp;
        h->snd = s->chain[v][cr].snd;
        n = len(ctx, t, h->pat);
        h->len = n ? n : bar;                   /* an empty pattern still takes a bar */
        return 1;
    }
    h->crow = 0;
    h->pat = v == PS_ON ? PS_ON : PS_OF;
    h->tsp = 0;
    h->snd = PS_EMPTY;
    h->len = v == PS_ON ? len(ctx, t, 0) : bar;
    if (!h->len)
        h->len = bar;                           /* no such loop: a silent bar */
    return 1;
}

/* from song row `row` / chain row `cr` on: the next playable position, wrapping to start_row once */
static void find(const ps_song *s, int t, int row, int cr, uint32_t bar, ps_len_fn len, void *ctx,
                 ps_head *h)
{
    int i, wrapped = 0;

    for (i = 0; i < 2 * PS_ROWS + 2; i++) {
        int r = place(s, t, row, cr, bar, len, ctx, h);

        if (r > 0) {
            h->active = 1;
            return;
        }
        if (r < 0) {                            /* an unplayable cell: the next row */
            row++;
            cr = 0;
            continue;
        }
        if (wrapped)
            break;                              /* the column is empty from start_row */
        wrapped = 1;
        row = h->start_row;
        cr = 0;
    }
    h->active = 0;
}

int py_song_start(const ps_song *s, int t, int row, uint32_t g, uint32_t bar, ps_len_fn len, void *ctx,
             ps_head *h)
{
    h->start_row = (uint8_t)(row < PS_ROWS ? row : 0);
    h->solo = PS_EMPTY;
    h->cue = PS_EMPTY;
    h->start = g;
    h->len = 0;
    find(s, t, h->start_row, 0, bar, len, ctx, h);
    return h->active;
}

/* place h on row cr of chain c (or where that row's chain ends: nothing); 1 = placed */
static int place_chain(const ps_song *s, int t, int c, int cr, uint32_t bar, ps_len_fn len, void *ctx,
                       ps_head *h)
{
    uint32_t n;

    cr = chain_row(s, c, cr);
    if (cr >= PS_CROWS)
        return 0;
    h->crow = (uint8_t)cr;
    h->pat = s->chain[c][cr].pat;
    h->tsp = s->chain[c][cr].tsp;
    h->snd = s->chain[c][cr].snd;
    n = len(ctx, t, h->pat);
    h->len = n ? n : bar;
    return 1;
}

int py_song_start_chain(const ps_song *s, int t, int c, uint32_t g, uint32_t bar, ps_len_fn len, void *ctx,
                        ps_head *h)
{
    h->row = h->start_row = 0;
    h->solo = (uint8_t)c;
    h->cue = PS_EMPTY;
    h->start = g;
    h->len = 0;
    h->active = (uint8_t)(c >= 0 && c < PS_CHAINS && place_chain(s, t, c, 0, bar, len, ctx, h));
    return h->active;
}

void py_song_next(const ps_song *s, int t, uint32_t bar, ps_len_fn len, void *ctx, ps_head *h)
{
    int row = h->row, cr = h->crow + 1;
    int c = s->cell[row][t];

    if (!h->active)
        return;
    h->start += h->len;
    if (h->solo < PS_CHAINS) {                  /* a chain alone: its next row, else its first */
        if (!place_chain(s, t, h->solo, cr, bar, len, ctx, h))
            h->active = (uint8_t)place_chain(s, t, h->solo, 0, bar, len, ctx, h);
        return;
    }
    if (!is_p(t) || c >= PS_CHAINS || chain_row(s, c, cr) >= PS_CROWS) {
        row++;                                  /* the chain is done: the next song row */
        cr = 0;
        if (h->cue < PS_ROWS) {                 /* live: a cued row instead, the column's start row from now */
            row = h->start_row = h->cue;        /* (an empty cell there: the column stops) */
            h->cue = PS_EMPTY;
        }
    }
    find(s, t, row, cr, bar, len, ctx, h);
}

int py_song_seek(const ps_song *s, int t, uint32_t g, uint32_t bar, ps_len_fn len, void *ctx, ps_head *h)
{
    int i;

    for (i = 0; h->active && g >= h->start + h->len && i < 100000; i++)
        py_song_next(s, t, bar, len, ctx, h);
    return h->active;
}

/* ------------------------------------------------------------------ editing */

int py_song_chain_used(const ps_song *s, int c)
{
    int r, t;

    for (r = 0; r < PS_ROWS; r++)
        for (t = 0; t < PS_TRACKS; t++)
            if (is_p(t) && s->cell[r][t] == c)
                return 1;
    return 0;
}

int py_song_chain_empty(const ps_song *s, int c)
{
    return chain_row(s, c, 0) >= PS_CROWS;
}

int py_song_new_chain(const ps_song *s)
{
    int c;

    for (c = 0; c < PS_CHAINS; c++)
        if (!py_song_chain_used(s, c) && py_song_chain_empty(s, c))
            return c;
    return -1;
}

int py_song_clone_chain(ps_song *s, int c)
{
    int n = py_song_new_chain(s), r;

    if (n < 0 || c < 0 || c >= PS_CHAINS)
        return -1;
    for (r = 0; r < PS_CROWS; r++)
        s->chain[n][r] = s->chain[c][r];
    return n;
}

int py_song_chain_users(const ps_song *s, int c)
{
    int r, t, m = 0;

    for (r = 0; r < PS_ROWS; r++)
        for (t = 0; t < PS_TRACKS; t++)
            if (is_p(t) && s->cell[r][t] == c)
                m |= 1 << t;
    return m;
}

int py_song_col_used(const ps_song *s, int t)
{
    int r;

    for (r = 0; r < PS_ROWS; r++)
        if (s->cell[r][t] != PS_EMPTY)
            return 1;
    return 0;
}

int py_song_quick(const ps_song *s, int t)
{
    int r, k, c = s->cell[0][t], refs = 0;

    for (r = 1; r < PS_ROWS; r++)
        if (s->cell[r][t] != PS_EMPTY)
            return 0;
    if (c == PS_EMPTY)
        return 1;
    if (c >= PS_CHAINS)
        return 0;
    for (r = 1; r < PS_CROWS; r++)
        if (s->chain[c][r].pat < PS_PATTERNS)
            return 0;
    for (r = 0; r < PS_ROWS; r++)
        for (k = 0; k < PS_TRACKS; k++)
            refs += is_p(k) && s->cell[r][k] == c;
    return refs == 1;
}

int py_song_row_insert(ps_song *s, int row)
{
    int r, t;

    if (row < 0 || row >= PS_ROWS)
        return -1;
    for (t = 0; t < PS_TRACKS; t++)
        if (s->cell[PS_ROWS - 1][t] != PS_EMPTY)
            return -1;
    for (r = PS_ROWS - 1; r > row; r--)
        for (t = 0; t < PS_TRACKS; t++)
            s->cell[r][t] = s->cell[r - 1][t];
    for (t = 0; t < PS_TRACKS; t++)
        s->cell[row][t] = PS_EMPTY;
    return 0;
}

void py_song_row_delete(ps_song *s, int row)
{
    int r, t;

    if (row < 0 || row >= PS_ROWS)
        return;
    for (r = row; r < PS_ROWS - 1; r++)
        for (t = 0; t < PS_TRACKS; t++)
            s->cell[r][t] = s->cell[r + 1][t];
    for (t = 0; t < PS_TRACKS; t++)
        s->cell[PS_ROWS - 1][t] = PS_EMPTY;
}

int py_song_crow_insert(ps_song *s, int c, int r)
{
    int i;

    if (c < 0 || c >= PS_CHAINS || r < 0 || r >= PS_CROWS || s->chain[c][PS_CROWS - 1].pat < PS_PATTERNS)
        return -1;
    for (i = PS_CROWS - 1; i > r; i--)
        s->chain[c][i] = s->chain[c][i - 1];
    s->chain[c][r].pat = s->chain[c][r].snd = PS_EMPTY;
    s->chain[c][r].tsp = 0;
    return 0;
}

void py_song_crow_delete(ps_song *s, int c, int r)
{
    int i;

    if (c < 0 || c >= PS_CHAINS || r < 0 || r >= PS_CROWS)
        return;
    for (i = r; i < PS_CROWS - 1; i++)
        s->chain[c][i] = s->chain[c][i + 1];
    s->chain[c][PS_CROWS - 1].pat = s->chain[c][PS_CROWS - 1].snd = PS_EMPTY;
    s->chain[c][PS_CROWS - 1].tsp = 0;
}

int py_song_audio_segs(const ps_song *s, int t, int start_row, uint8_t *seg)
{
    int r, n = 0;

    if (!py_song_col_used(s, t))
        return -1;
    for (r = start_row < 0 ? 0 : start_row; r < PS_ROWS && s->cell[r][t] != PS_EMPTY; r++)
        seg[n++] = s->cell[r][t] == PS_ON ? PS_ON : PS_OF;
    return n;
}

/* ------------------------------------------------------------------ storage */

static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xffffffffu;
    int k;

    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}

void py_song_save(const ps_song *s, uint8_t *buf)
{
    const uint8_t *p = (const uint8_t *)s;
    uint32_t i, c;

    buf[0] = 'P'; buf[1] = 'S'; buf[2] = 'N'; buf[3] = '3';
    for (i = 0; i < sizeof(*s); i++)
        buf[8 + i] = p[i];
    c = crc32(buf + 8, sizeof(*s));
    buf[4] = (uint8_t)c; buf[5] = (uint8_t)(c >> 8); buf[6] = (uint8_t)(c >> 16); buf[7] = (uint8_t)(c >> 24);
}

int py_song_load(ps_song *s, const uint8_t *buf, uint32_t n)
{
    uint8_t *p = (uint8_t *)s;
    uint32_t i, c, body;

    if (n < 8 || buf[0] != 'P' || buf[1] != 'S' || buf[2] != 'N')
        return -1;
    if (buf[3] == '3' && n == PS_FILE_BYTES)
        body = sizeof(*s);
    else if (buf[3] == '2' && n == PS_FILE_BYTES_V2)
        body = PS_FILE_BYTES_V2 - 8;
    else
        return -1;
    c = (uint32_t)buf[4] | (uint32_t)buf[5] << 8 | (uint32_t)buf[6] << 16 | (uint32_t)buf[7] << 24;
    if (c != crc32(buf + 8, body))
        return -1;
    if (buf[3] == '3') {
        for (i = 0; i < sizeof(*s); i++)
            p[i] = buf[8 + i];
        return 0;
    }
    /* PSN2: the cells, chain rows of { pat, tsp }, the tracks */
    buf += 8;
    for (i = 0; i < sizeof(s->cell); i++)
        p[i] = buf[i];
    buf += sizeof(s->cell);
    for (i = 0; i < PS_CHAINS * PS_CROWS; i++) {
        ps_crow *r = &s->chain[i / PS_CROWS][i % PS_CROWS];

        r->pat = buf[2 * i];
        r->tsp = (int8_t)buf[2 * i + 1];
        r->snd = PS_EMPTY;
    }
    buf += 2 * PS_CHAINS * PS_CROWS;
    for (i = 0; i < sizeof(s->trk); i++)
        ((uint8_t *)s->trk)[i] = buf[i];
    return 0;
}
