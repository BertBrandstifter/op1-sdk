/* py_slot: PLAY's song slots, the file format (py_slot.h). */
#include "py_slot.h"

#define ALIGN4      __attribute__((aligned(4)))     /* yaffs reads / writes: even addresses */
#define GET_TRIES   8                           /* a pattern being edited while it is copied: again */

static const uint8_t magic[4] = { 'P', 'S', 'L', '2' };
static const uint8_t magic1[4] = { 'P', 'S', 'L', '1' };   /* v0.10 / v0.11: no sounds */

static uint32_t crc_add(uint32_t c, const uint8_t *p, uint32_t n)
{
    int k;

    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return c;
}

static void put16(uint8_t *b, uint32_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *b, uint32_t v) { put16(b, v); put16(b + 2, v >> 16); }
static uint32_t get16(const uint8_t *b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8; }
static uint32_t get32(const uint8_t *b) { return get16(b) | get16(b + 2) << 16; }

static int same(const void *a, const void *b, uint32_t n)
{
    const uint8_t *x = a, *y = b;

    while (n--)
        if (*x++ != *y++)
            return 0;
    return 1;
}

static int bits(uint32_t m)
{
    int n = 0;

    for (; m; m >>= 1)
        n += (int)(m & 1);
    return n;
}

static uint32_t file_bytes(const psl_io *io, const uint16_t mask[2], int smask, int hdr)
{
    return (uint32_t)hdr + PS_FILE_BYTES + (uint32_t)bits(mask[0]) * io->size[0]
           + (uint32_t)bits(mask[1]) * io->size[1] + (uint32_t)bits((uint32_t)smask) * PSL_SND + 4;
}

/* pattern p of a kind as it plays now into io->pat (a few tries: it may be edited meanwhile) */
static int get_now(psl_io *io, int kind, int p)
{
    int i;

    for (i = 0; i < GET_TRIES && io->get(io->ctx, kind, p, io->pat); i++)
        ;
    return i < GET_TRIES;
}

void py_slot_summary(const ps_song *s, psl_info *info, int cur)
{
    int r, t, c, k;

    for (t = 0; t < PS_TRACKS; t++)
        info->type[t] = s->trk[t].type <= PS_DRUM ? s->trk[t].type : PS_NONE;
    info->rows = 0;
    for (r = 0; r < PS_ROWS; r++)
        for (t = 0; t < PS_TRACKS; t++)
            if (s->trk[t].type != PS_NONE && s->cell[r][t] != PS_EMPTY)
                info->rows = (uint8_t)(r + 1);
    for (c = k = 0; c < PS_CHAINS; c++)
        k += !py_song_chain_empty(s, c);
    info->chains = (uint8_t)k;
    info->used[0] = info->used[1] = 0;
    info->smask = 0;
    for (t = 0; t < PS_TRACKS; t++) {
        int synth = s->trk[t].type == PS_SYNTH;

        if (!py_song_is_pattern(s, t))
            continue;
        if (synth)                              /* its own sound (CUR: TE's current one) */
            info->smask |= (uint8_t)(1u << (s->trk[t].sound < PSL_SOUNDS ? s->trk[t].sound : cur & 7));
        for (r = 0; r < PS_ROWS; r++) {
            c = s->cell[r][t];
            if (c >= PS_CHAINS)
                continue;                       /* empty, a rest */
            for (k = 0; k < PS_CROWS; k++) {
                const ps_crow *w = &s->chain[c][k];

                if (w->pat < PSL_PATS)
                    info->used[!synth] |= (uint16_t)(1u << w->pat);
                if (synth && w->pat < PSL_PATS && w->snd < PSL_SOUNDS)
                    info->smask |= (uint8_t)(1u << w->snd);     /* a chain row's own sound */
            }
        }
    }
}

int py_slot_save(const ps_song *s, const uint8_t swing[2], psl_io *io, uint8_t *songbuf)
{
    uint8_t hdr[PSL_HDR] ALIGN4, tail[4] ALIGN4, rec[PSL_SND] ALIGN4;
    psl_info info;
    uint16_t mask[2] = { 0, 0 };
    uint32_t crc = 0xffffffffu;
    int kind, p, i;

    /* first pass: which patterns are not empty (the second pass writes what it reads then: a
     * pattern that changes in between is written as it is then, the mask stays) */
    for (kind = 0; kind < 2; kind++)
        for (p = 0; p < PSL_PATS; p++) {
            if (!get_now(io, kind, p))
                return PSL_BUSY;
            if (!same(io->pat, io->empty[kind], io->size[kind]))
                mask[kind] |= (uint16_t)(1u << p);
        }
    py_slot_summary(s, &info, io->cur_sound);
    for (i = 0; i < PSL_HDR; i++)
        hdr[i] = 0;
    for (i = 0; i < 4; i++)
        hdr[i] = magic[i];
    put16(hdr + 4, io->size[0]);
    put16(hdr + 6, io->size[1]);
    put32(hdr + 8, file_bytes(io, mask, info.smask, PSL_HDR));
    put16(hdr + 12, mask[0]);
    put16(hdr + 14, mask[1]);
    for (i = 0; i < PS_TRACKS; i++)
        hdr[16 + i] = info.type[i];
    hdr[24] = info.rows;
    hdr[25] = info.chains;
    hdr[26] = swing[0];
    hdr[27] = swing[1];
    put16(hdr + 28, info.used[0] | 0x8000u);
    put16(hdr + 30, info.used[1] | 0x8000u);
    hdr[32] = info.smask;
    py_song_save(s, songbuf);
    crc = crc_add(crc, hdr, PSL_HDR);
    crc = crc_add(crc, songbuf, PS_FILE_BYTES);
    if (io->write(io->ctx, hdr, PSL_HDR) || io->write(io->ctx, songbuf, PS_FILE_BYTES))
        return PSL_IO;
    for (kind = 0; kind < 2; kind++)
        for (p = 0; p < PSL_PATS; p++) {
            if (!((mask[kind] >> p) & 1))
                continue;
            if (!get_now(io, kind, p))
                return PSL_BUSY;
            crc = crc_add(crc, io->pat, io->size[kind]);
            if (io->write(io->ctx, io->pat, io->size[kind]))
                return PSL_IO;
        }
    for (p = 0; p < PSL_SOUNDS; p++) {
        if (!((info.smask >> p) & 1))
            continue;
        for (i = 0; i < PSL_SND; i++)
            rec[i] = 0;
        if (io->sget(io->ctx, p, rec))
            return PSL_BUSY;
        crc = crc_add(crc, rec, PSL_SND);
        if (io->write(io->ctx, rec, PSL_SND))
            return PSL_IO;
    }
    put32(tail, ~crc);
    return io->write(io->ctx, tail, 4) ? PSL_IO : PSL_OK;
}

static int parse(const uint8_t *hdr, int n, const psl_io *io, psl_info *info)
{
    int i;

    if (n <= 0)
        return PSL_NOFILE;
    if (n >= PSL_HDR1 && same(hdr, magic1, 4)) {
        info->hdr = PSL_HDR1;
        info->smask = 0;
    } else if (n >= PSL_HDR && same(hdr, magic, 4)) {
        info->hdr = PSL_HDR;
        info->smask = hdr[32];
    } else {
        return PSL_BAD;
    }
    info->sbad = 0;
    if (get16(hdr + 4) != io->size[0] || get16(hdr + 6) != io->size[1])
        return PSL_OTHER;                       /* written by another version of the editors */
    info->mask[0] = (uint16_t)get16(hdr + 12);
    info->mask[1] = (uint16_t)get16(hdr + 14);
    info->bytes = get32(hdr + 8);
    if ((info->mask[0] | info->mask[1]) >> PSL_PATS || info->bytes != file_bytes(io, info->mask, info->smask, info->hdr))
        return PSL_BAD;
    for (i = 0; i < PS_TRACKS; i++)
        info->type[i] = hdr[16 + i] <= PS_DRUM ? hdr[16 + i] : PS_NONE;
    info->rows = hdr[24];
    info->chains = hdr[25];
    info->swing[0] = hdr[26] > 100 ? 100 : hdr[26];
    info->swing[1] = hdr[27] > 100 ? 100 : hdr[27];
    for (i = 0; i < 2; i++)                     /* (v0.10.0: not written, the stored ones) */
        info->used[i] = get16(hdr + 28 + 2 * i) & 0x8000u ? (uint16_t)(get16(hdr + 28 + 2 * i) & 0x3fffu)
                                                          : info->mask[i];
    return PSL_OK;
}

int py_slot_peek(psl_io *io, psl_info *info)
{
    uint8_t hdr[PSL_HDR] ALIGN4;

    return parse(hdr, io->read(io->ctx, 0, hdr, PSL_HDR), io, info);    /* (PSL1: 40 bytes are there too) */
}

int py_slot_check(psl_io *io, psl_info *info)
{
    uint8_t hdr[PSL_HDR] ALIGN4, tail[4] ALIGN4;
    uint32_t crc = 0xffffffffu, off, chunk = io->size[0] < io->size[1] ? io->size[1] : io->size[0];
    int r = py_slot_peek(io, info);

    if (r)
        return r;
    if (io->read(io->ctx, 0, hdr, info->hdr) != info->hdr)
        return PSL_BAD;
    crc = crc_add(crc, hdr, info->hdr);
    for (off = info->hdr; off < info->bytes - 4; off += chunk) {
        uint32_t n = info->bytes - 4 - off < chunk ? info->bytes - 4 - off : chunk;

        if ((uint32_t)io->read(io->ctx, off, io->pat, n) != n)
            return PSL_BAD;
        crc = crc_add(crc, io->pat, n);
    }
    if (io->read(io->ctx, info->bytes - 4, tail, 4) != 4 || get32(tail) != ~crc)
        return PSL_BAD;
    if (io->read(io->ctx, info->bytes, tail, 1) > 0)
        return PSL_BAD;                         /* longer than it says */
    return PSL_OK;
}

int py_slot_diff(psl_io *io, psl_info *info, uint16_t diff[3])
{
    uint8_t now[PSL_SND] ALIGN4;
    uint32_t off = (uint32_t)PS_FILE_BYTES, crc;
    int r = py_slot_check(io, info), kind, p;

    diff[0] = diff[1] = diff[2] = 0;
    if (r)
        return r;
    off += info->hdr;
    for (kind = 0; kind < 2; kind++)
        for (p = 0; p < PSL_PATS; p++) {
            if (!get_now(io, kind, p))
                return PSL_BUSY;
            if (!((info->mask[kind] >> p) & 1)) {
                if (!same(io->pat, io->empty[kind], io->size[kind]))
                    diff[kind] |= (uint16_t)(1u << p);
                continue;
            }
            crc = crc_add(0xffffffffu, io->pat, io->size[kind]);    /* (one scratch: the two CRCs) */
            if ((uint32_t)io->read(io->ctx, off, io->pat, io->size[kind]) != io->size[kind])
                return PSL_IO;
            off += io->size[kind];
            if (crc_add(0xffffffffu, io->pat, io->size[kind]) != crc)
                diff[kind] |= (uint16_t)(1u << p);
        }
    for (p = 0; p < PSL_SOUNDS; p++) {
        if (!((info->smask >> p) & 1))
            continue;
        for (r = 0; r < PSL_SND; r++)
            now[r] = 0;
        if (io->sget(io->ctx, p, now))
            return PSL_BUSY;
        if (io->read(io->ctx, off, io->pat, PSL_SND) != PSL_SND)
            return PSL_IO;
        off += PSL_SND;
        if (!same(now, io->pat, PSL_SND))
            diff[2] |= (uint16_t)(1u << p);
        if (!io->sok(io->ctx, p, io->pat))
            info->sbad |= (uint8_t)(1u << p);
    }
    return PSL_OK;
}

int py_slot_load(ps_song *s, uint8_t swing[2], psl_io *io, uint8_t *songbuf, const uint16_t *keep)
{
    psl_info info;
    uint32_t off;
    int r = py_slot_check(io, &info), kind, p;

    if (r)
        return r;
    off = info.hdr;
    if (io->read(io->ctx, off, songbuf, PS_FILE_BYTES) != (int)PS_FILE_BYTES
        || py_song_load(s, songbuf, PS_FILE_BYTES))
        return PSL_BAD;
    off += PS_FILE_BYTES;
    swing[0] = info.swing[0];
    swing[1] = info.swing[1];
    for (kind = 0; kind < 2; kind++)
        for (p = 0; p < PSL_PATS; p++) {
            int stored = (info.mask[kind] >> p) & 1, kept = keep && ((keep[kind] >> p) & 1);

            if (stored && !kept) {
                if ((uint32_t)io->read(io->ctx, off, io->pat, io->size[kind]) != io->size[kind])
                    return PSL_IO;              /* (checked a moment ago: the flash failed now) */
                io->put(io->ctx, kind, p, io->pat);
            } else if (!kept) {
                io->put(io->ctx, kind, p, io->empty[kind]);
            }
            if (stored)
                off += io->size[kind];
        }
    for (p = 0; p < PSL_SOUNDS; p++) {
        if (!((info.smask >> p) & 1))
            continue;
        if (!(keep && ((keep[2] >> p) & 1))) {
            if (io->read(io->ctx, off, io->pat, PSL_SND) != PSL_SND)
                return PSL_IO;
            if (io->sok(io->ctx, p, io->pat))
                io->sput(io->ctx, p, io->pat);  /* (refused: the slot keeps what it has) */
        }
        off += PSL_SND;
    }
    return PSL_OK;
}

int py_slot_pick(const char *name, uint16_t taken, uint16_t same_as)
{
    static const char want[] = "SONG##.PSL";
    int i, n = 0;

    if (same_as)
        return -2;
    for (i = 0; want[i]; i++) {
        int c = name[i] >= 'a' && name[i] <= 'z' ? name[i] - 32 : name[i];

        if (want[i] == '#') {
            if (c < '0' || c > '9')
                break;
            n = n * 10 + c - '0';
        } else if (c != want[i]) {
            break;
        }
    }
    if (!want[i] && !name[i] && n >= 1 && n <= PSL_SLOTS && !((taken >> (n - 1)) & 1))
        return n - 1;
    for (i = 0; i < PSL_SLOTS; i++)
        if (!((taken >> i) & 1))
            return i;
    return -1;
}
