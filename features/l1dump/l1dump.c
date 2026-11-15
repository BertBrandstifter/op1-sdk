/* l1dump: read the decrypted firmware out of a running OP-1 (spec: README.md).
 *
 * The stock OP1_vdk.ldr's L1 blocks are encrypted in the update file; te-boot decrypts them
 * into L1 SRAM while booting, so the plaintext only exists inside the running chip. This
 * feature copies the four on-chip memories (L1 instruction, L1 data A/B, scratchpad) into a
 * framed, CRC-protected image in TE's heap at boot and offers two ways to get it out:
 *
 *   1. USB (always armed): an after-hook on the usb_ms_yaffs.cpp sector-read handler
 *      (0x01a7153c, "read(file, sector, nsectors) -> buf"). When the Mac reads a pre-placed
 *      carrier file (tools/l1dump_rx.py mkfile; 512-byte blocks "OP1DMPY1" + index + 0xA5
 *      fill), the hook replaces those blocks in the read buffer with the image slices. The
 *      Mac just copies the file back in disk mode. RAM only: nothing is written to NAND.
 *   2. Audio (shift + speech toggles; wraps the audio block callback only while on): the
 *      image is looped as an 8-tone FSK stream (3 bits/symbol, 1 symbol per 64-frame block,
 *      sync tone between packets). Record the line out / speaker and decode with
 *      tools/l1dump_rx.py decode.
 *
 * Read-only besides its own heap image; TE's audio is left alone while the modem is off. */
#include <op1/te246.h>
#include <op1/gfx.h>

/* ---------------------------------------------------------------- capture */

#ifdef SDK_BENCH                       /* the sim bench maps small L1 regions (run_l1dumptest.py) */
#define LD_L1I_ADDR  0x02180000        /* sim's default env has no DTEST device: SDRAM mirror */
#define LD_L1I_BYTES 0x400
#define LD_L1A_BYTES 0x400
#define LD_L1B_BYTES 0x400
#define LD_SCR_BYTES 0x400
#else
#define LD_L1I_ADDR  0xffa00000        /* L1 instruction SRAM (48 KB) */
#define LD_L1I_BYTES 0xc000
#define LD_L1A_BYTES 0x8000            /* L1 data A 0xff800000 (32 KB) */
#define LD_L1B_BYTES 0x8000            /* L1 data B 0xff900000 (32 KB) */
#define LD_SCR_BYTES 0x1000            /* scratchpad 0xffb00000 (4 KB) */
#endif

typedef struct {
    char     tag[4];                   /* "L1I0" etc, also the decoder's file name */
    uint32_t addr, len;
    int      inst;                     /* L1 instruction SRAM: no normal core loads */
} ld_region;

static const ld_region LD_REGIONS[] = {
    { "L1A0", 0xff800000, LD_L1A_BYTES, 0 },
    { "L1B0", 0xff900000, LD_L1B_BYTES, 0 },
    { "SCRP", 0xffb00000, LD_SCR_BYTES, 0 },
    { "L1I0", LD_L1I_ADDR, LD_L1I_BYTES, 1 },
};
#define LD_NREGS (sizeof(LD_REGIONS) / sizeof(LD_REGIONS[0]))

/* image: "OP1D" u32 ver=1 u32 nregs, per region {tag[4] u32 addr u32 len u32 crc32},
 * payloads in table order, u32 crc32 over everything before it (all little-endian).
 * Static buffer (the SDK pattern): no TE heap calls at boot (unproven on hardware). */
#define LD_IMG_MAX (12 + LD_NREGS * 16 + LD_L1I_BYTES + LD_L1A_BYTES + LD_L1B_BYTES + LD_SCR_BYTES + 4)
uint8_t *ld_img;
volatile uint32_t ld_img_size, ld_npkts, ld_ready, ld_progress;
static uint8_t img_buf[LD_IMG_MAX];

#define LD_PKT_PAYLOAD 120             /* bytes per packet; 332 symbols with the headers */

/* modem state (the audio callback is below; ld_boot initializes the state) */
typedef void (*ld_cb_fn)(int32_t *tx, const int32_t *rx);
static ld_cb_fn orig_cb;
volatile int ld_tx_on;
volatile uint32_t ld_sym_count;                /* symbols sent (progress) */
static uint32_t tx_pkt, tx_crc;
static int tx_pos = -64;                       /* -64..-1 leader, 0..331 packet */
static uint32_t tx_phase;

static void arm_crc(void);                     /* below (modem section) */

/* ---------------------------------------------------------------- crc */

static uint32_t crc32_tab[256];

static void crc32_init(void)
{
    uint32_t i, j, c;

    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++)
            c = (c & 1) ? (c >> 1) ^ 0xedb88320u : c >> 1;
        crc32_tab[i] = c;
    }
}

static uint32_t crc32_of(const uint8_t *p, uint32_t n)      /* zlib.crc32 compatible */
{
    uint32_t c = 0xffffffffu;

    while (n--)
        c = crc32_tab[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}

static uint16_t crc16_of(const uint8_t *p, uint32_t n)      /* CRC-16/CCITT-FALSE (binascii.crc_hqx) */
{
    uint32_t c = 0xffffu;

    while (n--) {
        int i;

        c ^= (uint32_t)*p++ << 8;
        for (i = 0; i < 8; i++)
            c = (c & 0x8000u) ? (c << 1) ^ 0x1021u : c << 1;
    }
    return (uint16_t)(c & 0xffffu);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* -------------------------------------------------------- L1 instruction SRAM read
 * The core cannot read L1 instruction SRAM with a normal load: a plain memcpy from
 * 0xffa00000 faults and hangs the device (the first hardware shift + speech did exactly that).
 * Read it 8 bytes at a time through the core's DTEST data-array interface, as the kernel's
 * arch/blackfin/mm/isram-driver.c does: write DTEST_COMMAND (read, instruction, data array),
 * CSYNC, read DTEST_DATA0/1, clear the command, CSYNC. The command and its data read must not
 * be separated, so interrupts are off across that short sequence only. */
#ifdef SDK_BENCH
/* The sim's default environment maps neither the MMU/DTEST device nor a writable L1I, so the
 * bench reads its SDRAM mirror; this path is exercised on hardware (the command encoding is
 * the kernel's, proven on BF5xx). */
#else
#define LD_DTEST_COMMAND (*(volatile uint32_t *)0xffe00300)   /* BF52x core MMRs */
#define LD_DTEST_DATA0   (*(volatile uint32_t *)0xffe00400)
#define LD_DTEST_DATA1   (*(volatile uint32_t *)0xffe00404)

static uint32_t ld_dtest_cmd(uint32_t addr)
{
    return ((addr & (1u << 11)) << (26 - 11))  /* address bit 11: way */
         | (1u << 24)                          /* instruction memory */
         | ((addr & (1u << 15)) << (23 - 15))  /* address bit 15: data bank */
         | ((addr & (3u << 12)) << (16 - 12))  /* address bits 13:12: subbank */
         | (addr & 0x47f8u)                    /* address bits 14 and 10:3 */
         | (1u << 2);                          /* data array (not the tag array) */
}
#endif

static void ld_l1i_read(uint8_t *dst, uint32_t addr, uint32_t n)
{
#ifdef SDK_BENCH
    TE_MEMCPY(dst, (const void *)addr, n);
#else
    while (n) {
        uint32_t cc, lo, hi, k;

        __builtin_bfin_csync();
        __asm__ volatile("cli %0;" : "=d"(cc) : : "memory");
        LD_DTEST_COMMAND = ld_dtest_cmd(addr);
        __builtin_bfin_csync();
        lo = LD_DTEST_DATA0;
        hi = LD_DTEST_DATA1;
        LD_DTEST_COMMAND = 0;
        __builtin_bfin_csync();
        __asm__ volatile("sti %0;" : : "d"(cc) : "memory");
        for (k = 0; k < 8 && n; k++, n--, addr++)
            *dst++ = (uint8_t)(k < 4 ? lo >> (8 * k) : hi >> (8 * (k - 4)));
    }
#endif
}

/* ---------------------------------------------------------------- boot capture */

void ld_capture(void)                   /* from ld_key (KeyHandler thread), once */
{
    uint8_t *img = img_buf, *pay;
    uint32_t total = 12 + LD_NREGS * 16 + 4;
    uint32_t i;

    crc32_init();
    for (i = 0; i < LD_NREGS; i++)
        total += LD_REGIONS[i].len;
    pay = img + 12 + LD_NREGS * 16;
    TE_MEMCPY(img, "OP1D", 4);
    put32(img + 4, 1);
    put32(img + 8, LD_NREGS);
    for (i = 0; i < LD_NREGS; i++) {
        uint8_t *row = img + 12 + i * 16;
        const ld_region *r = &LD_REGIONS[i];

        TE_MEMCPY(row, r->tag, 4);
        put32(row + 4, r->addr);
        put32(row + 8, r->len);
        if (r->inst)
            ld_l1i_read(pay, r->addr, r->len);
        else
            TE_MEMCPY(pay, (const void *)r->addr, r->len);
        put32(row + 12, crc32_of(pay, r->len));
        pay += r->len;
        ld_progress = i + 1;            /* the overlay shows where a fault would hit */
    }
    put32(pay, crc32_of(img, total - 4));

    ld_img = img;
    ld_img_size = total;
    ld_npkts = (total + LD_PKT_PAYLOAD - 1) / LD_PKT_PAYLOAD;
    tx_pkt = 0;
    tx_pos = -64;
    arm_crc();
    ld_ready = 1;                       /* last: the audio interrupt reads the fields above */
    ld_progress = LD_NREGS + 1;
}

/* ---------------------------------------------------------------- audio modem
 * One symbol per 64-frame block. 8 data tones (3 bits) at bins 2,4,...,16 of a 64-sample
 * window (bin k = k cycles per block = k * Fs/64 Hz: 1.4 .. 11 kHz at 44.1 kHz), sync tone at
 * bin 1 (never used by data, marks the packet start and sets the decoder's sample clock).
 * Packet: sync sync, 4 syms packet index (12 bits), 6 syms CRC-16 over index+payload (18
 * bits, top 2 zero), 320 syms payload (120 bytes). A 64-symbol leader (sync / tone 7
 * alternating) precedes every loop. */

#define LD_AMP 11468                           /* 0.35 full scale */
static const int16_t sine64[64] = {            /* one period, 0.5 * Q15: 16384*sin(2 pi k/64) */
         0,  1606,  3196,  4756,  6270,  7723,  9102, 10394,
     11585, 12665, 13623, 14449, 15137, 15679, 16069, 16305,
     16384, 16305, 16069, 15679, 15137, 14449, 13623, 12665,
     11585, 10394,  9102,  7723,  6270,  4756,  3196,  1606,
         0, -1606, -3196, -4756, -6270, -7723, -9102,-10394,
    -11585,-12665,-13623,-14449,-15137,-15679,-16069,-16305,
    -16384,-16305,-16069,-15679,-15137,-14449,-13623,-12665,
    -11585,-10394, -9102, -7723, -6270, -4756, -3196, -1606,
};

static void fill0(uint8_t *p, uint32_t n)
{
    while (n--)
        *p++ = 0;
}

#define SYM_SYNC 8

static void arm_crc(void)               /* crc16 over idx (2 bytes) + zero-padded payload */
{
    uint8_t tmp[2 + LD_PKT_PAYLOAD];
    uint32_t off = tx_pkt * LD_PKT_PAYLOAD, n = ld_img_size - off;

    tmp[0] = (uint8_t)(tx_pkt >> 8);
    tmp[1] = (uint8_t)tx_pkt;
    if (n > LD_PKT_PAYLOAD)
        n = LD_PKT_PAYLOAD;
    TE_MEMCPY(tmp + 2, ld_img + off, n);
    if (n < LD_PKT_PAYLOAD)
        fill0(tmp + 2 + n, LD_PKT_PAYLOAD - n);
    tx_crc = crc16_of(tmp, sizeof(tmp));
}

static int next_sym(void)               /* audio ISR: cheap shifts only */
{
    int pos = tx_pos, sym;

    if (pos < 0)                        /* leader: sync / tone 7 alternating */
        sym = (pos & 1) ? 7 : SYM_SYNC;
    else if (pos < 2)
        sym = SYM_SYNC;
    else if (pos < 6)                   /* packet index, 12 bits, MSB first */
        sym = (int)(tx_pkt >> (9 - (pos - 2) * 3)) & 7;
    else if (pos < 12)                  /* crc16 as 18 bits, MSB first (top 2 bits zero) */
        sym = (int)(tx_crc >> (15 - (pos - 6) * 3)) & 7;
    else {                              /* payload bits, 3 per symbol, MSB first */
        uint32_t bit = (uint32_t)(pos - 12) * 3, off = tx_pkt * LD_PKT_PAYLOAD + bit / 8;
        uint32_t w = 0;

        if (off < ld_img_size)
            w = (uint32_t)ld_img[off] << 8;
        if (off + 1 < ld_img_size)
            w |= ld_img[off + 1];
        sym = (int)(w >> (13 - (bit & 7))) & 7;
    }

    if (++tx_pos == 332) {              /* packet done: arm the next one's crc */
        tx_pkt = (tx_pkt + 1) % (ld_npkts ? ld_npkts : 1);
        tx_pos = tx_pkt ? 0 : -64;      /* a new loop starts with the leader */
        arm_crc();
    }
    return sym;
}

void ld_cb(int32_t *tx, const int32_t *rx)    /* non-static: the sim bench checks the wrap */
{
    int sym, bin, i;
    uint32_t inc;

    if (orig_cb)
        orig_cb(tx, rx);
    if (!ld_ready || !ld_tx_on || !ld_npkts)
        return;
    sym = next_sym();
    bin = sym == SYM_SYNC ? 1 : 2 + 2 * sym;
    inc = (uint32_t)bin << 26;          /* bin k = k cycles per 64 samples, any Fs */
    for (i = 0; i < 64; i++) {
        uint32_t u = (uint16_t)((sine64[tx_phase >> 26] * LD_AMP) >> 15);

        tx_phase += inc;
        u = (u << 16) | u;              /* both 16-bit halves: whichever the DAC slots use */
        tx[2 * i] = (int32_t)u;
        tx[2 * i + 1] = (int32_t)u;
    }
    ld_sym_count++;
}

int ld_key(int key, int down)           /* shared point on_key (filter) */
{
    if (key == TE_KEY_SPEECH && down && *TE_SHIFT_HELD) {
        /* nothing runs at boot (a boot-time fault hangs the device before the UI, with no
         * diagnosis): the capture happens lazily here, on the KeyHandler thread, and arms
         * the USB carrier patching. The capture progresses region by region on the overlay;
         * L1I goes through DTEST (ld_l1i_read), a normal load there hangs the device. */
        if (!ld_ready)
            ld_capture();
#ifdef SDK_BENCH                 /* the bench exercises the modem chain; on hardware the */
        ld_tx_on = !ld_tx_on;    /* audio wrap stays OFF until the press-hang is diagnosed */
        if (ld_tx_on) {          /* (the USB path alone delivers the dump) */
            orig_cb = *TE_AUDIO_BLOCK_CB;
            *TE_AUDIO_BLOCK_CB = ld_cb;
        } else if (orig_cb) {
            *TE_AUDIO_BLOCK_CB = orig_cb;
        }
#endif
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- USB return
 * usb_ms_yaffs.cpp sector read (0x01a7153c): read(buf, sector, nsectors) fills buf from the
 * backing file; hooked "after", so buf holds the sectors the Mac asked for. A carrier block
 * (512 bytes "OP1DMPY1" + u32 index + 0xA5 fill) is replaced with image slice #index. The
 * whole 512-byte pattern must match before anything is written, so user data is never
 * touched; DMA coherency after patching via TE_DCACHE_FLUSH. */

volatile uint32_t ld_usb_patched;

static int carrier(const uint8_t *b, uint32_t *idx)
{
    int i;

    for (i = 0; i < 8; i++)
        if (b[i] != (uint8_t)"OP1DMPY1"[i])
            return 0;
    for (i = 12; i < 512; i++)
        if (b[i] != 0xa5)
            return 0;
    *idx = (uint32_t)b[8] | ((uint32_t)b[9] << 8) | ((uint32_t)b[10] << 16) | ((uint32_t)b[11] << 24);
    return 1;
}

void ld_msc_after(uint8_t *buf, uint32_t sector, uint32_t nsectors)
{
    uint32_t n = nsectors * 512u, off, idx;
    int patched = 0;

    (void)sector;
    if (!ld_ready || nsectors > 4096)   /* sanity: 2 MB per call is far above smx's buffer */
        return;
    for (off = 0; off + 512 <= n; off += 512) {
        uint32_t slice, take;

        if (!carrier(buf + off, &idx))
            continue;
        slice = idx * 512u;
        if (slice >= ld_img_size)
            continue;
        take = ld_img_size - slice;
        if (take > 512)
            take = 512;
        TE_MEMCPY(buf + off, ld_img + slice, take);
        ld_usb_patched++;
        patched = 1;
    }
    if (patched)
        TE_DCACHE_FLUSH(buf, buf + n, 0);
}

/* ---------------------------------------------------------------- status */

void ld_frame(uint16_t *buf)            /* shared point frame (after TE's render) */
{
    int x;

    gfx_begin(buf);
    gfx_rect(0, 148, 106, 8, GFX_BLACK);
    x = gfx_text(2, 149, "L1D", GFX_WHITE, 1);
    x = gfx_dec(x + 4, 149, (int32_t)(ld_img_size / 1024), GFX_WHITE, 1);
    x = gfx_text(x, 149, "K", GFX_WHITE, 1);
    if (!ld_ready) {
        x = gfx_text(x + 4, 149, "P", GFX_RED, 1);
        gfx_dec(x + 1, 149, (int32_t)ld_progress, GFX_RED, 1);
        return;
    }
    x = gfx_text(x + 4, 149, "TX", GFX_WHITE, 1);
    x = gfx_text(x + 1, 149, ld_tx_on ? "ON" : "OFF", ld_tx_on ? GFX_GREEN : GFX_RED, 1);
    x = gfx_text(x + 4, 149, "USB", GFX_WHITE, 1);
    gfx_dec(x + 1, 149, (int32_t)ld_usb_patched, ld_usb_patched ? GFX_GREEN : GFX_WHITE, 1);
}
