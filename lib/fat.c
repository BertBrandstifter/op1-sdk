/* op1-sdk library "fat": read-only FAT16 / FAT32 reader, see include/op1/fat.h. */
#include <op1/fat.h>

#define SECTOR 512u

static uint32_t le16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return le16(p) | le16(p + 2) << 16; }

static int rd(fat_vol *v, uint32_t off, void *buf, uint32_t n)
{
    return v->read(v->ctx, off, buf, n) == (int)n ? 0 : FAT_EIO;
}

/* ------------------------------------------------------------------ volume */

static int is_bpb(const uint8_t *b)
{
    uint32_t bps = le16(b + 11), spc = b[13];

    return (b[0] == 0xeb || b[0] == 0xe9)
        && (bps == 512 || bps == 1024 || bps == 2048 || bps == 4096)
        && spc && !(spc & (spc - 1)) && le16(b + 14) && (b[16] == 1 || b[16] == 2);
}

int fat_mount(fat_vol *v, fat_read_fn read, void *ctx)
{
    uint8_t b[SECTOR] __attribute__((aligned(4)));  /* yaffs reads: even addresses */
    uint32_t bps, spc, rsvd, nfats, rootents, tot, fatsz, rootsecs, meta, nclus, need;

    v->read = read;
    v->ctx = ctx;
    v->base = 0;
    v->fat_cache_at = ~0u;
    if (rd(v, 0, b, SECTOR))
        return FAT_EIO;
    if (!is_bpb(b)) {                           /* an MBR: the first FAT partition */
        uint32_t lba;
        int i;

        if (b[510] != 0x55 || b[511] != 0xaa)
            return FAT_ENOFS;
        for (i = 0; i < 4; i++) {
            const uint8_t *p = b + 0x1be + 16 * i;
            uint8_t t = p[4];

            if (t == 0x01 || t == 0x04 || t == 0x06 || t == 0x0b || t == 0x0c || t == 0x0e)
                break;
        }
        if (i == 4)
            return FAT_ENOFS;
        lba = le32(b + 0x1be + 16 * i + 8);
        if (!lba || lba > 0x7fffffffu / SECTOR)
            return FAT_EGEOM;
        v->base = lba * SECTOR;
        if (rd(v, v->base, b, SECTOR))
            return FAT_EIO;
        if (!is_bpb(b))
            return FAT_ENOFS;
    }
    bps = le16(b + 11);
    spc = b[13];
    rsvd = le16(b + 14);
    nfats = b[16];
    rootents = le16(b + 17);
    tot = le16(b + 19) ? le16(b + 19) : le32(b + 32);
    fatsz = le16(b + 22) ? le16(b + 22) : le32(b + 36);
    if (!tot || !fatsz || bps * spc > 65536u || tot > 0x7fffffffu / bps)
        return FAT_EGEOM;
    rootsecs = (rootents * 32 + bps - 1) / bps;
    if (fatsz > tot || rsvd + nfats * fatsz + rootsecs >= tot)
        return FAT_EGEOM;
    meta = rsvd + nfats * fatsz + rootsecs;
    nclus = (tot - meta) / spc;
    if (nclus < 4085)
        return FAT_EFAT12;
    v->type = nclus < 65525 ? 16 : 32;
    need = (nclus + 2) * (v->type / 8);
    if (fatsz * bps < need)
        return FAT_EGEOM;
    v->bps = (uint16_t)bps;
    v->spc = (uint8_t)spc;
    v->clus_bytes = bps * spc;
    v->nclus = nclus;
    v->fat_off = v->base + rsvd * bps;
    v->root_off = v->fat_off + nfats * fatsz * bps;
    v->root_bytes = rootsecs * bps;
    v->data_off = v->root_off + v->root_bytes;
    v->root_clus = 0;
    if (v->type == 32) {
        v->root_clus = le32(b + 44);
        if (v->root_clus < 2 || v->root_clus > nclus + 1)
            return FAT_EGEOM;
    } else if (!rootents) {
        return FAT_EGEOM;
    }
    return FAT_OK;
}

uint32_t fat_next(fat_vol *v, uint32_t clus)
{
    uint32_t off, blk, val;

    if (clus < 2 || clus > v->nclus + 1)
        return 1;
    off = v->fat_off + clus * (v->type / 8u);
    blk = off & ~(SECTOR - 1);
    if (blk != v->fat_cache_at) {
        v->fat_cache_at = ~0u;
        if (rd(v, blk, v->fat_cache, SECTOR))
            return 1;
        v->fat_cache_at = blk;
    }
    if (v->type == 16) {
        val = le16(v->fat_cache + (off - blk));
        if (val >= 0xfff8)
            return 0;
    } else {
        val = le32(v->fat_cache + (off - blk)) & 0x0fffffffu;
        if (val >= 0x0ffffff8u)
            return 0;
    }
    return val < 2 || val > v->nclus + 1 ? 1 : val;
}

static uint32_t clus_off(fat_vol *v, uint32_t c)
{
    return v->data_off + (c - 2) * v->clus_bytes;
}

/* ------------------------------------------------------------------ directories */

static void short_name(const uint8_t *d, char *out)
{
    int i, n = 0;

    for (i = 0; i < 8 && d[i] != ' '; i++)
        out[n++] = (char)(i == 0 && d[0] == 0x05 ? 0xe5 : d[i]);
    if (d[8] != ' ') {
        out[n++] = '.';
        for (i = 8; i < 11 && d[i] != ' '; i++)
            out[n++] = (char)d[i];
    }
    out[n] = 0;
}

/* walk the raw 32-byte entries of dir; fn returns nonzero to stop. Returns entries seen or < 0 */
typedef int (*raw_fn)(void *ctx, const uint8_t *d);

static int dir_walk_any(fat_vol *v, uint32_t dir, raw_fn fn, void *ctx, int raw)
{
    uint8_t b[SECTOR] __attribute__((aligned(4)));  /* yaffs reads: even addresses */
    uint32_t clus = dir ? dir : v->root_clus, pos, len, i, seen = 0, hops = 0;
    int fixed = !dir && v->type == 16;

    for (;;) {
        if (fixed) {
            pos = v->root_off;
            len = v->root_bytes;
        } else {
            if (clus < 2 || clus > v->nclus + 1)
                return FAT_ECHAIN;
            pos = clus_off(v, clus);
            len = v->clus_bytes;
        }
        for (; len; pos += SECTOR, len -= SECTOR) {
            if (rd(v, pos, b, SECTOR))
                return FAT_EIO;
            for (i = 0; i < SECTOR; i += 32) {
                const uint8_t *d = b + i;

                if (!d[0])
                    return (int)seen;           /* end of directory */
                if (!raw && (d[0] == 0xe5 || d[11] == 0x0f || (d[11] & 0x08) || d[0] == '.'))
                    continue;
                seen++;
                if (fn(ctx, d))
                    return (int)seen;
            }
        }
        if (fixed)
            return (int)seen;
        clus = fat_next(v, clus);
        if (!clus)
            return (int)seen;
        if (clus == 1 || ++hops > v->nclus || hops > 4096)    /* 4096 clusters of entries: enough */
            return FAT_ECHAIN;
    }
}

static int dir_walk(fat_vol *v, uint32_t dir, raw_fn fn, void *ctx)
{
    return dir_walk_any(v, dir, fn, ctx, 0);
}

struct raw_ctx { fat_raw_fn fn; void *ctx; };

static int raw_cb(void *c, const uint8_t *d)
{
    struct raw_ctx *r = c;

    r->fn(r->ctx, d);
    return 0;
}

int fat_dir_raw(fat_vol *v, uint32_t dir, fat_raw_fn fn, void *ctx)
{
    struct raw_ctx r = { fn, ctx };

    return dir_walk_any(v, dir, raw_cb, &r, 1);
}

static void fill_ent(fat_vol *v, const uint8_t *d, fat_ent *e)
{
    short_name(d, e->name);
    e->attr = d[11];
    e->size = le32(d + 28);
    e->clus = le16(d + 26) | (v->type == 32 ? le16(d + 20) << 16 : 0);
}

struct list_ctx { fat_vol *v; fat_list_fn fn; void *ctx; };

static int list_cb(void *c, const uint8_t *d)
{
    struct list_ctx *l = c;
    fat_ent e;

    fill_ent(l->v, d, &e);
    l->fn(l->ctx, &e);
    return 0;
}

int fat_list(fat_vol *v, uint32_t dir, fat_list_fn fn, void *ctx)
{
    struct list_ctx l = { v, fn, ctx };

    return dir_walk(v, dir, list_cb, &l);
}

struct find_ctx { fat_vol *v; const char *name; fat_ent *out; int found; };

static int upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

static int find_cb(void *c, const uint8_t *d)
{
    struct find_ctx *f = c;
    char n[13];
    int i;

    short_name(d, n);
    for (i = 0; n[i] && upper((uint8_t)f->name[i]) == (uint8_t)n[i]; i++)
        ;
    if (n[i] || f->name[i])
        return 0;
    fill_ent(f->v, d, f->out);
    f->found = 1;
    return 1;
}

int fat_find(fat_vol *v, uint32_t dir, const char *name, fat_ent *out)
{
    struct find_ctx f = { v, name, out, 0 };
    int r = dir_walk(v, dir, find_cb, &f);

    return r < 0 ? r : f.found ? FAT_OK : FAT_ENOENT;
}

/* ------------------------------------------------------------------ files */

int fat_check(fat_vol *v, const fat_ent *e)
{
    uint32_t need = e->size / v->clus_bytes + (e->size % v->clus_bytes != 0), c = e->clus, i;

    if (!need)
        return FAT_OK;
    if (c < 2 || c > v->nclus + 1 || need > v->nclus)
        return FAT_ECHAIN;
    for (i = 1; i < need; i++) {
        c = fat_next(v, c);
        if (c < 2)
            return FAT_ECHAIN;
    }
    return fat_next(v, c) == 0 ? FAT_OK : FAT_ECHAIN;
}

void fat_open(fat_file *f, const fat_ent *e)
{
    f->e = *e;
    f->at_idx = 0;
    f->at_clus = 0;
}

int fat_read(fat_vol *v, fat_file *f, uint32_t off, void *buf, uint32_t n)
{
    uint8_t *p = buf;
    uint32_t cb = v->clus_bytes, done = 0;

    if (off >= f->e.size)
        return 0;
    if (n > f->e.size - off)
        n = f->e.size - off;
    while (n) {
        uint32_t ci = off / cb, in = off % cb, start, len, nx;

        if (!f->at_clus || ci < f->at_idx) {
            f->at_idx = 0;
            f->at_clus = f->e.clus;
            if (f->at_clus < 2 || f->at_clus > v->nclus + 1)
                return FAT_ECHAIN;
        }
        while (f->at_idx < ci) {
            nx = fat_next(v, f->at_clus);
            if (nx < 2)
                return FAT_ECHAIN;
            f->at_clus = nx;
            f->at_idx++;
        }
        start = f->at_clus;
        len = cb - in;
        while (len < n) {                       /* extend over consecutive clusters */
            nx = fat_next(v, f->at_clus);
            if (nx != f->at_clus + 1)
                break;
            f->at_clus = nx;
            f->at_idx++;
            len += cb;
        }
        if (len > n)
            len = n;
        if (rd(v, clus_off(v, start) + in, p, len))
            return FAT_EIO;
        p += len;
        off += len;
        n -= len;
        done += len;
    }
    return (int)done;
}

