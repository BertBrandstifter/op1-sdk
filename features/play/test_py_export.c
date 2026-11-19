/* host test for py_export.c (tools/test_all.py builds it with its sibling py_export.c).
 *
 * Renders copied into a fake volume: all copied byte for byte (several chunks, an empty file);
 * a file that does not fit the volume or the flash skipped, the next smaller one still copied;
 * a read error: that file removed, the rest copied; a short write: removed and stopped; a
 * failing close: removed. */
#include <stdio.h>
#include <string.h>
#include "py_export.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define NF 4
static const char *names[NF] = { "REND01.AIF", "REND02.AIF", "REND03.AIF", "REND04.AIF" };
static uint32_t sizes[NF];
static uint8_t data[NF][300000];

static uint8_t vol[NF][300000];                 /* the volume's copies */
static uint32_t vlen[NF];
static int dir_made;
static int vexists[NF], cur = -1, it, read_fail = -1, write_short_at = -1, close_fail = -1;
static uint32_t fat_free_b, flash_free_b, writes;
static uint32_t buf[1024];

static int idx(const char *n)
{
    int i;

    for (i = 0; i < NF; i++)
        if (!strcmp(n, names[i]))
            return i;
    return -1;
}

static int next(void *c, char name[13], uint32_t *size)
{
    (void)c;
    if (it >= NF)
        return 0;
    strcpy(name, names[it]);
    *size = sizes[it++];
    return 1;
}

static int rd(void *c, const char *name, uint32_t off, void *b, uint32_t n)
{
    int i = idx(name);

    (void)c;
    if (i == read_fail && off > 0)
        return -1;
    memcpy(b, data[i] + off, n);
    return (int)n;
}

static int create(void *c, const char *name)
{
    (void)c;
    cur = idx(name);
    if (!dir_made) {                            /* the folder takes a cluster */
        dir_made = 1;
        fat_free_b -= 4096;
    }
    vexists[cur] = 1;
    vlen[cur] = 0;
    return 0;
}

static int wr(void *c, const void *b, uint32_t n)
{
    (void)c;
    if ((int)writes++ == write_short_at)
        n /= 2;
    memcpy(vol[cur] + vlen[cur], b, n);
    vlen[cur] += n;
    fat_free_b -= n;
    flash_free_b -= n;
    return (int)n;
}

static int cls(void *c)
{
    (void)c;
    return cur == close_fail ? -1 : 0;
}

static void rm(void *c, const char *name)
{
    (void)c;
    vexists[idx(name)] = 0;
}

static uint32_t ffree(void *c) { (void)c; return fat_free_b; }
static uint32_t lfree(void *c) { (void)c; return flash_free_b; }

static pex_io io = { 0, 4096, buf, sizeof(buf), next, rd, create, wr, cls, rm, ffree, lfree };

static void reset(uint32_t fat, uint32_t flash)
{
    memset(vexists, 0, sizeof(vexists));
    memset(vlen, 0, sizeof(vlen));
    it = 0;
    writes = 0;
    dir_made = 0;
    read_fail = write_short_at = close_fail = -1;
    fat_free_b = fat;
    flash_free_b = flash;
}

int main(void)
{
    pex_result r;
    int i, j;

    sizes[0] = 300000;                          /* 74 chunks */
    sizes[1] = 0;
    sizes[2] = 4096;
    sizes[3] = 123457;
    for (i = 0; i < NF; i++)
        for (j = 0; j < 300000; j++)
            data[i][j] = (uint8_t)(j * 7 + i * 13 + (j >> 9));

    /* all fit */
    reset(100u << 20, 200u << 20);
    py_export_run(&io, &r);
    CHECK(r.copied == 4 && !r.skipped && !r.failed && r.bytes == 300000u + 4096u + 123457u);
    for (i = 0; i < NF; i++)
        CHECK(vexists[i] && vlen[i] == sizes[i] && !memcmp(vol[i], data[i], sizes[i]));

    /* the volume: room for the small ones only (the big first one skipped, the folder cluster
     * then counted for the next) */
    reset(PEX_FAT_RESERVE + 200000u, 200u << 20);
    py_export_run(&io, &r);
    CHECK(!vexists[0] && vexists[1] && vexists[2] && vexists[3] && r.copied == 3 && r.skipped == 1);
    reset(PEX_FAT_RESERVE + 4096u, 200u << 20);  /* the folder's cluster, then nothing more */
    py_export_run(&io, &r);
    CHECK(r.copied == 1 && vexists[1] && r.skipped == 3);   /* the 0-byte file: only the folder's cluster */

    /* the flash: margin kept */
    reset(100u << 20, PEX_FLASH_RESERVE + 200000u);
    py_export_run(&io, &r);
    CHECK(!vexists[0] && vexists[2] && vexists[3] && r.skipped == 1);

    /* a read error: that file removed, the next still copied */
    reset(100u << 20, 200u << 20);
    read_fail = 0;
    py_export_run(&io, &r);
    CHECK(!vexists[0] && vexists[1] && vexists[2] && vexists[3] && r.failed == 1 && r.copied == 3);

    /* a short write: removed, stopped */
    reset(100u << 20, 200u << 20);
    write_short_at = 3;
    py_export_run(&io, &r);
    CHECK(!vexists[0] && !vexists[1] && r.failed == 1 && r.copied == 0);

    /* a failing close: removed */
    reset(100u << 20, 200u << 20);
    close_fail = 2;
    py_export_run(&io, &r);
    CHECK(vexists[0] && !vexists[2] && vexists[3] && r.failed == 1 && r.copied == 3);

    if (fails)
        printf("%d checks failed\n", fails);
    else
        printf("py_export: all checks passed\n");
    return fails != 0;
}
