/* slotlink bench: TE's directory copies (stub) through the call-site hooks, a small in-memory file
 * system behind TE's file calls. Returns failed-check bits (0 = pass). */
#include <op1/te246.h>

extern void hook_synth(const char *dir, const char *root);     /* __sdk_hook_0196f8b6 */
extern void hook_drum(const char *dir, const char *root);      /* __sdk_hook_0196f8ce */
extern volatile int sl_made, sl_failed;

#define FILES   16
#define DATA    9000

typedef struct { char path[96]; int len, used; unsigned char data[DATA]; } file;
typedef struct { int f, pos, wr, open; } desc;

static file fs[FILES];
static desc fd[8];
static int copies, opens_w;
volatile int fails;
static int fail_writes;                         /* the disk "fills up" */

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static void cpy(char *d, const char *s) { while ((*d++ = *s++)) ; }

static int find(const char *p)
{
    int i;
    for (i = 0; i < FILES; i++)
        if (fs[i].used && same(fs[i].path, p))
            return i;
    return -1;
}

static int add(const char *p, int len, int seed)
{
    int i = 0, k;
    while (fs[i].used)
        i++;
    fs[i].used = 1;
    cpy(fs[i].path, p);
    fs[i].len = len;
    for (k = 0; k < len; k++)
        fs[i].data[k] = (unsigned char)(k * 7 + seed);
    return i;
}

int fs_open(const char *path, int oflag, int mode)
{
    int f = find(path), d;
    (void)mode;
    if (oflag == TE_FILE_CREATE) {
        opens_w++;
        if (f < 0)
            f = add(path, 0, 0);
        fs[f].len = 0;
    } else if (f < 0) {
        return -1;
    }
    for (d = 0; fd[d].open; d++)
        ;
    fd[d].open = 1; fd[d].f = f; fd[d].pos = 0; fd[d].wr = oflag == TE_FILE_CREATE;
    return d;
}

int fs_read(int d, void *buf, uint32_t n)
{
    file *f = &fs[fd[d].f];
    int k, left = f->len - fd[d].pos;
    if ((int)n > left)
        n = (uint32_t)left;
    for (k = 0; k < (int)n; k++)
        ((unsigned char *)buf)[k] = f->data[fd[d].pos + k];
    fd[d].pos += (int)n;
    return (int)n;
}

int fs_write(int d, const void *buf, uint32_t n)
{
    file *f = &fs[fd[d].f];
    int k;
    if (fail_writes)
        return -1;
    for (k = 0; k < (int)n && f->len < DATA; k++)
        f->data[f->len++] = ((const unsigned char *)buf)[k];
    return k;
}

int fs_close(int d) { fd[d].open = 0; return 0; }

/* TE's copy: here only the files the update still carries */
void dir_copy(const char *dir, const char *root)
{
    (void)root;
    copies++;
    if (same(dir, "audio/drum") && find("/yaffs2/user/audio/drum/drum_4.raw") < 0)
        add("/yaffs2/user/audio/drum/drum_4.raw", 100, 9);      /* kept in the update: TE copies it */
}

static int equal(const char *a, const char *b)
{
    int i = find(a), j = find(b), k;
    if (i < 0 || j < 0 || fs[i].len != fs[j].len)
        return 0;
    for (k = 0; k < fs[i].len; k++)
        if (fs[i].data[k] != fs[j].data[k])
            return 0;
    return 1;
}

#define CHECK(bit, c) do { if (!(c)) fails |= 1 << (bit); } while (0)

int main(void)
{
    static const char *const root = "/yaffs2/";
    int d4;

    add("/yaffs2/content/audio/factory_drum/peek.raw", 5000, 1);       /* > 2 read blocks */
    add("/yaffs2/content/audio/factory_drum/awa beef.raw", 2048, 2);   /* exactly one block */
    add("/yaffs2/content/audio/factory_drum/neoprene.raw", 300, 3);
    add("/yaffs2/content/audio/factory_drum/acetate.raw", 1, 4);
    /* red maple missing in content: counted as failed, nothing created */
    add("/yaffs2/content/audio/factory_synth/suitcase.raw", 4100, 5);
    add("/yaffs2/user/audio/drum/drum_5.raw", 77, 6);                  /* the user's file: kept */

    /* 0: synth copy: TE's copy ran, synth_7 made from suitcase */
    hook_synth("audio/synth", root);
    CHECK(0, copies == 1 && equal("/yaffs2/user/audio/synth/synth_7.raw", "/yaffs2/content/audio/factory_synth/suitcase.raw"));
    CHECK(0, find("/yaffs2/user/audio/drum/drum_0.raw") < 0);          /* nothing of the drum dir yet */

    /* 1: drum copy: 0, 2 made; 4 copied by TE, untouched; 5 the user's, untouched; 6: red maple missing */
    hook_drum("audio/drum", root);
    d4 = find("/yaffs2/user/audio/drum/drum_4.raw");
    CHECK(1, copies == 2);
    CHECK(1, equal("/yaffs2/user/audio/drum/drum_0.raw", "/yaffs2/content/audio/factory_drum/peek.raw"));
    CHECK(1, equal("/yaffs2/user/audio/drum/drum_2.raw", "/yaffs2/content/audio/factory_drum/awa beef.raw"));
    CHECK(1, find("/yaffs2/user/audio/drum/drum_6.raw") < 0);
    CHECK(2, d4 >= 0 && fs[d4].len == 100 && fs[d4].data[0] == 9);
    CHECK(2, fs[find("/yaffs2/user/audio/drum/drum_5.raw")].len == 77);
    CHECK(3, sl_made == 3 && sl_failed == 1 && opens_w == 3);

    /* 4: again: nothing new, nothing rewritten (red maple still missing: failed again) */
    hook_drum("audio/drum", root);
    CHECK(4, sl_made == 3 && opens_w == 3 && sl_failed == 2);

    /* 5: other directories: nothing */
    hook_drum("audio/preset_drum", root);
    CHECK(5, sl_made == 3 && sl_failed == 2);

    /* 6: a failing write: counted, every descriptor closed */
    add("/yaffs2/content/audio/factory_drum/red maple.raw", 10, 8);
    fail_writes = 1;
    hook_drum("audio/drum", root);
    CHECK(6, sl_made == 3 && sl_failed == 3 && opens_w == 4 && !fd[0].open && !fd[1].open && !fd[2].open);
    return fails;
}
