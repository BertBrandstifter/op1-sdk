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
 * library through an own read-only handle (TE keeps its own handle open). */
#include <op1/te246.h>
#include <op1/fat.h>
#include "play.h"
#include "py_yaffs.h"

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
