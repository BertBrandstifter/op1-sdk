/* NOTE ROLL core: data model, chords, scheduler (nr_core.h). */
#include "nr_core.h"

const uint8_t nr_len8[NR_LENS] = { 1, 2, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 0 };
const char *const nr_len_name[NR_LENS] = {
    "1/8", "1/4", "1/2", "3/4", "1", "1.5", "2", "3", "4", "6", "8", "12", "16", "TIE",
};
/* NR_LEN index of 1 step is 4; "TIE" (last): held until the next note of the voice's pitch or the
 * end of the pattern (the scheduler treats it as the pattern's length) */

static const int8_t shape_iv[NR_SHAPES][NR_VOICES] = {    /* intervals over the root, -1 end */
    { 0, -1 },                          /* single */
    { 0, 4, 7, -1 },                    /* MAJ */
    { 0, 3, 7, -1 },                    /* MIN */
    { 0, 2, 7, -1 },                    /* SUS2 */
    { 0, 5, 7, -1 },                    /* SUS4 */
    { 0, 4, 7, 10, -1 },                /* 7 */
    { 0, 4, 7, 11, -1 },                /* MAJ7 */
    { 0, 3, 7, 10, -1 },                /* MIN7 */
    { 0, 3, 6, -1 },                    /* DIM */
    { 0, 4, 8, -1 },                    /* AUG */
    { 0, 7, -1 },                       /* 5 */
    { 0, 12, -1 },                      /* OCT */
};
const char *const nr_shape_name[NR_SHAPES] = {
    "NOTE", "MAJ", "MIN", "SUS2", "SUS4", "7", "MAJ7", "MIN7", "DIM", "AUG", "5", "OCT",
};

void nr_step_init(nr_step *s)
{
    int v;

    for (v = 0; v < NR_VOICES; v++) {
        s->n[v].key = NR_NONE;
        s->n[v].vel = 100;
        s->n[v].len = NR_LEN_DEF;
        s->n[v].pad = 0;
    }
    s->nudge = 0;
    s->prob = 100;
    s->ratch = 1;
    s->pad = 0;
}

void nr_pattern_init(nr_pattern *p)
{
    int i;

    p->len = 16;
    p->pad[0] = p->pad[1] = p->pad[2] = 0;
    for (i = 0; i < NR_STEPS; i++)
        nr_step_init(&p->s[i]);
}

int nr_voices(const nr_step *s)
{
    int v, n = 0;

    for (v = 0; v < NR_VOICES; v++)
        n += s->n[v].key != NR_NONE;
    return n;
}

void nr_sort(nr_step *s)
{
    int i, j;

    for (i = 1; i < NR_VOICES; i++) {           /* insertion sort, NR_NONE (255) sorts last */
        nr_note t = s->n[i];

        for (j = i; j > 0 && s->n[j - 1].key > t.key; j--)
            s->n[j] = s->n[j - 1];
        s->n[j] = t;
    }
    for (i = 1; i < NR_VOICES; i++)
        if (s->n[i].key != NR_NONE && s->n[i].key == s->n[i - 1].key) {
            for (j = i; j < NR_VOICES - 1; j++)
                s->n[j] = s->n[j + 1];
            s->n[NR_VOICES - 1].key = NR_NONE;
            i--;
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

int nr_add(nr_step *s, int key, int vel, int len)
{
    int v;

    if (key < 0 || key > 127 || find(s, key) >= 0 || nr_voices(s) >= NR_VOICES)
        return -1;
    v = nr_voices(s);
    s->n[v].key = (uint8_t)key;
    s->n[v].vel = (uint8_t)(vel < 1 ? 1 : vel > 100 ? 100 : vel);
    s->n[v].len = (uint8_t)(len < 0 || len >= NR_LENS ? NR_LEN_DEF : len);
    nr_sort(s);
    return find(s, key);
}

void nr_remove(nr_step *s, int voice)
{
    if (voice < 0 || voice >= NR_VOICES)
        return;
    s->n[voice].key = NR_NONE;
    nr_sort(s);
}

int nr_set_key(nr_step *s, int voice, int key)
{
    key = key < 0 ? 0 : key > 127 ? 127 : key;
    if (voice < 0 || voice >= NR_VOICES || s->n[voice].key == NR_NONE)
        return voice;
    if (find(s, key) >= 0)
        return voice;
    s->n[voice].key = (uint8_t)key;
    nr_sort(s);
    return find(s, key);
}

int nr_chord(nr_step *s, int sh)
{
    nr_step was = *s;
    nr_note root = s->n[0];
    int i, v;

    if (root.key == NR_NONE || sh < 0 || sh >= NR_SHAPES)
        return 0;
    for (v = 0; v < NR_VOICES; v++)
        s->n[v].key = NR_NONE;
    for (i = v = 0; i < NR_VOICES && shape_iv[sh][i] >= 0; i++)
        if (root.key + shape_iv[sh][i] <= 127) {
            s->n[v] = root;
            s->n[v++].key = (uint8_t)(root.key + shape_iv[sh][i]);
        }
    nr_sort(s);
    for (v = 0; v < NR_VOICES; v++)
        if (was.n[v].key != s->n[v].key)
            return 1;
    return 0;
}

int nr_shape_of(const nr_step *s)
{
    int sh, i, n = nr_voices(s);

    for (sh = 0; sh < NR_SHAPES && n; sh++) {
        for (i = 0; i < n && shape_iv[sh][i] >= 0 && s->n[i].key - s->n[0].key == shape_iv[sh][i]; i++)
            ;
        if (i == n && (i == NR_VOICES || shape_iv[sh][i] < 0))
            return sh;
    }
    return -1;
}

int nr_invert(nr_step *s, int up)
{
    int n = nr_voices(s), v = up ? 0 : n - 1, key;

    if (n < 2)
        return 0;
    key = s->n[v].key + (up ? 12 : -12);
    if (key < 0 || key > 127 || find(s, key) >= 0)
        return 0;
    s->n[v].key = (uint8_t)key;
    nr_sort(s);
    return 1;
}

uint64_t nr_step_q16(uint32_t bpm_x100)
{
    /* a 1/16 = a quarter / 4 = sr * 60 / bpm / 4 = sr * 1500 / bpm_x100 */
    return bpm_x100 ? ((uint64_t)NR_SR * 1500 << 16) / bpm_x100 : 0;
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

int nr_schedule(const nr_pattern *p, uint32_t from, uint32_t n, uint64_t L, int swing,
                uint32_t seed, nr_emit_fn emit, void *ctx)
{
    uint32_t ls = (uint32_t)(L >> 16), k, k0, k1, len, si, hq;
    int32_t sw;
    int count = 0;

    if (!ls)
        return 0;
    len = p->len < 1 ? 1 : p->len > NR_STEPS ? NR_STEPS : p->len;
    sw = (int32_t)(ls * (uint32_t)swing / 200);             /* swing 100 = half a step late */
    k0 = from / ls;                             /* a note lands at most half a step early (nudge) and
                                                 * under two steps late (swing + nudge + ratchet) */
    k1 = k0 + 1;
    for (hq = from - k0 * ls + n; hq >= ls; hq -= ls)
        k1++;
    k0 = k0 < 2 ? 0 : k0 - 2;
    for (k = k0, si = k0 % len; k <= k1; k++, si = si + 1 == len ? 0 : si + 1) {
        const nr_step *s = &p->s[si];
        int32_t base, off;
        int r, h, v;

        if (s->n[0].key == NR_NONE)
            continue;
        if (s->prob < 100 && (int)(mix(seed ^ k * 0x9e3779b1u) % 100) >= s->prob)
            continue;
        r = s->ratch < 1 ? 1 : s->ratch > 4 ? 4 : s->ratch;
        base = (int32_t)(((uint64_t)k * L) >> 16);
        off = (k & 1 ? sw : 0) + (int32_t)ls * s->nudge / 100;
        for (v = 0; v < NR_VOICES && s->n[v].key != NR_NONE; v++) {
            const nr_note *nt = &s->n[v];
            int li = nt->len < NR_LENS ? nt->len : NR_LEN_DEF;
            uint32_t hold = li == NR_LENS - 1 ? ls * len : ls * nr_len8[li] / 8;

            if (r > 1 && hold > ls / (uint32_t)r)
                hold = ls / (uint32_t)r;        /* a ratchet hit ends before the next one */
            hold = hold < 64 ? 64 : hold;       /* at least one audio block */
            for (h = 0; h < r; h++) {
                int32_t t = base + off + (int32_t)(ls * (uint32_t)h / (uint32_t)r);
                nr_hit hit;

                if (t < (int32_t)from || t - (int32_t)from >= (int32_t)n)
                    continue;
                hit.step = (int)si;
                hit.voice = v;
                hit.hit = h;
                hit.k = k;
                hit.at = (uint32_t)t;
                hit.hold = hold;
                hit.n = nt;
                emit(ctx, &hit);
                count++;
            }
        }
    }
    return count;
}

int nr_playhead(const nr_pattern *p, uint32_t t, uint64_t step)
{
    uint32_t len = p->len < 1 ? 1 : p->len > NR_STEPS ? NR_STEPS : p->len, ls = (uint32_t)(step >> 16);

    return ls ? (int)((t / ls) % len) : -1;
}
