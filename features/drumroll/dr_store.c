/* DRUM ROLL store: rows for TE's op1.db (dr_store.h). */
#include "dr_store.h"

static uint8_t blob[DR_BLOB_MAX];               /* static, not on the caller's (TE thread) stack:
                                                 * one caller at a time (DRUM ROLL: the main thread) */

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int dr_b64_enc(const uint8_t *in, int n, char *out)
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

int dr_b64_dec(const char *in, uint8_t *out, int max)
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

    blob[0] = 'D';
    blob[1] = 'R';
    blob[2] = DR_STORE_VERSION;
    blob[3] = (uint8_t)id;
    for (i = 0; i < n; i++)
        blob[DR_HDR + i] = s[i];
    return dr_b64_enc(blob, DR_HDR + n, out);
}

int dr_store_pattern(int id, const dr_pattern *p, char *out)
{
    return store(id, p, (int)sizeof(*p), out);
}

int dr_store_settings(const dr_settings *s, char *out)
{
    return store(DR_ROW_SETTINGS, s, (int)sizeof(*s), out);
}

/* blob of exactly n payload bytes for row id, or 0 */
static const uint8_t *load(int id, const char *text, int n)
{
    if (!text || dr_b64_dec(text, blob, (int)DR_BLOB_MAX) != DR_HDR + n)
        return 0;
    if (blob[0] != 'D' || blob[1] != 'R' || blob[2] != DR_STORE_VERSION || blob[3] != id)
        return 0;
    return blob + DR_HDR;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

int dr_load_pattern(int id, const char *text, dr_pattern *out)
{
    const uint8_t *b = load(id, text, (int)sizeof(dr_pattern));
    dr_pattern *p = out;
    int l, i, j;

    if (!b || id < 0 || id >= DR_PATTERNS)
        return -1;
    for (i = 0; i < (int)sizeof(dr_pattern); i++)
        ((uint8_t *)p)[i] = b[i];
    for (l = 0; l < DR_LANES; l++) {            /* every field back in range, whatever was stored */
        dr_lane *ln = &p->lane[l];

        ln->key = (uint8_t)(ln->key % DR_KEYS);
        ln->len = (uint8_t)clampi(ln->len, 1, DR_STEPS);
        ln->rate = (uint8_t)(ln->rate < DR_RATES ? ln->rate : DR_RATE_16);
        ln->level = (uint8_t)clampi(ln->level, 0, 100);
        ln->mute = ln->mute != 0;
        ln->pad[0] = ln->pad[1] = ln->pad[2] = 0;
        for (i = 0; i < DR_STEPS; i++) {
            ln->s[i].on = ln->s[i].on != 0;
            for (j = 0; j < DR_PARAMS; j++)
                ln->s[i].p[j] = (int8_t)dr_param_clamp(j, ln->s[i].p[j]);
        }
    }
    return 0;
}

int dr_load_settings(const char *text, int filters, dr_settings *out)
{
    const uint8_t *b = load(DR_ROW_SETTINGS, text, (int)sizeof(dr_settings));
    int i;

    if (!b)
        return -1;
    for (i = 0; i < (int)sizeof(dr_settings); i++)
        ((uint8_t *)out)[i] = b[i];
    out->cur = (uint8_t)(out->cur % DR_PATTERNS);
    out->swing = (uint8_t)clampi(out->swing, 0, 100);
    out->zoom = (uint8_t)clampi(out->zoom, 0, 2);
    out->pad = 0;
    for (i = 0; i < DR_KEYS; i++) {
        dr_filt *f = &out->filt[i];

        f->type = (uint8_t)(f->type < filters ? f->type : 0);
        f->cut = (uint8_t)clampi(f->cut, 0, 127);
        f->res = (uint8_t)clampi(f->res, 0, 127);
    }
    return 0;
}

uint32_t dr_hash(const void *p, int n)
{
    const uint8_t *b = p;
    uint32_t h = 2166136261u;                   /* FNV-1a */
    int i;

    for (i = 0; i < n; i++)
        h = (h ^ b[i]) * 16777619u;
    return h;
}
