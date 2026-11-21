/* py_import: disk mode's play/ folder -> PLAY's own folder on yaffs (play.h, README.md).
 *
 * TE deletes every root entry of the disk image except tape/ and album/ in one cleanup function
 * (0x01997de4, f_unlink per entry), called when disk mode is left without changes, in the sync
 * after a restart, and at the start of the defragment check. A hook before each of those calls
 * (py_te.c) copies play/ out first.
 * Two folders take turns, so the folder the main loop streams from is never written: the set is
 * copied into the inactive one, which then gets the ACTIVE marker; the main loop switches over at
 * its next pass and empties the old one. yaffs has no rename. Copied: every .WAV / .AIF / .AIFF
 * (short names), SET.TXT and SONG.BIN; hidden entries (macOS "._" files) and the rest are left. No
 * play/ folder = nothing happens, the last set stays. The image is read with the SDK's fat
 * library through an own read-only handle (TE keeps its own handle open).
 *
 * Shared songs (py_slots_import, the same moment): every whole .PSL file in the disk's play_s/
 * folder (PLAY's slot files, written there by py_te.c when disk mode is entered, or one from
 * someone else) that no slot holds already goes into a free slot: SONGnn.PSL into slot nn if that
 * is free, else the first free one. A slot is never overwritten; a file that is damaged or of
 * another PLAY version (other pattern sizes) is left.
 *
 * Demo songs (py_demos_install, main loop, once): the image carries slot files as
 * /yaffs2/content/play_demo_13.psl .. _16.psl (feature.json content_files, demos/gen_demos.c; TE's
 * content folder, a new folder there would have no file of TE's to model it on); at the first boot
 * with them each goes into a free slot the same way (SONG13..16 if free), then a marker in the
 * slot folder says it was done: a demo deleted later stays deleted, a new image does not bring
 * them back. */
#include <op1/te246.h>
#include <op1/fat.h>
#include "play.h"
#include "py_yaffs.h"
#include "py_slot.h"

#define PY_IMAGE    "/yaffs2/user/tape_c.raw"
#define COPY_CHUNK  (64u << 10)
#define MAXIMPORT   18                      /* 16 audio files + SET.TXT + SONG.BIN */
#define CONSOLE_PRINT ((void (*)(const char *line))0x019d2bf0)   /* TE's text console (disk sync) */

static const char *const dirs[2] = { PY_DIR_A, PY_DIR_B };
static int img_fd = -1;
static fat_vol vol;
static volatile uint8_t pending;                /* a new set is ACTIVE: the main loop must reload */
volatile uint32_t py_import_bytes, py_import_files, py_import_errors, py_import_runs;

static int img_read(void *ctx, uint32_t off, void *buf, uint32_t n)
{
    (void)ctx;
    return PY_PREAD(img_fd, buf, n, (int32_t)off);
}

static int exists(const char *path)
{
    static uint32_t st[32];                     /* TE's stat buffer is 0x48 bytes */

    return PY_STAT(path, st) >= 0;
}

/* the folder with the ACTIVE marker (0 if none / both) */
int py_active(void)
{
    char m[48];

    TE_SNPRINTF(m, sizeof(m), "%s/ACTIVE", dirs[1]);
    return exists(m) ? 1 : 0;
}

const char *py_dir(void)
{
    return dirs[py_active()];
}

static int up(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

static int same(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

/* any .WAV / .AIF / .AIFF, SET.TXT or SONG.BIN */
static int wanted(const fat_ent *e)
{
    const char *n = e->name, *x = 0;

    if (e->attr & 0x1e)                         /* hidden, system, label, directory */
        return 0;
    if (same(n, "SET.TXT") || same(n, "SONG.BIN"))
        return 1;
    for (; *n; n++)
        if (*n == '.')
            x = n + 1;
    if (!x)
        return 0;
    return (up(x[0]) == 'W' && up(x[1]) == 'A' && up(x[2]) == 'V' && !x[3])
        || (up(x[0]) == 'A' && up(x[1]) == 'I' && up(x[2]) == 'F' && (!x[3] || (up(x[3]) == 'F' && !x[4])));
}

static fat_ent found[MAXIMPORT];
static int nfound;

static void collect(void *ctx, const fat_ent *e)
{
    (void)ctx;
    if (wanted(e) && nfound < MAXIMPORT && e->size <= (512u << 20))
        found[nfound++] = *e;
}

/* delete every regular file in dir (the folder itself stays) */
void py_clear_dir(const char *dir)
{
    char path[64];
    void *d;
    uint8_t *de;
    int guard = 0;

    while ((d = PY_OPENDIR(dir)) != 0 && guard++ < 64) {
        int removed = 0;

        while ((de = PY_READDIR(d)) != 0) {
            const char *name = (const char *)de + 11;

            if (name[0] == '.')
                continue;
            TE_SNPRINTF(path, sizeof(path), "%s/%s", dir, name);
            if (PY_UNLINK(path) == 0) {
                removed = 1;
                break;                          /* the directory changed: start over */
            }
        }
        PY_CLOSEDIR(d);
        if (!removed)
            break;
    }
}

static int copy_file(const char *dir, const fat_ent *e, uint8_t *buf)
{
    char path[64];
    fat_file f;
    uint32_t off = 0;
    int out, ok = 1;

    TE_SNPRINTF(path, sizeof(path), "%s/%s", dir, e->name);
    out = TE_FILE_OPEN(path, TE_FILE_CREATE, TE_FILE_CREATE_MODE);
    if (out < 0)
        return 0;
    fat_open(&f, e);
    while (off < e->size) {
        uint32_t n = e->size - off < COPY_CHUNK ? e->size - off : COPY_CHUNK;

        if (fat_read(&vol, &f, off, buf, n) != (int)n || TE_FILE_WRITE(out, buf, n) != (int)n) {
            ok = 0;
            break;
        }
        off += n;
        py_import_bytes += n;
    }
    TE_FILE_CLOSE(out);
    if (!ok)
        PY_UNLINK(path);
    return ok;
}

/* copy from/SONG.BIN to to/SONG.BIN (yaffs to yaffs), if there is one */
static void keep_song(const char *from, const char *to, uint8_t *buf)
{
    char a[64], b[64];
    int in, out, n;

    TE_SNPRINTF(a, sizeof(a), "%s/SONG.BIN", from);
    TE_SNPRINTF(b, sizeof(b), "%s/SONG.BIN", to);
    in = TE_FILE_OPEN(a, TE_FILE_RDONLY, TE_FILE_MODE);
    if (in < 0)
        return;
    n = PY_PREAD(in, buf, COPY_CHUNK, 0);
    TE_FILE_CLOSE(in);
    if (n <= 0)
        return;
    out = TE_FILE_OPEN(b, TE_FILE_CREATE, TE_FILE_CREATE_MODE);
    if (out < 0)
        return;
    TE_FILE_WRITE(out, buf, (uint32_t)n);
    TE_FILE_CLOSE(out);
}

int py_import_stage(void)
{
    char m[48];
    fat_ent play;
    uint8_t *buf;
    int i, from, to, r = 0;

    nfound = 0;
    img_fd = TE_FILE_OPEN(PY_IMAGE, TE_FILE_RDONLY, TE_FILE_MODE);
    if (img_fd < 0)
        return 0;
    if (fat_mount(&vol, img_read, 0) || fat_find(&vol, 0, "PLAY", &play) || !(play.attr & 0x10)
        || fat_list(&vol, play.clus, collect, 0) < 0)
        goto done;
    r = 1;
    py_import_runs++;
    CONSOLE_PRINT("PLAY: importing the set...");
    buf = TE_HEAP_MALLOC(TE_HEAP_SAMPLES, COPY_CHUNK);
    if (!buf) {
        py_import_errors++;
        goto done;
    }
    from = py_active();
    to = !from;
    PY_MKDIR(PY_DIR_A, 0);                      /* may exist already */
    PY_MKDIR(PY_DIR_B, 0);
    py_clear_dir(dirs[to]);
    py_import_files = 0;
    for (i = 0; i < nfound; i++) {
        if (fat_check(&vol, &found[i]) == FAT_OK && copy_file(dirs[to], &found[i], buf))
            py_import_files++;
        else
            py_import_errors++;
    }
    {                                           /* no SONG.BIN in the new set: keep the song */
        int i2, has = 0;

        for (i2 = 0; i2 < nfound; i2++)
            has |= same(found[i2].name, "SONG.BIN");
        if (!has)
            keep_song(dirs[from], dirs[to], buf);
    }
    TE_FREE(buf);
    /* the switch: B's marker decides (py_active), A needs none */
    TE_SNPRINTF(m, sizeof(m), "%s/ACTIVE", dirs[1]);
    if (to == 1) {
        int fd = TE_FILE_OPEN(m, TE_FILE_CREATE, TE_FILE_CREATE_MODE);

        if (fd >= 0)
            TE_FILE_CLOSE(fd);
    } else {
        PY_UNLINK(m);
    }
    pending = 1;
done:
    TE_FILE_CLOSE(img_fd);
    img_fd = -1;
    return r;
}

int py_import_pending(void)
{
    return pending;
}

/* main thread, the old set closed: empty the folder that is no longer active */
void py_import_run(void)
{
    pending = 0;
    py_clear_dir(dirs[!py_active()]);
}

/* ------------------------------------------------------------------ shared songs: play_s/ -> slots */

#define MAXSONGS    32
#define SONG_MAX    (512u << 10)                /* (a slot file: ~84 KB) */

static fat_ent songs[MAXSONGS];
static int nsongs;
volatile uint32_t py_songs_imported, py_songs_left, py_songs_runs;    /* (bench, debug) */

static int is_psl(const fat_ent *e)
{
    const char *n = e->name, *x = 0;

    if (e->attr & 0x1e || e->size > SONG_MAX || n[0] == '_')
        return 0;                               /* (macOS "._X.PSL" copies: short name "_X~1.PSL") */
    for (; *n; n++)
        if (*n == '.')
            x = n + 1;
    return x && up(x[0]) == 'P' && up(x[1]) == 'S' && up(x[2]) == 'L' && !x[3];
}

static void collect_songs(void *ctx, const fat_ent *e)
{
    (void)ctx;
    if (is_psl(e) && nsongs < MAXSONGS)
        songs[nsongs++] = *e;
}

static fat_file song_f;
static int song_fd = -1;

static int psl_fat_read(void *ctx, uint32_t off, void *buf, uint32_t n)
{
    (void)ctx;
    return fat_read(&vol, &song_f, off, buf, n);
}

static int psl_yaffs_read(void *ctx, uint32_t off, void *buf, uint32_t n)
{
    (void)ctx;
    return PY_PREAD(song_fd, buf, n, (int32_t)off);
}

/* a slot file's length and CRC (its last 4 bytes); 0 = no file */
static uint32_t slot_tail(int n, uint32_t *crc, uint8_t *buf)
{
    static uint32_t st[32];
    char path[64];
    uint32_t len;
    int fd;

    py_pat_slot_path(path, n);
    if (PY_STAT(path, st) < 0)
        return 0;
    len = st[0x1c / 4];
    *crc = 0;
    fd = TE_FILE_OPEN(path, TE_FILE_RDONLY, TE_FILE_MODE);
    if (fd < 0)
        return len ? len : 1;
    if (len >= 4 && PY_PREAD(fd, buf, 4, (int32_t)(len - 4)) == 4)
        *crc = (uint32_t)buf[0] | (uint32_t)buf[1] << 8 | (uint32_t)buf[2] << 16 | (uint32_t)buf[3] << 24;
    TE_FILE_CLOSE(fd);
    return len ? len : 1;
}

/* copy songs[i] into slot n; 1 = done and read back whole */
static int import_song(int i, int n, psl_io *io, uint8_t *buf)
{
    char path[64];
    psl_info info;
    uint32_t off = 0, size = songs[i].size;
    int out, ok = 1;

    py_pat_slot_path(path, n);
    PY_MKDIR(PY_DIR_S, 0);
    out = TE_FILE_OPEN(path, TE_FILE_CREATE, TE_FILE_CREATE_MODE);
    if (out < 0)
        return 0;
    fat_open(&song_f, &songs[i]);
    while (ok && off < size) {
        uint32_t k = size - off < COPY_CHUNK ? size - off : COPY_CHUNK;

        ok = fat_read(&vol, &song_f, off, buf, k) == (int)k && TE_FILE_WRITE(out, buf, k) == (int)k;
        off += k;
    }
    TE_FILE_CLOSE(out);
    if (ok) {
        song_fd = TE_FILE_OPEN(path, TE_FILE_RDONLY, TE_FILE_MODE);
        io->read = psl_yaffs_read;
        ok = song_fd >= 0 && py_slot_check(io, &info) == PSL_OK;
        if (song_fd >= 0)
            TE_FILE_CLOSE(song_fd);
        song_fd = -1;
    }
    if (!ok)
        PY_UNLINK(path);                        /* no half-written slot */
    return ok;
}

/* ------------------------------------------------------------------ demo songs: content -> slots */

#define DEMO_PATH   "/yaffs2/content/play_demo_%d.psl"
#define DEMO_MARK   PY_DIR_S "/DEMOS.OK"
static const uint8_t demo_slot[] = { 13, 14, 15, 16 };    /* each its own slot number if free */
#define NDEMOS      ((int)sizeof(demo_slot))
volatile uint32_t py_demos_installed, py_demos_found, py_demos_why;    /* (bench, debug) */

static int demo_read(void *ctx, uint32_t off, void *buf, uint32_t n)
{
    return PY_PREAD(*(int *)ctx, buf, n, (int32_t)off);    /* (own handles: py_slots_import may run
                                                             * on the disk sync's thread) */
}

/* copy the open file `in` (size bytes) into slot n and read it back whole; 1 = done */
static int install_demo(int in, uint32_t size, int n, psl_io *io, uint8_t *buf)
{
    char path[64];
    psl_info info;
    uint32_t off = 0;
    int out, ok = 1, back;

    py_pat_slot_path(path, n);
    PY_MKDIR(PY_DIR_S, 0);
    out = TE_FILE_OPEN(path, TE_FILE_CREATE, TE_FILE_CREATE_MODE);
    if (out < 0)
        return 0;
    while (ok && off < size) {
        uint32_t k = size - off < COPY_CHUNK ? size - off : COPY_CHUNK;

        ok = PY_PREAD(in, buf, k, (int32_t)off) == (int)k && TE_FILE_WRITE(out, buf, k) == (int)k;
        off += k;
    }
    TE_FILE_CLOSE(out);
    if (ok) {
        void *was = io->ctx;

        back = TE_FILE_OPEN(path, TE_FILE_RDONLY, TE_FILE_MODE);
        io->ctx = &back;
        ok = back >= 0 && py_slot_check(io, &info) == PSL_OK;
        io->ctx = was;
        if (back >= 0)
            TE_FILE_CLOSE(back);
    }
    if (!ok)
        PY_UNLINK(path);
    return ok;
}

int py_demos_install(void)
{
    static uint32_t st[32];
    static char line[48];
    char path[64], name[16];
    psl_io io;
    psl_info info;
    uint8_t *buf;
    uint16_t taken = 0;
    uint32_t len[PSL_SLOTS], crc[PSL_SLOTS];
    int i, n, r = 0, found = 0, left = 0, in;

    if (py_pat_slot_busy())
        return -1;                              /* (a slot is being written: next pass) */
    if (PY_STAT(DEMO_MARK, st) >= 0)
        return 0;                               /* done at an earlier boot */
    buf = TE_HEAP_MALLOC(TE_HEAP_SAMPLES, COPY_CHUNK);
    if (!buf)
        return -1;
    for (n = 0; n < PSL_SLOTS; n++) {
        len[n] = slot_tail(n, &crc[n], buf);
        if (len[n])
            taken |= (uint16_t)(1u << n);
    }
    for (i = 0; i < (int)sizeof(io); i++)
        ((uint8_t *)&io)[i] = 0;
    py_pat_slot_sizes(io.size);
    io.pat = buf;
    io.read = demo_read;
    for (i = 0; i < NDEMOS; i++) {
        uint16_t same_as = 0;
        uint32_t size, c;
        int to, ok;

        TE_SNPRINTF(path, sizeof(path), DEMO_PATH, demo_slot[i]);
        if (PY_STAT(path, st) < 0)
            continue;
        found++;
        size = st[0x1c / 4];
        in = TE_FILE_OPEN(path, TE_FILE_RDONLY, TE_FILE_MODE);
        if (in < 0) {
            left++;
            py_demos_why = 1;
            continue;
        }
        io.ctx = &in;
        ok = size >= 8 && py_slot_check(&io, &info) == PSL_OK && PY_PREAD(in, buf, 4, (int32_t)(size - 4)) == 4;
        c = ok ? (uint32_t)buf[0] | (uint32_t)buf[1] << 8 | (uint32_t)buf[2] << 16 | (uint32_t)buf[3] << 24 : 0;
        for (n = 0; ok && n < PSL_SLOTS; n++)
            if (len[n] == size && crc[n] == c)
                same_as |= (uint16_t)(1u << n);
        name[0] = 'S'; name[1] = 'O'; name[2] = 'N'; name[3] = 'G';        /* "SONGnn.PSL" */
        name[4] = (char)('0' + demo_slot[i] / 10);
        name[5] = (char)('0' + demo_slot[i] % 10);
        name[6] = '.'; name[7] = 'P'; name[8] = 'S'; name[9] = 'L'; name[10] = 0;
        to = ok ? py_slot_pick(name, taken, same_as) : -1;
        py_demos_why = !ok ? 2 : to == -1 ? 3 : 0;
        if (to == -2) {                         /* a slot holds it already */
            TE_FILE_CLOSE(in);
            continue;
        }
        ok = ok && to >= 0 && install_demo(in, size, to, &io, buf);
        TE_FILE_CLOSE(in);
        if (!ok && !py_demos_why)
            py_demos_why = 4;
        if (!ok) {
            left++;                             /* damaged, another version, no free slot */
            continue;
        }
        taken |= (uint16_t)(1u << to);
        len[to] = size;
        crc[to] = c;
        r++;
    }
    TE_FREE(buf);
    py_demos_found = (uint32_t)found;
    py_demos_installed = (uint32_t)r;
    if (found) {                                /* (no demos in this image: ask again next boot) */
        int fd;

        PY_MKDIR(PY_DIR_S, 0);
        fd = TE_FILE_OPEN(DEMO_MARK, TE_FILE_CREATE, TE_FILE_CREATE_MODE);
        if (fd >= 0) {
            TE_FILE_WRITE(fd, "1", 1);
            TE_FILE_CLOSE(fd);
        }
        TE_SNPRINTF(line, sizeof(line), "PLAY: %d demo songs into slots (%d left)", r, left);
        CONSOLE_PRINT(line);
    }
    return r;
}

int py_slots_import(void)
{
    static char line[48];
    fat_ent dir;
    psl_io io;
    psl_info info;
    uint8_t *buf;
    uint16_t taken = 0;
    uint32_t len[PSL_SLOTS], crc[PSL_SLOTS];
    int i, n, r = 0;

    nsongs = 0;
    if (py_pat_slot_busy())
        return 0;                               /* (the main loop writes a slot: next time) */
    img_fd = TE_FILE_OPEN(PY_IMAGE, TE_FILE_RDONLY, TE_FILE_MODE);
    if (img_fd < 0)
        return 0;
    if (fat_mount(&vol, img_read, 0) || fat_find(&vol, 0, "PLAY_S", &dir) || !(dir.attr & 0x10)
        || fat_list(&vol, dir.clus, collect_songs, 0) < 0 || !nsongs)
        goto done;
    buf = TE_HEAP_MALLOC(TE_HEAP_SAMPLES, COPY_CHUNK);
    if (!buf)
        goto done;
    py_songs_runs++;
    py_songs_left = 0;
    for (n = 0; n < PSL_SLOTS; n++) {
        len[n] = slot_tail(n, &crc[n], buf);
        if (len[n])
            taken |= (uint16_t)(1u << n);
    }
    {
        int k;

        for (k = 0; k < (int)sizeof(io); k++)   /* (only size, pat and read are used by check) */
            ((uint8_t *)&io)[k] = 0;
    }
    py_pat_slot_sizes(io.size);
    io.pat = buf;                               /* (check: pattern-sized chunks) */
    for (i = 0; i < nsongs; i++) {
        uint16_t same_as = 0;
        uint32_t c;
        int to;

        io.read = psl_fat_read;
        fat_open(&song_f, &songs[i]);
        if (fat_check(&vol, &songs[i]) != FAT_OK || py_slot_check(&io, &info) != PSL_OK
            || fat_read(&vol, &song_f, songs[i].size - 4, buf, 4) != 4) {
            py_songs_left++;                    /* damaged, cut short, another version */
            continue;
        }
        c = (uint32_t)buf[0] | (uint32_t)buf[1] << 8 | (uint32_t)buf[2] << 16 | (uint32_t)buf[3] << 24;
        for (n = 0; n < PSL_SLOTS; n++)
            if (len[n] == songs[i].size && crc[n] == c)
                same_as |= (uint16_t)(1u << n);
        to = py_slot_pick(songs[i].name, taken, same_as);
        if (to == -2)
            continue;                           /* a slot holds it already (the export) */
        if (to < 0 || !import_song(i, to, &io, buf)) {
            py_songs_left++;
            continue;
        }
        taken |= (uint16_t)(1u << to);
        len[to] = songs[i].size;
        crc[to] = c;
        py_songs_imported++;
        r++;
    }
    TE_FREE(buf);
    if (r || py_songs_left) {
        TE_SNPRINTF(line, sizeof(line), "PLAY: %d songs into slots (%d left)", r, (int)py_songs_left);
        CONSOLE_PRINT(line);
    }
done:
    TE_FILE_CLOSE(img_fd);
    img_fd = -1;
    return r;
}
