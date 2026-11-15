/* py_song: PLAY's tracker song (py_song.h). */
#include "py_song.h"

void py_song_clear(ps_song *s)
{
    uint8_t *p = (uint8_t *)s;
    uint32_t i;

    for (i = 0; i < sizeof(*s); i++)
        p[i] = PS_EMPTY;
    for (i = 0; i < PS_CHAINS * PS_CROWS; i++)
        s->chain[i / PS_CROWS][i % PS_CROWS].tsp = 0;
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

        if (v >= PS_CHAINS)
            return -1;
        cr = chain_row(s, v, cr);
        if (cr >= PS_CROWS)
            return -1;
        h->crow = (uint8_t)cr;
        h->pat = s->chain[v][cr].pat;
        h->tsp = s->chain[v][cr].tsp;
        n = len(ctx, t, h->pat);
        h->len = n ? n : bar;                   /* an empty pattern still takes a bar */
        return 1;
    }
    h->crow = 0;
    h->pat = v == PS_ON ? PS_ON : PS_OF;
    h->tsp = 0;
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
    h->start = g;
    h->len = 0;
    find(s, t, h->start_row, 0, bar, len, ctx, h);
    return h->active;
}

void py_song_next(const ps_song *s, int t, uint32_t bar, ps_len_fn len, void *ctx, ps_head *h)
{
    int row = h->row, cr = h->crow + 1;
    int c = s->cell[row][t];

    if (!h->active)
        return;
    h->start += h->len;
    if (!is_p(t) || c >= PS_CHAINS || chain_row(s, c, cr) >= PS_CROWS) {
        row++;                                  /* the chain is done: the next song row */
        cr = 0;
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

    buf[0] = 'P'; buf[1] = 'S'; buf[2] = 'N'; buf[3] = '2';
    for (i = 0; i < sizeof(*s); i++)
        buf[8 + i] = p[i];
    c = crc32(buf + 8, sizeof(*s));
    buf[4] = (uint8_t)c; buf[5] = (uint8_t)(c >> 8); buf[6] = (uint8_t)(c >> 16); buf[7] = (uint8_t)(c >> 24);
}

int py_song_load(ps_song *s, const uint8_t *buf, uint32_t n)
{
    uint8_t *p = (uint8_t *)s;
    uint32_t i, c;

    if (n != PS_FILE_BYTES || buf[0] != 'P' || buf[1] != 'S' || buf[2] != 'N' || buf[3] != '2')
        return -1;
    c = (uint32_t)buf[4] | (uint32_t)buf[5] << 8 | (uint32_t)buf[6] << 16 | (uint32_t)buf[7] << 24;
    if (c != crc32(buf + 8, sizeof(*s)))
        return -1;
    for (i = 0; i < sizeof(*s); i++)
        p[i] = buf[8 + i];
    return 0;
}
