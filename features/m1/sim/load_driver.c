/* M1 loader bench (build/apps/m1load: enginehost + m1 with the SoundFont tables, data from a file):
 * m1_main against stub TE file / heap functions serving build/m1_rom.bin from memory. Returns
 * failed-check bits (0 = pass). */
#include <op1/te246.h>
#include "../m1_core.h"

extern void m1_main(void);
extern void m1_frame(uint16_t *buf);
extern volatile uint8_t m1_load;
extern char FB[], FILE_DATA[], HEAP_BUF[], FILE_BYTES[];

enum { LD_IDLE, LD_READING, LD_READY, LD_NOFILE, LD_NOMEM, LD_BAD };
#define K(s, i)     TE_SYNTH_KNOBS[(s) * 8 + (i)]

static int file_ok, heap_ok, opens, closes, reads, allocs, open_fd = -1;
static uint32_t file_pos;
volatile int fails;

int file_open(const char *path, int oflag, int mode)
{
    static const char want[] = "/yaffs2/content/m1_rom.bin";
    int i;

    for (i = 0; want[i] && path[i] == want[i]; i++)
        ;
    if (want[i] || path[i] || oflag != 0 || mode != 0x100 || !file_ok)
        return -1;
    opens++;
    file_pos = 0;
    return open_fd = 3;
}

int file_read(int fd, void *buf, uint32_t n)
{
    uint32_t size = (uint32_t)FILE_BYTES, i;

    if (fd != open_fd)
        return -1;
    reads++;
    if (n > size - file_pos)
        n = size - file_pos;
    for (i = 0; i < n; i++)
        ((char *)buf)[i] = FILE_DATA[file_pos + i];
    file_pos += n;
    return (int)n;
}

int file_close(int fd)
{
    if (fd == open_fd) {
        closes++;
        open_fd = -1;
    }
    return 0;
}

void *heap_malloc(int heap, uint32_t size)
{
    if (heap != 1 || size != (uint32_t)FILE_BYTES || !heap_ok)
        return 0;
    allocs++;
    return HEAP_BUF;
}

#define CHECK(bit, c) do { if (!(c)) fails |= 1 << (bit); } while (0)

static int loud(void)                           /* a note's first blocks: any sample above 500? */
{
    static const int16_t p[M1_PARAMS] = { 0, 0, 0, 32767 };   /* wave 0 (PIANO), cutoff open */
    static m1_voice v;
    static int16_t out[128];
    static uint32_t inc[128];
    int i, b, m = 0;

    for (i = 0; i < 128; i++)
        inc[i] = 21426141u;                     /* A4 */
    m1_note_on(&v, inc[0], p);
    for (b = 0; b < 20; b++) {
        m1_render(&v, out, 128, inc, p);
        for (i = 0; i < 128; i++)
            if (out[i] > m || -out[i] > m)
                m = out[i] > 0 ? out[i] : -out[i];
    }
    return m > 500;
}

static void run(int n)
{
    while (n--)
        m1_main();
}

int main(void)
{
    static const int16_t m1_sound[8] = { 81, 81, 0, 16383, 13824, 12288, 12288, 12288 };
    int i;

    *TE_EDIT_MODE = 0;
    *TE_SYNTH_SUBSCREEN = 0;
    *TE_SYNTH_SLOT = 2;
    for (i = 0; i < 8; i++)
        TE_SYNTH_TYPE[i] = TE_INSTR_DRWAVE;     /* plain drwave sounds (all knobs 0) */

    /* 0: no M1 sound in any slot: nothing opened, no data, notes silent */
    run(300);
    CHECK(0, m1_load == LD_IDLE && opens == 0 && allocs == 0 && !loud());

    /* 1: an M1 sound in slot 3, no file: NOFILE, nothing allocated, stays */
    for (i = 0; i < 8; i++)
        K(2, i) = m1_sound[i];
    run(5);
    CHECK(1, m1_load == LD_NOFILE && allocs == 0 && !loud());

    /* 2: the file is there, TE's sample memory full: NOMEM, the file closed again; retried after
     * ~250 passes, not before */
    m1_load = LD_IDLE;
    file_ok = 1;
    run(1);
    CHECK(2, m1_load == LD_NOMEM && opens == 1 && closes == 1 && allocs == 0);
    run(240);
    CHECK(2, opens == 1);
    m1_frame((uint16_t *)(FB + 0x40000));       /* the screen: NO SAMPLE MEMORY FREE */

    /* 3: room now, but the file is damaged (one byte): read in full, BAD, still silent */
    heap_ok = 1;
    FILE_DATA[123456] ^= 0x40;
    run(40);
    CHECK(3, opens == 2 && allocs == 1 && m1_load == LD_BAD && closes == 2 && !loud());
    FILE_DATA[123456] ^= 0x40;

    /* 4: the good file: 64 KB per pass, the screen shows the progress, then READY with the data
     * in TE's sample memory (the buffer reused, not allocated twice) */
    m1_load = LD_IDLE;
    reads = 0;
    run(6);
    CHECK(4, m1_load == LD_READING && !loud());
    m1_frame((uint16_t *)FB);                   /* LOADING SAMPLES n% */
    run(30);
    CHECK(4, m1_load == LD_READY && allocs == 1 && closes == 3
          && reads >= (int)(((uint32_t)FILE_BYTES + 65535) >> 16)
          && reads <= (int)(((uint32_t)FILE_BYTES + 65535) >> 16) + 1);
    for (i = 0; i < (int)(uint32_t)FILE_BYTES; i++)
        if (HEAP_BUF[i] != FILE_DATA[i])
            break;
    CHECK(4, i == (int)(uint32_t)FILE_BYTES);

    /* 5: notes sound now; further passes do nothing */
    CHECK(5, loud());
    run(300);
    CHECK(5, opens == 3 && allocs == 1 && m1_load == LD_READY);
    m1_frame((uint16_t *)(FB + 0x80000));       /* loaded: the usual screen */
    return fails;
}
