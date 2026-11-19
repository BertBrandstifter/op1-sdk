/* py_te: the disk-mode hooks (play.h, README.md).
 *
 * Leaving: TE deletes the disk image's root entries in one cleanup function, 0x01997de4(dir,
 * depth), called with ("", 0) at three sites (static RE 2026-11-15): 0x01997584 (disk mode left,
 * no changes in synth/drum/album/tape), 0x01996986 (the sync after a restart, after TE's own
 * import) and 0x01996f6a (the defragment check, every boot and once more on the way out). PLAY's
 * call-site hook runs before each; py_import_stage does nothing when there is no play/ folder
 * (any more).
 *
 * Entering (static RE 2026-11-19, fat_manager toggle 0x0199742c): TE cleans the root, writes
 * synth/ and drum/, the tape and album entries, then unmounts its FatFs (0x019975fa:
 * f_mount(x, NULL)) and only then registers the USB disk (0x01997604). Before that unmount PLAY
 * copies the render folder into play_r/ through TE's own FatFs (it caches the FAT window and the
 * free count: raw writes into tape_c.raw would corrupt the volume). KeyHandler thread; TE spends
 * seconds there on its own exports. TE's cleanup deletes play_r/ again when disk mode is left.
 * The song slots go the same way first (small): SONGnn.BIN -> play_s/SONGnn.PSL, the file to
 * share (py_import.c takes new ones back into free slots when disk mode is left). */
#include <op1/te246.h>
#include "play.h"
#include "py_yaffs.h"
#include "py_ui.h"
#include "py_export.h"

void py_cleanup_hook(const char *dir, int depth)
{
    if (depth == 0 && dir && !dir[0]) {
        py_import_stage();
        py_slots_import();                      /* play_s/: shared songs into free slots */
    }
}

/* ------------------------------------------------------------------ renders into disk mode */

#define F_OPEN      ((int (*)(void *fil, const char *path, int mode))0x0199b2a8)
#define F_WRITE     ((int (*)(void *fil, const void *buf, uint32_t n, uint32_t *bw))0x0199b8a8)
#define F_CLOSE     ((int (*)(void *fil))0x0199bd7c)
#define F_UNLINK    ((int (*)(const char *path))0x0199c620)
#define F_MKDIR     ((int (*)(const char *path))0x0199c7b8)
#define FAT_FREE    ((uint32_t (*)(void))0x019964ec)        /* free clusters x cluster bytes */
#define FAT_CSIZE   (*(volatile uint8_t *)(0x01aabee4 + 2)) /* TE's FATFS: sectors per cluster */
#define YAFFS_FREE  ((int32_t (*)(const char *path))0x01a9b1c0)
#define CONSOLE_PRINT ((void (*)(const char *line))0x019d2bf0)
#define FA_WRITE_NEW 0x0a                       /* FA_WRITE | FA_CREATE_ALWAYS */
#define EXPORT_CHUNK (32u << 10)

static uint32_t fil[0x824 / 4 + 1];             /* TE's FIL (0x824 bytes) */
static void *dir;
static int src_fd = -1;
volatile uint32_t py_export_files, py_export_skipped, py_export_failed;   /* (bench, debug) */
volatile uint32_t py_export_songs;

typedef struct ex_dir {
    const char *src;                            /* yaffs folder */
    const char *vol;                            /* the volume's folder ("/play_r") */
    int songs;                                  /* SONGnn.BIN only, as SONGnn.PSL */
} ex_dir;

static const ex_dir ex_renders = { PY_DIR_R, "/play_r", 0 };
static const ex_dir ex_songs = { PY_DIR_S, "/play_s", 1 };

static int is_slot(const char *n)              /* SONGnn.BIN */
{
    return n[0] == 'S' && n[1] == 'O' && n[2] == 'N' && n[3] == 'G' && n[4] >= '0' && n[4] <= '9'
           && n[5] >= '0' && n[5] <= '9' && n[6] == '.' && n[7] == 'B' && n[8] == 'I' && n[9] == 'N' && !n[10];
}

static void vol_path(char *out, const ex_dir *d, const char *name)
{
    char n[13];
    int i;

    for (i = 0; name[i] && i < 12; i++)
        n[i] = name[i];
    n[i] = 0;
    if (d->songs && i == 10) {                  /* SONGnn.BIN -> SONGnn.PSL */
        n[7] = 'P';
        n[8] = 'S';
        n[9] = 'L';
    }
    TE_SNPRINTF(out, 64, "%s/%s", d->vol, n);
}

static int ex_next(void *ctx, char name[13], uint32_t *size)
{
    static uint32_t st[32];                     /* TE's stat buffer is 0x48 bytes */
    const ex_dir *d = ctx;
    char path[64];
    uint8_t *de;
    int i;

    while (dir && (de = PY_READDIR(dir)) != 0) {
        const char *n = (const char *)de + 11;

        for (i = 0; n[i] && i < 12; i++)
            name[i] = n[i];
        name[i] = 0;
        if (n[0] == '.' || n[i] || (d->songs && !is_slot(name)))
            continue;
        TE_SNPRINTF(path, sizeof(path), "%s/%s", d->src, name);
        if (PY_STAT(path, st) < 0 || !(st[0x08 / 4] & 0x8000))
            continue;
        *size = st[0x1c / 4];
        return 1;
    }
    return 0;
}

static int ex_read(void *ctx, const char *name, uint32_t off, void *buf, uint32_t n)
{
    (void)ctx;
    (void)name;
    return src_fd < 0 ? -1 : PY_PREAD(src_fd, buf, n, (int32_t)off);
}

static int ex_create(void *ctx, const char *name)
{
    const ex_dir *d = ctx;
    char path[64];

    TE_SNPRINTF(path, sizeof(path), "%s/%s", d->src, name);
    src_fd = TE_FILE_OPEN(path, TE_FILE_RDONLY, TE_FILE_MODE);
    if (src_fd < 0)
        return -1;
    F_MKDIR(d->vol);                            /* (exists after the first: FR_EXIST) */
    vol_path(path, d, name);
    if (F_OPEN(fil, path, FA_WRITE_NEW)) {
        TE_FILE_CLOSE(src_fd);
        src_fd = -1;
        return -1;
    }
    return 0;
}

static int ex_write(void *ctx, const void *buf, uint32_t n)
{
    uint32_t bw = 0;

    (void)ctx;
    return F_WRITE(fil, buf, n, &bw) ? -1 : (int)bw;
}

static int ex_close(void *ctx)
{
    (void)ctx;
    if (src_fd >= 0)
        TE_FILE_CLOSE(src_fd);
    src_fd = -1;
    return F_CLOSE(fil);
}

static void ex_remove(void *ctx, const char *name)
{
    char path[64];

    vol_path(path, ctx, name);
    F_UNLINK(path);
}

static uint32_t ex_fat_free(void *ctx) { (void)ctx; return FAT_FREE(); }

static uint32_t ex_flash_free(void *ctx)
{
    int32_t f = YAFFS_FREE("/yaffs2");

    (void)ctx;
    return f < 0 ? 0 : (uint32_t)f;
}

/* one folder into the volume; 0 = nothing there */
static int export_dir(const ex_dir *d, void *buf, pex_result *res)
{
    pex_io io;

    dir = PY_OPENDIR(d->src);
    if (!dir)
        return 0;
    io.ctx = (void *)d;
    io.cluster = (FAT_CSIZE ? FAT_CSIZE : 64) * 512u;
    io.buf = buf;
    io.chunk = EXPORT_CHUNK;
    io.next = ex_next;
    io.read = ex_read;
    io.create = ex_create;
    io.write = ex_write;
    io.close = ex_close;
    io.remove = ex_remove;
    io.fat_free = ex_fat_free;
    io.flash_free = ex_flash_free;
    py_export_run(&io, res);
    PY_CLOSEDIR(dir);
    dir = 0;
    return 1;
}

/* call site 0x019975fa: TE's f_mount(x, NULL) on the way into disk mode (FatFs still mounted) */
void py_disk_enter_hook(void *vol, void *fs)
{
    static char line[48];
    pex_result res;
    void *buf;

    (void)vol;
    if (fs)
        return;
    buf = TE_HEAP_MALLOC(TE_HEAP_SAMPLES, EXPORT_CHUNK);
    if (!buf)
        return;
    F_MKDIR(ex_songs.vol);                      /* (always: the folder to drop shared songs into) */
    if (!py_pat_slot_busy() && export_dir(&ex_songs, buf, &res)) {    /* (a slot being written: not now) */
        py_export_songs = res.copied;
        TE_SNPRINTF(line, sizeof(line), "PLAY: %d songs in play_s", res.copied);
        CONSOLE_PRINT(line);
    }
    if (!py_rd_busy() && export_dir(&ex_renders, buf, &res)) {        /* (a render writes its file: not now) */
        py_export_files = res.copied;
        py_export_skipped = res.skipped;
        py_export_failed = res.failed;
        TE_SNPRINTF(line, sizeof(line), "PLAY: %d renders in play_r (%d did not fit)", res.copied,
                    res.skipped + res.failed);
        CONSOLE_PRINT(line);
    }
    TE_FREE(buf);
}
