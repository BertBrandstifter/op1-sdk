/* host test for lib/fat.c (tools/test_all.py builds it with its sibling fat.c, -I include).
 *
 * Builds FAT16 (superfloppy) and FAT32 (behind an MBR) volumes in memory, the way a Mac's Finder
 * leaves them: a PLAY folder with long-name entries, AppleDouble "._" files, fragmented files, a
 * directory spanning several clusters; then checks mount, find, list, cluster-run reads against
 * the reference bytes, and corrupt volumes (no hang, an error). Header parsing: test_audiofile.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <op1/fat.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ------------------------------------------------------------------ memory image + formatter */

typedef struct img {
    uint8_t *b;
    uint32_t size, base;                /* partition byte offset */
    int type;                           /* 16 / 32 */
    uint32_t bps, spc, rsvd, nfats, rootents, fatsz, tot, nclus;
    uint32_t fat_off, root_off, data_off, cb, next_free;
    int reads;
} img;

static int mem_read(void *ctx, uint32_t off, void *buf, uint32_t n)
{
    img *m = ctx;

    m->reads++;
    if (off > m->size || n > m->size - off)
        return -1;
    memcpy(buf, m->b + off, n);
    return (int)n;
}

static void w16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { w16(p, v); w16(p + 2, v >> 16); }
static void b16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void b32(uint8_t *p, uint32_t v) { b16(p, v >> 16); b16(p + 2, v); }

static void set_fat(img *m, uint32_t c, uint32_t v)
{
    uint32_t f;

    for (f = 0; f < m->nfats; f++) {
        uint8_t *t = m->b + m->fat_off + f * m->fatsz * m->bps;

        if (m->type == 16)
            w16(t + c * 2, v);
        else
            w32(t + c * 4, v);
    }
}

static uint32_t eoc(img *m) { return m->type == 16 ? 0xffff : 0x0fffffff; }

static void mkfs(img *m, uint32_t size, int type, uint32_t spc, uint32_t part_lba)
{
    uint8_t *bs;
    uint32_t meta, rootsecs;

    memset(m, 0, sizeof(*m));
    m->size = size;
    m->b = calloc(1, size);
    m->base = part_lba * 512;
    m->type = type;
    m->bps = 512;
    m->spc = spc;
    m->rsvd = type == 32 ? 32 : 1;
    m->nfats = 2;
    m->rootents = type == 32 ? 0 : 512;
    m->tot = (size - m->base) / 512;
    rootsecs = m->rootents * 32 / 512;
    m->fatsz = 1;
    for (;;) {                              /* smallest FAT that covers the clusters */
        meta = m->rsvd + m->nfats * m->fatsz + rootsecs;
        m->nclus = (m->tot - meta) / spc;
        if (m->fatsz * 512 >= (m->nclus + 2) * (type / 8))
            break;
        m->fatsz++;
    }
    if (part_lba) {                         /* MBR with one FAT32 LBA partition */
        uint8_t *p = m->b + 0x1be;

        p[4] = 0x0c;
        w32(p + 8, part_lba);
        w32(p + 12, m->tot);
        m->b[510] = 0x55;
        m->b[511] = 0xaa;
    }
    bs = m->b + m->base;
    bs[0] = 0xeb; bs[1] = 0x58; bs[2] = 0x90;
    memcpy(bs + 3, "BSD  4.4", 8);
    w16(bs + 11, 512);
    bs[13] = (uint8_t)spc;
    w16(bs + 14, m->rsvd);
    bs[16] = (uint8_t)m->nfats;
    w16(bs + 17, m->rootents);
    if (m->tot < 65536 && type == 16)
        w16(bs + 19, m->tot);
    else
        w32(bs + 32, m->tot);
    bs[21] = 0xf8;
    if (type == 16) {
        w16(bs + 22, m->fatsz);
    } else {
        w32(bs + 36, m->fatsz);
        w32(bs + 44, 2);                    /* root cluster */
    }
    bs[510] = 0x55;
    bs[511] = 0xaa;
    m->cb = 512 * spc;
    m->fat_off = m->base + m->rsvd * 512;
    m->root_off = m->fat_off + m->nfats * m->fatsz * 512;
    m->data_off = m->root_off + rootsecs * 512;
    set_fat(m, 0, type == 16 ? 0xfff8 : 0x0ffffff8);
    set_fat(m, 1, eoc(m));
    m->next_free = 2;
    if (type == 32) {
        set_fat(m, 2, eoc(m));              /* root directory */
        m->next_free = 3;
    }
}

static uint8_t *clus_ptr(img *m, uint32_t c) { return m->b + m->data_off + (c - 2) * m->cb; }

/* allocate a chain for n bytes; gap > 0 leaves `gap` free clusters after every run of `run` */
static uint32_t alloc_chain(img *m, uint32_t n, uint32_t run, uint32_t gap)
{
    uint32_t count = n ? (n + m->cb - 1) / m->cb : 1, first = 0, prev = 0, i, inrun = 0;

    for (i = 0; i < count; i++) {
        uint32_t c = m->next_free++;

        if (gap && run && inrun == run) {
            m->next_free += gap;
            c = m->next_free++;
            inrun = 0;
        }
        inrun++;
        if (prev)
            set_fat(m, prev, c);
        else
            first = c;
        set_fat(m, c, eoc(m));
        prev = c;
    }
    return first;
}

static void write_chain(img *m, uint32_t c, const uint8_t *data, uint32_t n)
{
    while (n) {
        uint32_t k = n < m->cb ? n : m->cb, nx;

        memcpy(clus_ptr(m, c), data, k);
        data += k;
        n -= k;
        nx = m->type == 16 ? (uint32_t)(m->b[m->fat_off + c * 2] | m->b[m->fat_off + c * 2 + 1] << 8)
                           : (uint32_t)(m->b[m->fat_off + c * 4] | m->b[m->fat_off + c * 4 + 1] << 8
                                        | m->b[m->fat_off + c * 4 + 2] << 16);
        c = nx;
    }
}

/* a directory being filled: either the FAT16 root area or a chain of directory clusters */
typedef struct dirw { img *m; uint32_t first, clus, used; int root16; } dirw;

static uint8_t *dir_slot(dirw *d)
{
    img *m = d->m;

    if (d->root16)
        return m->b + m->root_off + 32 * d->used++;
    if (d->used * 32 == m->cb) {            /* grow the directory by one cluster */
        uint32_t c = m->next_free++;

        set_fat(m, d->clus, c);
        set_fat(m, c, eoc(m));
        d->clus = c;
        d->used = 0;
    }
    return clus_ptr(m, d->clus) + 32 * d->used++;
}

static void put83(uint8_t *e, const char *name)    /* "A1.WAV" -> "A1      WAV" */
{
    const char *dot = strchr(name, '.');
    int i, n = dot ? (int)(dot - name) : (int)strlen(name);

    memset(e, ' ', 11);
    for (i = 0; i < n && i < 8; i++)
        e[i] = (uint8_t)name[i];
    if (dot)
        for (i = 0; i < 3 && dot[1 + i]; i++)
            e[8 + i] = (uint8_t)dot[1 + i];
}

static void add_lfn(dirw *d, int parts)             /* placeholder long-name entries (skipped) */
{
    int i;

    for (i = parts; i >= 1; i--) {
        uint8_t *e = dir_slot(d);

        memset(e, 0, 32);
        e[0] = (uint8_t)(i | (i == parts ? 0x40 : 0));
        e[1] = 'x';
        e[11] = 0x0f;
    }
}

static void add_ent(dirw *d, const char *n83, uint8_t attr, uint32_t clus, uint32_t size, int lfn)
{
    uint8_t *e;

    if (lfn)
        add_lfn(d, lfn);
    e = dir_slot(d);
    memset(e, 0, 32);
    put83(e, n83);
    e[11] = attr;
    w16(e + 26, clus & 0xffff);
    if (d->m->type == 32)
        w16(e + 20, clus >> 16);
    w32(e + 28, size);
}

static uint32_t add_file(dirw *d, const char *n83, const uint8_t *data, uint32_t n, uint32_t run,
                         uint32_t gap)
{
    uint32_t c = n ? alloc_chain(d->m, n, run, gap) : 0;

    if (n)
        write_chain(d->m, c, data, n);
    add_ent(d, n83, 0x20, c, n, 2);
    return c;
}

static dirw new_dir(img *m, dirw *parent, const char *n83)
{
    dirw d = { m, 0, 0, 0, 0 };
    uint8_t *e;

    d.first = d.clus = alloc_chain(m, m->cb, 0, 0);
    memset(clus_ptr(m, d.first), 0, m->cb);
    e = dir_slot(&d);                       /* "." and ".." */
    memset(e, ' ', 11); e[0] = '.'; e[11] = 0x10;
    e = dir_slot(&d);
    memset(e, ' ', 11); e[0] = '.'; e[1] = '.'; e[11] = 0x10;
    add_ent(parent, n83, 0x10, d.first, 0, 1);
    return d;
}

static dirw root_dir(img *m)
{
    dirw d = { m, 0, 0, 0, m->type == 16 };

    if (!d.root16) {
        d.first = d.clus = 2;
        memset(clus_ptr(m, 2), 0, m->cb);
    }
    return d;
}

/* ------------------------------------------------------------------ audio files */

static uint8_t file_buf[1 << 21];

static uint8_t pat(uint32_t i) { return (uint8_t)(i * 7 + (i >> 9)); }

/* WAV: fmt (tag, ch, rate, bits), an optional LIST chunk before it, data of n bytes;
 * declared != 0 overrides the data chunk's size field (a truncated file) */
static uint32_t make_wav(uint8_t *b, int tag, int ch, uint32_t rate, int bits, uint32_t n,
                         int list, uint32_t declared)
{
    uint32_t p = 12, i, flen = tag == 0xfffe ? 40 : 16;

    memcpy(b, "RIFF", 4);
    memcpy(b + 8, "WAVE", 4);
    if (list) {                             /* an odd-sized chunk: padding byte */
        memcpy(b + p, "LIST", 4); w32(b + p + 4, 13);
        memset(b + p + 8, 'i', 13);
        p += 8 + 14;
    }
    memcpy(b + p, "fmt ", 4); w32(b + p + 4, flen);
    w16(b + p + 8, (uint32_t)tag); w16(b + p + 10, (uint32_t)ch); w32(b + p + 12, rate);
    w32(b + p + 16, rate * (uint32_t)ch * (uint32_t)bits / 8); w16(b + p + 20, (uint32_t)(ch * bits / 8));
    w16(b + p + 22, (uint32_t)bits);
    if (tag == 0xfffe) {
        w16(b + p + 24, 22); w16(b + p + 26, (uint32_t)bits); w32(b + p + 28, 3);
        w16(b + p + 32, 1);                 /* KSDATAFORMAT_SUBTYPE_PCM */
        memcpy(b + p + 34, "\x00\x00\x00\x00\x10\x00", 6);
    }
    p += 8 + flen;
    memcpy(b + p, "data", 4); w32(b + p + 4, declared ? declared : n);
    for (i = 0; i < n; i++)
        b[p + 8 + i] = pat(i);
    p += 8 + n;
    w32(b + 4, p - 8);
    return p;
}

static void ext80(uint8_t *b, uint32_t rate)  /* integer -> 80-bit extended */
{
    int e = 31;

    while (!(rate & 0x80000000u)) {
        rate <<= 1;
        e--;
    }
    b16(b, 16383 + (uint32_t)e);
    b32(b + 2, rate);
    memset(b + 6, 0, 4);
}

/* AIFF / AIFC: COMM (ch, frames, bits, rate[, compression]), SSND with offset ofs */
static uint32_t make_aif(uint8_t *b, const char *comp, int ch, uint32_t rate, int bits,
                         uint32_t frames, uint32_t ofs)
{
    uint32_t p = 12, n = frames * (uint32_t)ch * 2, i, clen = comp ? 24 : 18;

    memcpy(b, "FORM", 4);
    memcpy(b + 8, comp ? "AIFC" : "AIFF", 4);
    if (comp) {
        memcpy(b + p, "FVER", 4); b32(b + p + 4, 4); b32(b + p + 8, 0xa2805140u);
        p += 12;
    }
    memcpy(b + p, "COMM", 4); b32(b + p + 4, clen);
    b16(b + p + 8, (uint32_t)ch); b32(b + p + 10, frames); b16(b + p + 14, (uint32_t)bits);
    ext80(b + p + 16, rate);
    if (comp) {
        memcpy(b + p + 26, comp, 4);
        b[p + 30] = 0; b[p + 31] = 0;       /* empty pstring + pad */
    }
    p += 8 + clen;
    memcpy(b + p, "APPL", 4); b32(b + p + 4, 5); memcpy(b + p + 8, "op-1\0", 5);
    p += 8 + 6;
    memcpy(b + p, "SSND", 4); b32(b + p + 4, 8 + ofs + n); b32(b + p + 8, ofs); b32(b + p + 12, 0);
    memset(b + p + 16, 0, ofs);
    for (i = 0; i < n; i++)
        b[p + 16 + ofs + i] = pat(i);
    p += 16 + ofs + n;
    b32(b + 4, p - 8);
    return p;
}

/* ------------------------------------------------------------------ checks */

typedef struct expect { const char *name; uint32_t size; } expect;

static int list_count, raw_count;
static void raw_cb(void *ctx, const uint8_t *d) { (void)ctx; (void)d; raw_count++; }
static void count_cb(void *ctx, const fat_ent *e) { (void)ctx; (void)e; list_count++; }

static expect play_files[16];
static int nplay;

static void check_file(fat_vol *v, uint32_t dir, const expect *x)
{
    fat_ent e;

    if (fat_find(v, dir, x->name, &e) != FAT_OK) {
        fails++;
        printf("FAIL missing %s\n", x->name);
        return;
    }
    if (e.size != x->size || fat_check(v, &e) != FAT_OK || (e.attr & 0x10)) {
        fails++;
        printf("FAIL %s: size %u (want %u), chain %d\n", x->name, e.size, x->size, fat_check(v, &e));
    }
}

/* every read of the file at a set of offsets / lengths matches the reference bytes */
static void check_reads(fat_vol *v, uint32_t dir, const char *name, const uint8_t *ref, uint32_t n)
{
    static uint8_t got[1 << 21];
    static const uint32_t offs[] = { 0, 1, 511, 512, 2047, 2048, 4095, 10000, 65537 };
    static const uint32_t lens[] = { 1, 100, 512, 2048, 5000, 70000, 1 << 20 };
    fat_ent e;
    fat_file f;
    unsigned i, j;
    int r;

    CHECK(fat_find(v, dir, name, &e) == FAT_OK);
    fat_open(&f, &e);
    r = fat_read(v, &f, 0, got, n + 100);   /* clamped to the size */
    CHECK(r == (int)n && !memcmp(got, ref, n));
    for (i = 0; i < sizeof(offs) / sizeof(offs[0]); i++)
        for (j = 0; j < sizeof(lens) / sizeof(lens[0]); j++) {
            uint32_t o = offs[i] < n ? offs[i] : n - 1, l = lens[j];
            uint32_t want = l < n - o ? l : n - o;

            r = fat_read(v, &f, o, got, l);
            if (r != (int)want || memcmp(got, ref + o, want)) {
                fails++;
                printf("FAIL read %s at %u len %u: %d\n", name, o, l, r);
            }
        }
    /* backwards after forwards: the cursor restarts */
    CHECK(fat_read(v, &f, n - 10, got, 10) == 10 && !memcmp(got, ref + n - 10, 10));
    CHECK(fat_read(v, &f, 3, got, 10) == 10 && !memcmp(got, ref + 3, 10));
    CHECK(fat_read(v, &f, n, got, 10) == 0);
}

static uint8_t a1[1 << 21];                  /* A1.WAV's bytes: the reference for the reads */

static void note(const char *name, uint32_t size)
{
    play_files[nplay].name = name;
    play_files[nplay++].size = size;
}

static void build_play(img *m, dirw *root, dirw *play, uint32_t *a1_len)
{
    uint32_t n;

    nplay = 0;
    add_ent(root, "OP1     ", 0x08, 0, 0, 0);  /* volume label: not listed */
    *play = new_dir(m, root, "PLAY");
    add_file(root, "TAPE", 0, 0, 0, 0);
    add_file(play, "_A1~1.WAV", (const uint8_t *)"\0\x05\x16\x07", 4, 0, 0);
    note("_A1~1.WAV", 4);   /* AppleDouble */
    /* A1: stereo, a LIST chunk first, fragmented in runs of 3 clusters with gaps of 2 */
    *a1_len = n = make_wav(a1, 1, 2, 44100, 16, 300000, 1, 0);
    add_file(play, "A1.WAV", a1, n, 3, 2);
    note("A1.WAV", n);
    n = make_aif(file_buf, 0, 1, 44100, 16, 20000, 0);         /* mono AIFF */
    add_file(play, "A2.AIF", file_buf, n, 0, 0);
    note("A2.AIF", n);
    n = make_aif(file_buf, 0, 2, 48000, 16, 1000, 0);          /* 48 kHz */
    add_file(play, "A3.AIF", file_buf, n, 0, 0);
    note("A3.AIF", n);
    n = make_wav(file_buf, 1, 2, 44100, 24, 6000, 0, 0);        /* 24 bit */
    add_file(play, "A4.WAV", file_buf, n, 0, 0);
    note("A4.WAV", n);
    n = make_aif(file_buf, "sowt", 2, 44100, 16, 5000, 4);     /* AIFC little-endian, SSND offset */
    add_file(play, "A5.AIF", file_buf, n, 2, 1);
    note("A5.AIF", n);
    n = make_wav(file_buf, 1, 2, 44100, 16, 4000, 0, 900000);   /* data says more than there is */
    add_file(play, "A6.WAV", file_buf, n, 0, 0);
    note("A6.WAV", n);
    n = make_wav(file_buf, 0xfffe, 2, 44100, 16, 8000, 0, 0);   /* WAVE_FORMAT_EXTENSIBLE PCM */
    add_file(play, "A7.WAV", file_buf, n, 0, 0);
    note("A7.WAV", n);
    add_file(play, "A8.MP3", (const uint8_t *)"ID3\x04 not audio we read", 23, 0, 0);
    note("A8.MP3", 23);
    n = make_aif(file_buf, "ima4", 2, 44100, 16, 100, 0);      /* compressed AIFC */
    add_file(play, "B1.AIF", file_buf, n, 0, 0);
    note("B1.AIF", n);
    n = make_wav(file_buf, 3, 2, 44100, 32, 800, 0, 0);         /* float */
    add_file(play, "B2.WAV", file_buf, n, 0, 0);
    note("B2.WAV", n);
    n = make_aif(file_buf, "twos", 2, 44100, 16, 3000, 0);     /* AIFC big-endian PCM (afconvert) */
    add_file(play, "C1.AIF", file_buf, n, 0, 0);
    note("C1.AIF", n);
    add_file(play, "SET.TXT", (const uint8_t *)"bpm 120\n", 8, 0, 0);
    note("SET.TXT", 8);
}


static void check_volume(img *m, int type)
{
    fat_vol v;
    fat_ent play, e;
    unsigned i;
    int r;

    r = fat_mount(&v, mem_read, m);
    CHECK(r == FAT_OK && v.type == type && v.nclus == m->nclus && v.clus_bytes == m->cb);
    if (r)
        return;
    CHECK(fat_find(&v, 0, "play", &play) == FAT_OK && (play.attr & 0x10));
    CHECK(fat_find(&v, 0, "NOPE", &e) == FAT_ENOENT);
    list_count = 0;
    CHECK(fat_list(&v, 0, count_cb, 0) == 2 && list_count == 2);    /* PLAY, TAPE (no label) */
    raw_count = 0;
    CHECK(fat_dir_raw(&v, 0, raw_cb, 0) == 6 && raw_count == 6);    /* + label, 3 long-name parts */
    list_count = 0;
    r = fat_list(&v, play.clus, count_cb, 0);
    CHECK(r == 13 && list_count == 13);
    for (i = 0; i < (unsigned)nplay; i++)
        check_file(&v, play.clus, &play_files[i]);
    CHECK(fat_find(&v, play.clus, "a2.aif", &e) == FAT_OK);            /* case-insensitive */
}

static void test_fat16(void)
{
    img m;
    dirw root, play;
    uint32_t n;
    fat_vol v;
    fat_ent e;
    fat_file f;
    static uint8_t buf[1 << 20];

    mkfs(&m, 16u << 20, 16, 4, 0);
    root = root_dir(&m);
    build_play(&m, &root, &play, &n);
    check_volume(&m, 16);
    if (fat_mount(&v, mem_read, &m))
        return;
    {
        fat_ent pl;

        fat_find(&v, 0, "PLAY", &pl);
        check_reads(&v, pl.clus, "A1.WAV", a1, n);
        /* a whole read of fragmented A1 = one data read per run (+ FAT blocks), not per cluster */
        fat_find(&v, pl.clus, "A1.WAV", &e);
        fat_open(&f, &e);
        m.reads = 0;
        CHECK(fat_read(&v, &f, 0, buf, 600000) == 600000 || n < 600000);
        printf("fat16: A1 %u bytes in %u clusters, %d reads for 600000 bytes\n", n,
               (n + m.cb - 1) / m.cb, m.reads);
        CHECK(m.reads < (int)((600000 / m.cb) / 2));
    }
    free(m.b);
}

static void test_fat32(void)
{
    img m;
    dirw root, play, many;
    uint32_t n, i;
    fat_vol v;
    fat_ent e, pl;
    char name[13];

    mkfs(&m, 40u << 20, 32, 1, 2048);       /* 512-byte clusters: > 65525 of them */
    root = root_dir(&m);
    build_play(&m, &root, &play, &n);
    check_volume(&m, 32);
    /* a folder of 150 files with long names: the directory spans many clusters */
    many = new_dir(&m, &root, "MANY");
    for (i = 0; i < 150; i++) {
        snprintf(name, sizeof(name), "F%03u.AIF", i);
        add_ent(&many, name, 0x20, 0, 0, 3);
    }
    if (fat_mount(&v, mem_read, &m))
        return;
    CHECK(v.base == 2048 * 512);
    fat_find(&v, 0, "PLAY", &pl);
    check_reads(&v, pl.clus, "A1.WAV", a1, n);
    CHECK(fat_find(&v, 0, "MANY", &e) == FAT_OK);
    CHECK(fat_find(&v, e.clus, "F149.AIF", &pl) == FAT_OK);
    list_count = 0;
    CHECK(fat_list(&v, e.clus, count_cb, 0) == 150);

    /* corrupt: A1's chain loops back on itself -> an error, no hang */
    fat_find(&v, 0, "PLAY", &pl);
    fat_find(&v, pl.clus, "A1.WAV", &e);
    set_fat(&m, e.clus + 1, e.clus);
    v.fat_cache_at = ~0u;
    {
        fat_file f;
        static uint8_t buf[1 << 20];

        fat_open(&f, &e);
        CHECK(fat_check(&v, &e) == FAT_ECHAIN);
        CHECK(fat_read(&v, &f, 0, buf, 1 << 20) == (int)e.size);    /* bounded: no hang */
    }
    /* corrupt: a chain that ends early */
    fat_find(&v, pl.clus, "A2.AIF", &e);
    set_fat(&m, e.clus, eoc(&m));
    v.fat_cache_at = ~0u;
    CHECK(fat_check(&v, &e) == FAT_ECHAIN);
    /* corrupt: a directory chain into a free cluster */
    fat_find(&v, 0, "MANY", &e);
    set_fat(&m, e.clus, 0);
    v.fat_cache_at = ~0u;
    list_count = 0;
    CHECK(fat_list(&v, e.clus, count_cb, 0) == FAT_ECHAIN);
    free(m.b);
}

static void test_bad(void)
{
    img m;
    fat_vol v;

    memset(&m, 0, sizeof(m));
    m.size = 1 << 16;
    m.b = calloc(1, m.size);
    CHECK(fat_mount(&v, mem_read, &m) == FAT_ENOFS);             /* all zero */
    memset(m.b, 0xa5, m.size);
    CHECK(fat_mount(&v, mem_read, &m) == FAT_ENOFS);             /* garbage */
    free(m.b);
    mkfs(&m, 1u << 20, 16, 1, 0);           /* 1 MB: < 4085 clusters = FAT12 */
    CHECK(fat_mount(&v, mem_read, &m) == FAT_EFAT12);
    m.b[13] = 3;                            /* sectors per cluster not a power of two */
    CHECK(fat_mount(&v, mem_read, &m) == FAT_ENOFS);
    free(m.b);
    m.size = 100;                           /* shorter than a sector */
    m.b = calloc(1, 100);
    CHECK(fat_mount(&v, mem_read, &m) == FAT_EIO);
    free(m.b);
}

int main(void)
{
    test_fat16();
    test_fat32();
    test_bad();
    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
