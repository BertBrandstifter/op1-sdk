/* playprobe bench: the composed probe with TE stand-ins. Checks: nothing before the panel
 * opens; the callback wrap (TE's callback still runs every block); the tone in both / word 0 /
 * word 1 and saturation; the mix test (allocated, costs cycles, never reaches the output, freed);
 * the disk scan of a FAT16 image (no image -> error; play/ listed, hidden files skipped, headers);
 * the read test (reads through A1, no errors); the heap test (free list + largest block, all
 * freed); keys / knobs consumed only while the panel is open. Returns failed-check bits. */
#include <op1/te246.h>
#include <op1/fat.h>
#include <op1/audiofile.h>

typedef struct ppf { fat_ent e; aud_info a; int8_t chain; } ppf;  /* as playprobe.c */

extern void compose_init(void);
extern void hook_key(int key, int down);
extern void hook_knob(int knob, int16_t *diff);
extern void hook_frame(uint16_t *buf);
extern void pp_main(void);
extern volatile uint32_t pp_te, pp_pp, pp_p, pp_windows, pp_tx_peak, pp_rewraps;
extern volatile uint8_t pp_tone_mode, pp_mix_n, pp_nfiles, pp_scan_state, pp_speed_on;
extern volatile int8_t pp_scan_err;
extern int16_t *volatile pp_mix_buf;
extern ppf pp_files[];
extern int pp_a1_index;
extern volatile uint32_t pp_sp_reads, pp_sp_errs;
extern volatile uint32_t pp_hp_list_sum, pp_hp_list_big, pp_hp_list_n, pp_hp_big, pp_hp_total, pp_hp_blocks;
extern volatile uint8_t pp_hp_state, pp_hp_list_ok;
extern char FB[];
extern uint8_t IMG[], IMG_END[];

#define WIN 172
volatile int fails;
volatile uint32_t sink;

/* ------------------------------------------------------------------ TE stand-ins */

static int keys_to_te, knobs_to_te, opens, closes, preads, fail_open;

void log_key(int key, int down) { (void)key; (void)down; keys_to_te++; }
void log_knob(int knob, int16_t *diff) { (void)knob; (void)diff; knobs_to_te++; }
void nop(void) {}
static int flushes;
void cache_flush(void *a, void *b, int inv) { (void)a; (void)b; (void)inv; flushes++; }

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

int file_open(const char *path, int oflag, int mode)
{
    (void)oflag; (void)mode;
    if (fail_open || !streq(path, "/yaffs2/user/tape_c.raw"))
        return -1;
    opens++;
    return 7;
}

int file_close(int fd) { (void)fd; closes++; return 0; }

int file_stat(const char *path, uint32_t *st)
{
    if (!streq(path, "/yaffs2/user/tape_c.raw"))
        return -1;
    st[0x1c / 4] = (uint32_t)(IMG_END - IMG);
    return 0;
}

int file_pread(int fd, void *buf, uint32_t n, int32_t off)
{
    uint32_t size = (uint32_t)(IMG_END - IMG), i;
    uint8_t *b = buf;

    if (fd != 7 || off < 0 || (uint32_t)off > size)
        return -1;
    if (n > size - (uint32_t)off)
        n = size - (uint32_t)off;
    for (i = 0; i < n; i++)
        b[i] = IMG[off + i];
    preads++;
    return (int)n;
}

/* sample heap: a 3 MB arena; a block goes above the highest live one */
#define ARENA       0x03100000u
#define ARENA_SIZE  (3u << 20)
static uint32_t live_at[16], live_n[16];
static int nlive;

void *heap_malloc(int heap, uint32_t size)
{
    uint32_t top = ARENA;
    int i;

    if (heap != 1 || nlive == 16)
        return 0;
    for (i = 0; i < nlive; i++)
        if (live_at[i] + live_n[i] > top)
            top = live_at[i] + live_n[i];
    size = (size + 15) & ~15u;
    if (size > ARENA + ARENA_SIZE - top)
        return 0;
    live_at[nlive] = top;
    live_n[nlive++] = size;
    return (void *)top;
}

void heap_free(void *p)
{
    int i;

    for (i = 0; i < nlive; i++)
        if (live_at[i] == (uint32_t)p) {
            live_at[i] = live_at[nlive - 1];
            live_n[i] = live_n[--nlive];
            return;
        }
}

static uint32_t desc[8];                    /* +0x18: the free list */

const void *heap_desc(int heap)
{
    volatile uint32_t *n1 = (uint32_t *)0x03050000, *n2 = (uint32_t *)0x03060000, *n3 = (uint32_t *)0x03070000;

    if (heap != 1)
        return 0;
    n1[0] = 0x20000; n1[1] = 1; n1[2] = (uint32_t)n2;
    n2[0] = 0x40000; n2[1] = 1; n2[2] = (uint32_t)n3;
    n3[0] = 0x1000;  n3[1] = 0; n3[2] = 0;                  /* another heap's: not counted */
    desc[6] = (uint32_t)n1;
    return desc;
}

void *mem_set(void *d, int c, uint32_t n)
{
    uint8_t *p = d;

    while (n--)
        *p++ = (uint8_t)c;
    return d;
}

/* ------------------------------------------------------------------ TE's audio callback */

static int32_t tx[128], rx[128];
static int calls;
static int32_t te_even = 0x02000000, te_odd = -0x01000000;

static void te_cb(int32_t *t, const int32_t *r)
{
    int i;

    (void)r;
    calls++;
    for (i = 0; i < 128; i += 2) {
        t[i] = te_even;
        t[i + 1] = te_odd;
    }
}

static void blocks(int n)
{
    int i;

    for (i = 0; i < n; i++)
        (*TE_AUDIO_BLOCK_CB)(tx, rx);
}

/* words of the last block that differ from what TE wrote: bit 0 even, bit 1 odd */
static int changed(void)
{
    int i, m = 0;

    for (i = 0; i < 128; i += 2) {
        m |= tx[i] != te_even;
        m |= (tx[i + 1] != te_odd) << 1;
    }
    return m;
}

static void press(int key)
{
    hook_key(key, 1);
    hook_key(key, 0);
}

static void turn(int knob, int steps)
{
    int16_t d = steps > 0 ? 1 : -1;

    while (steps) {
        hook_knob(knob, &d);
        steps += steps > 0 ? -1 : 1;
    }
}

#define CHECK(bit, c) do { if (!(c)) fails |= 1 << (bit); } while (0)

int main(void)
{
    uint32_t idle_pp;
    int i, a1 = -1, a3 = -1, txt = -1;

    compose_init();
    *TE_EDIT_MODE = 2;
    *TE_AUDIO_BLOCK_CB = te_cb;

    /* 0: nothing before the panel opens; knobs and keys go to TE */
    pp_main();
    blocks(2);
    turn(3, 2);
    press(TE_KEY_GREEN_PUSH);
    CHECK(0, *TE_AUDIO_BLOCK_CB == te_cb && calls == 2 && knobs_to_te == 2 && keys_to_te == 2
          && pp_mix_n == 0);

    /* 1: shift + speech opens; wrapped at the next main tick; TE's callback runs every block;
     * output untouched with the tone off */
    *TE_SHIFT_HELD = 1;
    press(TE_KEY_SPEECH);
    *TE_SHIFT_HELD = 0;
    pp_main();
    CHECK(1, *TE_AUDIO_BLOCK_CB != te_cb);
    blocks(WIN + 2);
    CHECK(1, calls == 2 + WIN + 2 && pp_windows == 1 && changed() == 0);
    CHECK(2, pp_te > 0 && pp_p > pp_te && pp_tx_peak == 0x02000000);
    idle_pp = pp_pp;

    /* 3: tone: both, word 0, word 1, off */
    press(TE_KEY_BLUE_PUSH);
    blocks(1);
    CHECK(3, pp_tone_mode == 1 && changed() == 3 && flushes == 1);     /* tx in SDRAM here */
    press(TE_KEY_BLUE_PUSH);
    blocks(1);
    CHECK(3, changed() == 1);
    press(TE_KEY_BLUE_PUSH);
    blocks(1);
    CHECK(3, changed() == 2);
    press(TE_KEY_BLUE_PUSH);
    blocks(1);
    CHECK(3, pp_tone_mode == 0 && changed() == 0);

    /* 4: saturation: TE at full scale + the loudest tone never wraps */
    te_even = 0x7fff0000;
    te_odd = (int32_t)0x80010000;
    turn(0, 5);
    press(TE_KEY_BLUE_PUSH);
    for (i = 0; i < 200; i++) {
        int k, bad = 0;

        blocks(1);
        for (k = 0; k < 128; k += 2)
            bad |= tx[k] < 0x40000000 || tx[k + 1] > -0x40000000;
        CHECK(4, !bad);
    }
    press(TE_KEY_BLUE_PUSH);
    press(TE_KEY_BLUE_PUSH);
    press(TE_KEY_BLUE_PUSH);
    te_even = 0x02000000;
    te_odd = -0x01000000;

    /* 5: mix test: 8 streams allocated, cost cycles, never reach the output; freed at 0 */
    turn(3, 9);
    pp_main();
    CHECK(5, pp_mix_n == 8 && pp_mix_buf && nlive == 1);
    blocks(2 * WIN + 1);                        /* one whole window with the mix only */
    CHECK(5, changed() == 0 && pp_pp > idle_pp + 8 * 128);
    turn(3, -9);
    pp_main();
    CHECK(5, pp_mix_n == 0 && !pp_mix_buf && nlive == 0);

    /* 6: disk scan without an image -> error 1, nothing left open */
    fail_open = 1;
    press(TE_KEY_GREEN_PUSH);
    pp_main();
    CHECK(6, pp_scan_state == 2 && pp_scan_err == 1);
    fail_open = 0;

    /* 7: disk scan of the FAT16 image: play/ without the hidden "._" file, headers parsed */
    press(TE_KEY_GREEN_PUSH);
    pp_main();
    CHECK(7, pp_scan_state == 2 && pp_scan_err == 0 && pp_nfiles == 4 && opens == closes);
    for (i = 0; i < pp_nfiles; i++) {
        const char *n = pp_files[i].e.name;

        if (streq(n, "A1.WAV"))
            a1 = i;
        if (streq(n, "A3.AIF"))
            a3 = i;
        if (streq(n, "SET.TXT"))
            txt = i;
    }
    CHECK(8, a1 >= 0 && pp_a1_index == a1 && pp_files[a1].a.err == AUD_OK && pp_files[a1].a.channels == 2
          && pp_files[a1].a.frames == 50000 && !pp_files[a1].chain);
    CHECK(8, a3 >= 0 && pp_files[a3].a.err == AUD_ERATE && pp_files[a3].a.rate == 48000);
    CHECK(8, txt >= 0 && pp_files[txt].a.err == AUD_EFORMAT);

    /* 9: read test through A1's data (64 KB per pass, loops at the end): no errors */
    press(TE_KEY_WHITE_PUSH);
    for (i = 0; i < 12; i++)
        pp_main();
    CHECK(9, pp_speed_on && pp_sp_reads == 12 && pp_sp_errs == 0 && nlive == 1);
    press(TE_KEY_WHITE_PUSH);
    pp_main();
    CHECK(9, !pp_speed_on && nlive == 0 && opens == closes);

    /* 10: heap test: the free list (2 nodes of heap 1 + one of another heap), the largest block
     * of the 3 MB arena, everything freed */
    press(TE_KEY_ORANGE_PUSH);
    pp_main();
    CHECK(10, pp_hp_state == 1 && pp_hp_list_ok == 1 && pp_hp_list_n == 3 && pp_hp_list_sum == 0x60000
          && pp_hp_list_big == 0x40000);
    CHECK(10, pp_hp_big > ARENA_SIZE - 16384 && pp_hp_big <= ARENA_SIZE && pp_hp_blocks == 1
          && pp_hp_total == pp_hp_big && nlive == 0);

    /* 11: keys other than the knob presses still reach TE while open */
    keys_to_te = 0;
    press(TE_KEY_PLAY);
    press(TE_KEY_ORANGE_PUSH);              /* consumed (and runs the heap test once more) */
    pp_main();
    CHECK(11, keys_to_te == 2);

    /* 12: TE re-sets its callback: wrapped again */
    *TE_AUDIO_BLOCK_CB = te_cb;
    pp_main();
    CHECK(12, *TE_AUDIO_BLOCK_CB != te_cb && pp_rewraps == 1);

    /* the panel (rescan for the list, tone on for the label) */
    press(TE_KEY_GREEN_PUSH);
    pp_main();
    press(TE_KEY_BLUE_PUSH);
    blocks(WIN);
    hook_frame((uint16_t *)FB);

    /* 13: closed again: knobs go to TE */
    *TE_SHIFT_HELD = 1;
    press(TE_KEY_SPEECH);
    *TE_SHIFT_HELD = 0;
    knobs_to_te = 0;
    turn(2, 1);
    CHECK(13, knobs_to_te == 1);
    return fails;
}
