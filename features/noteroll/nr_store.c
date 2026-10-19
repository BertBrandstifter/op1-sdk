/* NOTE ROLL store: rows for TE's op1.db (nr_store.h). */
#include "nr_store.h"

static uint8_t blob[NR_BLOB_MAX];               /* static, not on the caller's (TE thread) stack:
                                                 * one caller at a time (NOTE ROLL: the main thread) */

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int nr_b64_enc(const uint8_t *in, int n, char *out)
{
    int i, o = 0;

    for (i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0)
                     | (i + 2 < n ? in[i + 2] : 0);

        out[o++] = b64[v >> 18 & 63];
        out[o++] = b64[v >> 12 & 63];
        out[o++] = i + 1 < n ? b64[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? b64[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}

int nr_b64_dec(const char *in, uint8_t *out, int max)
{
    int o = 0, i;

    for (;;) {
        int v[4], pad = 0;
        uint32_t w;

        if (!in[0])
            return o;
        for (i = 0; i < 4; i++) {
            if (!in[i])
                return -1;                      /* not a multiple of 4 */
            if (in[i] == '=' && i >= 2) {
                v[i] = 0;
                pad++;
            } else if (pad || (v[i] = b64_val(in[i])) < 0) {
                return -1;
            }
        }
        in += 4;
        if (pad && in[0])
            return -1;                          /* padding only at the end */
        w = (uint32_t)v[0] << 18 | (uint32_t)v[1] << 12 | (uint32_t)v[2] << 6 | (uint32_t)v[3];
        for (i = 0; i < 3 - pad; i++) {
            if (o >= max)
                return -1;
            out[o++] = (uint8_t)(w >> (16 - 8 * i));
        }
    }
}

static int store(int id, const void *payload, int n, char *out)
{
    const uint8_t *s = payload;
    int i;

    blob[0] = 'N';
    blob[1] = 'R';
    blob[2] = NR_STORE_VERSION;
    blob[3] = (uint8_t)id;
    for (i = 0; i < n; i++)
        blob[NR_HDR + i] = s[i];
    return nr_b64_enc(blob, NR_HDR + n, out);
}

int nr_store_pattern(int id, const nr_pattern *p, char *out)
{
    return store(id, p, (int)sizeof(*p), out);
}

int nr_store_settings(const nr_settings *s, char *out)
{
    return store(NR_ROW_SETTINGS, s, (int)sizeof(*s), out);
}

/* blob of exactly n payload bytes for row id, or 0 */
static const uint8_t *load(int id, const char *text, int n)
{
    if (!text || nr_b64_dec(text, blob, (int)NR_BLOB_MAX) != NR_HDR + n)
        return 0;
    if (blob[0] != 'N' || blob[1] != 'R' || blob[2] != NR_STORE_VERSION || blob[3] != id)
        return 0;
    return blob + NR_HDR;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

int nr_load_pattern(int id, const char *text, nr_pattern *out)
{
    const uint8_t *b = load(id, text, (int)sizeof(nr_pattern));
    int i, v;

    if (!b || id < 0 || id >= NR_PATTERNS)
        return -1;
    for (i = 0; i < (int)sizeof(nr_pattern); i++)
        ((uint8_t *)out)[i] = b[i];
    out->len = (uint8_t)clampi(out->len, 1, NR_STEPS);    /* every field back in range */
    out->pad[0] = out->pad[1] = out->pad[2] = 0;
    for (i = 0; i < NR_STEPS; i++) {
        nr_step *s = &out->s[i];

        for (v = 0; v < NR_VOICES; v++) {
            nr_note *n = &s->n[v];

            if (n->key > 127)
                n->key = NR_NONE;
            n->vel = (uint8_t)clampi(n->vel, 1, 100);
            n->len = (uint8_t)(n->len < NR_LENS ? n->len : NR_LEN_DEF);
            n->pad = 0;
        }
        nr_sort(s);
        s->nudge = (int8_t)clampi(s->nudge, -50, 50);
        s->prob = (uint8_t)clampi(s->prob, 0, 100);
        s->ratch = (uint8_t)clampi(s->ratch, 1, 4);
        s->pad = 0;
    }
    return 0;
}

int nr_load_settings(const char *text, nr_settings *out)
{
    const uint8_t *b = load(NR_ROW_SETTINGS, text, (int)sizeof(nr_settings));
    int i;

    if (!b)
        return -1;
    for (i = 0; i < (int)sizeof(nr_settings); i++)
        ((uint8_t *)out)[i] = b[i];
    out->cur = (uint8_t)(out->cur % NR_PATTERNS);
    out->swing = (uint8_t)clampi(out->swing, 0, 100);
    out->zoom = (uint8_t)clampi(out->zoom, 0, 4);
    out->pad = out->pad2[0] = out->pad2[1] = 0;
    out->owned[1] &= (uint8_t)((1 << (NR_PATTERNS - 8)) - 1);
    return 0;
}

uint32_t nr_hash(const void *p, int n)
{
    const uint8_t *b = p;
    uint32_t h = 2166136261u;                   /* FNV-1a */
    int i;

    for (i = 0; i < n; i++)
        h = (h ^ b[i]) * 16777619u;
    return h;
}
