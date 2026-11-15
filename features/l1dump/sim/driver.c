/* l1dump bench driver: planted L1 content -> boot_done capture -> audio callback stream ->
 * capture buffer (the runner decodes it and byte-compares), plus the USB carrier patching and
 * the status overlay. Returns failed-check bits (0 = pass; the runner prints them). */
#include <op1/te246.h>

extern int  hook_key(int key, int down);/* composed on_key dispatcher */
extern void hook_frame(uint16_t *buf);  /* composed frame dispatcher */

extern uint8_t *ld_img;
extern volatile uint32_t ld_img_size, ld_ready, ld_usb_patched;
extern volatile int ld_tx_on;
extern void ld_cb(int32_t *tx, const int32_t *rx);
extern void ld_msc_after(uint8_t *buf, uint32_t sector, uint32_t nsectors);

#define L1I ((uint8_t *)0x02180000)   /* the sim's L1I rejects data writes: SDRAM mirror */
#define L1A ((uint8_t *)0xff800000)
#define L1B ((uint8_t *)0xff900000)
#define SCR ((uint8_t *)0xffb00000)
#define REG_BYTES 0x400                 /* SDK_BENCH region size (l1dump.c) */
#define IMG_BYTES (12 + 4 * 16 + 4 * REG_BYTES + 4)
#define NPKTS     ((IMG_BYTES + 119) / 120)
#define LOOP_SYMS (64 + NPKTS * 332)
#define BLOCKS    (LOOP_SYMS + 600)     /* one full audio loop + margin */

#define CAP ((int16_t *)0x02000000)     /* captured mono samples (dumped by the runner) */
#define PANEL ((uint16_t *)0x03000000)
#define HEAP_AT 0x01f40000u             /* driver arena for the TE_NEW stand-in */

volatile uint32_t cap_n, fails, flush_cnt;
static uint32_t heap_at;
static int te_cb_calls;
static int32_t tx[128], rx[128];

/* TE stand-ins (the stubs in te_stubs.S forward here) */
void *te_new(uint32_t size)
{
    void *p = (void *)heap_at;

    heap_at += (size + 7u) & ~7u;
    return p;
}

void *te_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    while (n--)
        *d++ = *s++;
    return dst;
}

void te_flush(void *start, void *end, int inv)
{
    (void)start; (void)end; (void)inv;
    flush_cnt++;
}

static void te_cb(int32_t *t, const int32_t *r)   /* TE's own audio callback: silence out */
{
    int i;

    (void)r;
    te_cb_calls++;
    for (i = 0; i < 128; i++)
        t[i] = 0;
}

/* xorshift content per region (the runner regenerates the same in python) */
static void plant(uint8_t *p, uint32_t n, uint32_t seed)
{
    uint32_t x = seed;

    while (n--) {
        x = x * 1664525u + 1013904223u;
        *p++ = (uint8_t)(x >> 24);
    }
}

#define CHECK(bit, c) do { if (!(c)) fails |= 1u << (bit); } while (0)

static void carrier(uint8_t *b, uint32_t idx)
{
    int i;

    for (i = 0; i < 512; i++)
        b[i] = 0xa5;
    for (i = 0; i < 8; i++)
        b[i] = (uint8_t)"OP1DMPY1"[i];
    b[8] = (uint8_t)idx; b[9] = (uint8_t)(idx >> 8); b[10] = (uint8_t)(idx >> 16); b[11] = (uint8_t)(idx >> 24);
}

int main(void)
{
    int i, j;
    uint32_t peak = 0;
    uint8_t buf[4 * 512];

    heap_at = HEAP_AT;
    plant(L1I, REG_BYTES, 11);
    plant(L1A, REG_BYTES, 22);
    plant(L1B, REG_BYTES, 33);
    plant(SCR, REG_BYTES, 44);
    *TE_AUDIO_BLOCK_CB = te_cb;         /* TE audio already up when our boot hook runs */
    *TE_SHIFT_HELD = 0;

    CHECK(0, ld_ready == 0);            /* nothing at boot: capture and wrap are lazy */
    CHECK(1, *TE_AUDIO_BLOCK_CB == te_cb);

    (*TE_AUDIO_BLOCK_CB)(tx, rx);       /* off: TE's audio passes through, tx stays silent */
    CHECK(2, te_cb_calls == 1 && tx[0] == 0 && tx[127] == 0);

    *TE_SHIFT_HELD = 1;
    hook_key(TE_KEY_SPEECH, 1);         /* first toggle: capture + modem on (wraps the cb) */
    CHECK(3, ld_tx_on == 1 && *TE_AUDIO_BLOCK_CB == ld_cb);
    CHECK(4, ld_ready == 1 && ld_img_size == IMG_BYTES);
    CHECK(5, ld_img && ld_img[0] == 'O' && ld_img[1] == 'P' && ld_img[2] == '1' && ld_img[3] == 'D');
    hook_key(TE_KEY_SPEECH, 0);
    *TE_SHIFT_HELD = 0;
    hook_key(TE_KEY_METRONOME, 1);      /* not ours: falls through to TE, no toggle */
    CHECK(6, ld_tx_on == 1 && *TE_AUDIO_BLOCK_CB == ld_cb);

    cap_n = 0;
    for (i = 0; i < BLOCKS; i++) {      /* one full loop + margin; mono from the high half */
        (*TE_AUDIO_BLOCK_CB)(tx, rx);
        for (j = 0; j < 64; j++) {
            int16_t s = (int16_t)(tx[2 * j] >> 16);
            uint32_t a = s < 0 ? (uint32_t)-s : (uint32_t)s;

            CAP[cap_n++] = s;
            if (a > peak)
                peak = a;
        }
    }
    CHECK(8, peak > 2000);              /* the modem really sang */

    for (i = 0; i < 4 * 512; i++)       /* USB: 4 sectors, carriers for slices 1, 8, 0 + foreign */
        buf[i] = 0;
    carrier(buf, 1);
    carrier(buf + 2 * 512, 8);          /* last slice: partial (IMG_BYTES - 8*512 bytes) */
    carrier(buf + 3 * 512, 0);
    ld_msc_after(buf, 100, 4);
    CHECK(9, ld_usb_patched == 3 && flush_cnt == 1);
    CHECK(10, buf[0] == ld_img[512] && buf[500] == ld_img[1012]);       /* slice 1 */
    for (i = 0; i < 512; i++) {         /* foreign sector must be untouched zeros */
        if (buf[512 + i])
            fails |= 1u << 11;
    }
    for (i = 0; i < IMG_BYTES - 8 * 512; i++) {   /* slice 8: partial tail */
        if (buf[2 * 512 + i] != ld_img[8 * 512 + i])
            fails |= 1u << 12;
    }
    for (i = IMG_BYTES - 8 * 512; i < 512; i++) { /* the rest keeps its 0xA5 fill */
        if (buf[2 * 512 + i] != 0xa5)
            fails |= 1u << 12;
    }
    for (i = 0; i < 512; i++) {         /* slice 0 */
        if (buf[3 * 512 + i] != ld_img[i])
            fails |= 1u << 13;
    }

    *TE_SHIFT_HELD = 1;
    hook_key(TE_KEY_SPEECH, 1);         /* toggle back off: the callback is restored */
    CHECK(7, ld_tx_on == 0 && *TE_AUDIO_BLOCK_CB == te_cb);

    hook_frame(PANEL);                  /* the status overlay (screenshot by the runner) */
    return (int)fails;
}
