/* diskinfo: what fills the OP-1's internal flash (YAFFS2, mounted at /yaffs2). Debug, read-only.
 *
 * shift + speech toggles a full-screen panel: free / total space, then the biggest folders and
 * files (MB), measured with TE's own YAFFS calls (the ones its dump fat_utils.cpp 0x01998c14
 * uses for /log/files.txt). One walk per toggle, in the GUI thread; nothing is ever written.
 *
 * Groups: paths below /yaffs2 cut after 2 parts ("content/display"), after 3 under user/ and
 * content/audio/ ("user/audio/drum", "user/album/side_a.wav", "content/audio/factory_drum"); "user/"
 * and "content/" are left off on screen. */
#include <op1/te246.h>
#include <op1/gfx.h>

/* YAFFS direct interface (R246), from the walker 0x01998a5a and the dump 0x01998c14 */
#define DI_OPENDIR   ((void *(*)(const char *path))0x01a9b450)
#define DI_READDIR   ((uint8_t *(*)(void *dir))0x01a9b648)     /* dirent, d_name at +11 */
#define DI_CLOSEDIR  ((int (*)(void *dir))0x01a9b74c)
#define DI_STAT      ((int (*)(const char *path, void *st))0x01a9a69c)
#define DI_FREESPACE ((int32_t (*)(const char *path))0x01a9b1c0)
#define DI_TOTALSPACE ((int32_t (*)(const char *path))0x01a9b284)
#define DI_NAME      11
#define DI_ST_MODE   0x08                                       /* 0x4000 dir, 0x8000 file */
#define DI_ST_SIZE   0x1c

#define DI_DEPTH  8
#define DI_PATH   128
#define DI_GROUPS 32
#define DI_ROWS   9

static volatile int on, want_walk;
static char path[DI_DEPTH][DI_PATH];
static uint32_t st[32];                         /* yaffs_stat buffer (TE's is 0x48 bytes) */
static char gname[DI_GROUPS][DI_PATH];
static uint32_t gbytes[DI_GROUPS];
static int ngroups, nfiles;
static uint32_t other_bytes, sum_bytes;
static int32_t free_bytes, total_bytes;

static int is_dot(const char *s)
{
    return s[0] == '.' && (!s[1] || (s[1] == '.' && !s[2]));
}

/* group key of a path below "/yaffs2/" */
static void group_key(const char *rel, char *out)
{
    int parts = (!TE_STRNCMP(rel, "user/", 5) || !TE_STRNCMP(rel, "content/audio/", 14)) ? 3 : 2, i;

    for (i = 0; rel[i] && i < DI_PATH - 1; i++) {
        if (rel[i] == '/' && --parts == 0)
            break;
        out[i] = rel[i];
    }
    out[i] = 0;
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static void add_file(const char *full, uint32_t size)
{
    char key[DI_PATH];
    int i;

    nfiles++;
    sum_bytes += size;
    group_key(full + 8, key);                   /* skip "/yaffs2/" */
    for (i = 0; i < ngroups; i++)
        if (str_eq(gname[i], key)) {
            gbytes[i] += size;
            return;
        }
    if (ngroups == DI_GROUPS) {
        other_bytes += size;
        return;
    }
    TE_SNPRINTF(gname[ngroups], DI_PATH, "%s", key);
    gbytes[ngroups++] = size;
}

static void walk(int depth)
{
    void *dir = DI_OPENDIR(path[depth]);
    uint8_t *de;

    if (!dir)
        return;
    while ((de = DI_READDIR(dir)) != 0) {
        const char *name = (const char *)de + DI_NAME;
        uint32_t mode;

        if (is_dot(name) || depth + 1 >= DI_DEPTH)
            continue;
        TE_SNPRINTF(path[depth + 1], DI_PATH, "%s/%s", path[depth], name);
        if (DI_STAT(path[depth + 1], st) < 0)
            continue;
        mode = st[DI_ST_MODE / 4];
        if (mode & 0x4000)
            walk(depth + 1);
        else if (mode & 0x8000)
            add_file(path[depth + 1], st[DI_ST_SIZE / 4]);
    }
    DI_CLOSEDIR(dir);
}

static void measure(void)
{
    int i, j;

    ngroups = nfiles = 0;
    other_bytes = sum_bytes = 0;
    free_bytes = DI_FREESPACE("/yaffs2");
    total_bytes = DI_TOTALSPACE("/yaffs2");
    TE_SNPRINTF(path[0], DI_PATH, "/yaffs2");
    walk(0);
    for (i = 1; i < ngroups; i++)               /* biggest first */
        for (j = i; j > 0 && gbytes[j] > gbytes[j - 1]; j--) {
            char t[DI_PATH];
            uint32_t b = gbytes[j];

            TE_MEMCPY(t, gname[j], DI_PATH);
            TE_MEMCPY(gname[j], gname[j - 1], DI_PATH);
            TE_MEMCPY(gname[j - 1], t, DI_PATH);
            gbytes[j] = gbytes[j - 1];
            gbytes[j - 1] = b;
        }
}

/* "123.4" MB, one decimal */
static int mb(int x, int y, uint32_t bytes, uint16_t c)
{
    uint32_t tenths = ((bytes >> 10) * 10 + 512) >> 10;

    x = gfx_dec(x, y, (int32_t)(tenths / 10), c, 2);
    x = gfx_text(x, y, ".", c, 2);
    return gfx_dec(x, y, (int32_t)(tenths % 10), c, 2);
}

/* on_key filter (KeyHandler thread): shift + speech toggles the panel */
int di_key(int key, int down)
{
    if (key != TE_KEY_SPEECH || !*TE_SHIFT_HELD)
        return 0;
    if (down) {
        on = !on;
        want_walk = on;
    }
    return 1;
}

/* frame (GUI thread): walk once after the toggle, then draw the panel */
void di_frame(uint16_t *buf)
{
    int i, x, y = 2;
    uint32_t rest = other_bytes;

    if (!on)
        return;
    if (want_walk) {
        want_walk = 0;
        measure();
    }
    gfx_begin(buf);
    gfx_rect(0, 0, gfx_w, gfx_h, GFX_BLACK);

    x = gfx_text(2, y, "FREE ", GFX_GREEN, 2);
    x = mb(x, y, (uint32_t)free_bytes, GFX_GREEN);
    x = gfx_text(x, y, " OF ", GFX_WHITE, 2);
    x = mb(x, y, (uint32_t)total_bytes, GFX_WHITE);
    gfx_text(x, y, " MB", GFX_WHITE, 2);
    y += 12;
    x = gfx_text(2, y, "FILES ", GFX_WHITE, 2);
    x = gfx_dec(x, y, nfiles, GFX_WHITE, 2);
    x = gfx_text(x, y, " SUM ", GFX_WHITE, 2);
    x = mb(x, y, sum_bytes, GFX_WHITE);
    x = gfx_text(x + 8, y, "MODE ", GFX_GREEN, 2);  /* TE's edit mode, live (which screen has the keys) */
    gfx_dec(x, y, *TE_EDIT_MODE, GFX_GREEN, 2);
    y += 14;

    for (i = 0; i < ngroups; i++) {
        const char *n = gname[i];

        if (i >= DI_ROWS) {
            rest += gbytes[i];
            continue;
        }
        if (!TE_STRNCMP(n, "user/", 5))
            n += 5;
        else if (!TE_STRNCMP(n, "content/", 8))
            n += 8;
        gfx_text(2, y, n, GFX_WHITE, 2);
        mb(gfx_w - 2 - 6 * 8, y, gbytes[i], GFX_WHITE);
        y += 12;
    }
    if (rest) {
        gfx_text(2, y, "OTHER", GFX_WHITE, 2);
        mb(gfx_w - 2 - 6 * 8, y, rest, GFX_WHITE);
    }
}
