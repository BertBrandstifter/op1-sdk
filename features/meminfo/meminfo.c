/* meminfo: how the OP-1's SDRAM is used at runtime. Debug, read-only (spec: README.md).
 *
 * shift + speech cycles: off -> page 1 (map) -> page 2 (CPU memory protection) -> off.
 *   - SDRAM size from the memory controller (EBIU_SDBCTL), data CPLB state (DMEM_CONTROL, the 16
 *     DCPLB entries).
 *   - TE's heap (te_new.cpp: TLSF 2.4 pool, 8.25 MB at 0x0106b7cc): used / peak from its
 *     statistics, free / largest free block by walking the blocks (GUI thread, read-only).
 *   - Write map: the first opening starts a scan in the main loop (16 KB per pass) that hashes
 *     every 64 KB chunk of SDRAM; the first pass is the baseline, later passes mark chunks whose
 *     content changed (sticky). A chunk is only read while the data CPLBs cover it (or the CPLBs
 *     are off): never an access the CPU could fault on. Nothing is ever written outside our own
 *     variables, and nothing runs before the panel was opened once (a fault = reboot, no loop). */
#include <op1/te246.h>
#include <op1/gfx.h>

#define MI_SDBCTL       ((volatile uint16_t *)0xffc00a14)       /* EBSZ bits 3:1 */
#define MI_DMEM         ((volatile uint32_t *)0xffe00004)       /* ENDCPLB bit 1 */
#define MI_DCPLB_ADDR   ((volatile uint32_t *)0xffe00100)
#define MI_DCPLB_DATA   ((volatile uint32_t *)0xffe00200)

#define HEAP            0x0106b7ccu                             /* TLSF pool (te_new.cpp) */
#define HEAP_SIZE       0x00840000u
#define HEAP_SIG        0x2a59fa59u
#define HEAP_FIRST      0x0c88u                                 /* first block header */

#define CHUNK           0x10000u                                /* 64 KB */
#define STEP            0x4000u                                 /* bytes hashed per main-loop pass */
#define MAX_CHUNKS      2048                                    /* up to 128 MB */
#define LIST            4                                       /* changed ranges listed */

enum { C_NONE, C_SAME, C_ZERO, C_CHANGED, C_UNMAPPED };

static volatile uint8_t page;                   /* 0 off, 1 map, 2 CPLBs */
static volatile uint8_t started;
static uint32_t hash[MAX_CHUNKS];
static volatile uint8_t state[MAX_CHUNKS];
static volatile uint32_t passes, scan_at;       /* main thread */
static uint32_t acc, zeros;                     /* the chunk being hashed */

/* TE's image, ours, TE's fixed buffers (from the LDR blocks) and TE's malloc heaps (VisualDSP heap
 * table 0x018e628c: heap 0 0x100 + 0xfffefc, heap 1 0x03040000 + 0xfbfdb0 = samples, updates): the
 * rest is "other" */
static const uint32_t known[][2] = {
    { 0x00000000, 0x01000000 },                 /* te-boot while booting, then malloc heap 0 */
    { 0x01000000, 0x01e00000 },                 /* TE: variables, heap, code */
    { 0x01e00000, 0x02000000 },                 /* SDK */
    { 0x02000000, 0x02800000 },                 /* screen, tape clipboard, Ptch */
    { 0x03000000, 0x03040000 },                 /* screen 2 */
    { 0x03040000, 0x04000000 },                 /* malloc heap 1: SampleBuffer.cpp, tar.cpp */
};

static uint32_t sdram_bytes(void)
{
    int sz = (*MI_SDBCTL >> 1) & 7;

    return sz > 5 ? 0 : (16u << 20) << sz;
}

static uint32_t page_bytes(uint32_t data)
{
    static const uint32_t sizes[4] = { 1u << 10, 4u << 10, 1u << 20, 4u << 20 };

    return sizes[(data >> 16) & 3];
}

/* the whole range [a, a + n) is covered by valid data CPLBs right now (or they are off) */
static int mapped(uint32_t a, uint32_t n)
{
    uint32_t end = a + n;
    int i;

    if (!(*MI_DMEM & 2))
        return 1;
    while (a < end) {
        for (i = 0; i < 16; i++) {
            uint32_t d = MI_DCPLB_DATA[i], base = MI_DCPLB_ADDR[i], sz = page_bytes(d);

            if ((d & 1) && a >= base && a - base < sz) {
                a = base + sz;
                break;
            }
        }
        if (i == 16)
            return 0;
    }
    return 1;
}

static int is_known(uint32_t a)
{
    unsigned int i;

    for (i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (a >= known[i][0] && a < known[i][1])
            return 1;
    return 0;
}

/* main_tick (main thread): hash STEP bytes, finish a chunk every CHUNK / STEP passes */
void mi_main(void)
{
    uint32_t total = sdram_bytes(), c, i, n;
    const volatile uint32_t *w;

    if (!started || !total)
        return;
    if (total > MAX_CHUNKS * CHUNK)
        total = MAX_CHUNKS * CHUNK;
    c = scan_at / CHUNK;
    if (!(scan_at % CHUNK)) {
        acc = 0x811c9dc5u;
        zeros = 0;
        if (!mapped(c * CHUNK, CHUNK)) {        /* skip the chunk this pass */
            if (state[c] == C_NONE)
                state[c] = C_UNMAPPED;
            scan_at += CHUNK;
            goto next;
        }
    }
    w = (const volatile uint32_t *)scan_at;
    for (i = 0, n = STEP / 4; i < n; i++) {
        uint32_t v = w[i];

        acc = ((acc << 5) | (acc >> 27)) ^ v;
        zeros += !v;
    }
    scan_at += STEP;
    if (!(scan_at % CHUNK)) {
        if (state[c] == C_NONE || state[c] == C_UNMAPPED) {
            hash[c] = acc;
            state[c] = zeros == CHUNK / 4 ? C_ZERO : C_SAME;
        } else if (hash[c] != acc) {
            state[c] = C_CHANGED;
        }
    }
next:
    if (scan_at >= total) {
        scan_at = 0;
        passes++;
    }
}

/* on_key filter (KeyHandler thread): shift + speech cycles the pages */
int mi_key(int key, int down)
{
    if (key != TE_KEY_SPEECH || !*TE_SHIFT_HELD)
        return 0;
    if (down) {
        page = (uint8_t)(page < 2 ? page + 1 : 0);
        started = 1;
    }
    return 1;
}

/* ------------------------------------------------------------------ GUI thread */

#define C_GREY      GFX_RGB(0x8a, 0x8a, 0x99)
#define C_DARK      GFX_RGB(0x30, 0x30, 0x38)
#define C_BLUE      GFX_RGB(0x6e, 0x8b, 0xff)
#define C_GREEN     GFX_RGB(0x3d, 0xdc, 0x97)
#define C_TEAL      GFX_RGB(0x1e, 0x6e, 0x5a)
#define C_RED       GFX_RGB(0xff, 0x4f, 0x5e)
#define C_AMBER     GFX_RGB(0xff, 0xb0, 0x3d)

static uint32_t heap_free, heap_big, heap_blocks, heap_ok;

/* walk TE's heap blocks: free total, largest free, count (bounded, every pointer checked) */
static void heap_walk(void)
{
    const uint32_t lo = HEAP + HEAP_FIRST, hi = HEAP + HEAP_SIZE;
    uint32_t b = lo, n = 0, fr = 0, big = 0;

    heap_ok = *(volatile uint32_t *)HEAP == HEAP_SIG;
    if (!heap_ok)
        return;
    while (n < 200000) {
        uint32_t size = *(volatile uint32_t *)(b + 4), s = size & ~3u;

        if (!s)
            break;                              /* the sentinel */
        n++;
        if (size & 1) {
            fr += s;
            if (s > big)
                big = s;
        }
        if (s > hi - b - 8) {                   /* torn read while TE allocates: give up */
            heap_ok = 2;
            break;
        }
        b += 8 + s;
    }
    heap_free = fr;
    heap_big = big;
    heap_blocks = n;
}

/* "1.23" (MB, two decimals) */
static int mb(int x, int y, uint32_t bytes, uint16_t c)
{
    uint32_t h = ((bytes >> 10) * 100 + 512) >> 10;

    x = gfx_dec(x, y, (int32_t)(h / 100), c, 1);
    x = gfx_text(x, y, ".", c, 1);
    x = gfx_text(x, y, h % 100 < 10 ? "0" : "", c, 1);
    return gfx_dec(x, y, (int32_t)(h % 100), c, 1);
}

static uint16_t cell_colour(uint32_t c)
{
    int k = is_known(c * CHUNK);

    switch (state[c]) {
    case C_SAME: return k ? C_BLUE : C_GREEN;
    case C_ZERO: return k ? C_BLUE : C_TEAL;
    case C_CHANGED: return k ? C_AMBER : C_RED;
    case C_UNMAPPED: return C_GREY;
    default: return C_DARK;
    }
}

static int legend(int x, int y, uint16_t c, const char *s)
{
    gfx_rect(x, y, 4, 5, c);
    return gfx_text(x + 6, y, s, GFX_WHITE, 1) + 8;
}

static void draw_map(void)
{
    uint32_t total = sdram_bytes(), chunks, c, rows, used;
    int x, y, ch, list = 0, run = -1;
    static uint8_t tick;

    if (!(tick++ & 31))
        heap_walk();
    if (total > MAX_CHUNKS * CHUNK)
        total = MAX_CHUNKS * CHUNK;
    chunks = total / CHUNK;
    rows = chunks / 64;
    ch = rows > 16 ? 2 : 4;

    x = gfx_text(2, 2, "SDRAM ", GFX_WHITE, 1);
    x = gfx_dec(x, 2, (int32_t)(total >> 20), C_GREEN, 1);
    x = gfx_text(x, 2, " MB  SDBCTL ", GFX_WHITE, 1);
    x = gfx_hex(x, 2, *MI_SDBCTL, 4, GFX_WHITE, 1);
    x = gfx_text(x, 2, (*MI_DMEM & 2) ? "  CPLB ON" : "  CPLB OFF", GFX_WHITE, 1);
    x = gfx_text(x, 2, "  PASS ", GFX_WHITE, 1);
    x = gfx_dec(x, 2, (int32_t)passes, C_GREEN, 1);
    x = gfx_text(x, 2, "  AT ", GFX_WHITE, 1);
    gfx_hex(x, 2, scan_at, 8, GFX_WHITE, 1);

    x = gfx_text(2, 10, "HEAP USED ", GFX_WHITE, 1);
    used = *(volatile uint32_t *)(HEAP + 4);
    x = mb(x, 10, used, C_AMBER);
    x = gfx_text(x, 10, " PEAK ", GFX_WHITE, 1);
    x = mb(x, 10, *(volatile uint32_t *)(HEAP + 8), C_AMBER);
    x = gfx_text(x, 10, " FREE ", GFX_WHITE, 1);
    x = mb(x, 10, heap_free, C_GREEN);
    x = gfx_text(x, 10, " BIG ", GFX_WHITE, 1);
    x = mb(x, 10, heap_big, C_GREEN);
    x = gfx_text(x, 10, " N ", GFX_WHITE, 1);
    x = gfx_dec(x, 10, (int32_t)heap_blocks, GFX_WHITE, 1);
    if (heap_ok != 1)
        gfx_text(x + 4, 10, heap_ok ? "TORN" : "NO SIG", C_RED, 1);

    /* the map: a row = 64 chunks = 4 MB, labelled in MB */
    for (c = 0; c < chunks; c++) {
        uint32_t r = c / 64;

        if (!(c % 64)) {
            y = 18 + (int)r * (ch + 1);
            if (!(r & 1))                       /* every 8 MB */
                gfx_dec(2, y, (int32_t)(r * 4), C_GREY, 1);
        }
        gfx_rect(16 + (int)(c % 64) * 4, 18 + (int)r * (ch + 1), 3, ch, cell_colour(c));
    }
    y = 18 + (int)rows * (ch + 1) + 2;
    x = legend(2, y, C_BLUE, "TE/SDK");
    x = legend(x, y, C_AMBER, "TE/SDK CHANGED");
    x = legend(x, y, C_GREEN, "SAME");
    x = legend(x, y, C_TEAL, "ZERO");
    x = legend(x, y, C_RED, "CHANGED");
    x = legend(x, y, C_GREY, "UNMAPPED");
    legend(x, y, C_DARK, "NOT YET");

    /* changed ranges outside TE's image and ours, one per line */
    y += 8;
    gfx_text(2, y, "CHANGED OUTSIDE TE/SDK:", C_RED, 1);
    for (c = 0; c <= chunks; c++) {
        int out = c < chunks && state[c] == C_CHANGED && !is_known(c * CHUNK);

        if (out && run < 0)
            run = (int)c;
        if (!out && run >= 0) {
            if (list < LIST) {
                x = gfx_hex(100, y + list * 7, (uint32_t)run * CHUNK, 8, GFX_WHITE, 1);
                x = gfx_text(x, y + list * 7, "-", GFX_WHITE, 1);
                gfx_hex(x, y + list * 7, c * CHUNK - 1, 8, GFX_WHITE, 1);
            }
            list++;
            run = -1;
        }
    }
    if (!list)
        gfx_text(100, y, "NONE", C_GREEN, 1);
    else if (list > LIST)
        gfx_dec(gfx_text(180, y, "+", GFX_WHITE, 1), y, list - LIST, GFX_WHITE, 1);

}

static void draw_cplb(void)
{
    static const char *const sizes[4] = { "1K", "4K", "1M", "4M" };
    int i, x, y = 14;

    x = gfx_text(2, 2, "DMEM CONTROL ", GFX_WHITE, 1);
    x = gfx_hex(x, 2, *MI_DMEM, 8, C_GREEN, 1);
    gfx_text(x + 8, 2, "DCPLB: ADDR SIZE DATA", GFX_WHITE, 1);
    for (i = 0; i < 16; i++, y += 8) {
        uint32_t d = MI_DCPLB_DATA[i];
        uint16_t c = (d & 1) ? GFX_WHITE : C_GREY;

        x = gfx_dec(2, y, i, C_GREY, 1);
        x = gfx_hex(16, y, MI_DCPLB_ADDR[i], 8, c, 1);
        x = gfx_text(x + 6, y, sizes[(d >> 16) & 3], c, 1);
        gfx_hex(x + 6, y, d, 8, c, 1);
    }
}

void mi_frame(uint16_t *buf)
{
    if (!page)
        return;
    gfx_begin(buf);
    gfx_rect(0, 0, gfx_w, gfx_h, GFX_BLACK);
    if (page == 1)
        draw_map();
    else
        draw_cplb();
}
