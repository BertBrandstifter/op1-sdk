/* playprobe: device answers the PLAY tape screen needs before it is built (README.md,
 * new_pattern_tape.md section 8). Debug; reads only, except the test tone it adds to the output.
 *
 * shift + speech: panel on / off. The first opening wraps TE's audio block callback (like
 * cpuinfo); nothing runs before that.
 *   MIX POINT  blue press: a triangle tone added to TE's output words after TE's render
 *              (off / both / word 0 / word 1), blue turn: its level; TX PEAK / LOW16 = what TE
 *              wrote (format check). Audible = PLAY can mix there.
 *   LOAD       TE's callback, our share (tone + mix test), SDK engines (enginehost), per 0.25 s.
 *   MIX COST   orange turn: 0..8 stereo streams mixed from the sample heap every block (gain,
 *              sum, soft clip; the result is discarded, nothing is heard): the cost of PLAY's mix.
 *   DISK       green press: mount disk mode's FAT volume inside user/tape_c.raw (lib fat, read
 *              only), list play/ with each file's WAV / AIFF header.
 *   READ       white press: read speed test from the main loop (white turn: 4..128 KB per pass)
 *              on A1 (else the first good file, else the raw image): KB/s, average / worst read.
 *   HEAP       orange press: TE's sample heap (heap 1): its free list (read only) and the largest
 *              blocks heap_malloc really gives (allocated and freed at once, main thread).
 *   KEYS       the last key events with shift and edit mode (shift + tape, T1-T4 on the tape
 *              screen).
 * File reads, the heap test and allocation run in the main loop (main_tick), never in the audio
 * interrupt or the GUI thread. */
#include <op1/te246.h>
#include <op1/gfx.h>
#include <op1/fat.h>
#include <op1/audiofile.h>

#define PP_IMAGE        "/yaffs2/user/tape_c.raw"
/* yaffs_pread(fd, buf, n, offset): yaffsfs_do_read(fd, buf, n, 1, offset) (0x01a999e8) */
#define PP_PREAD        ((int (*)(int fd, void *buf, uint32_t n, int32_t off))0x01a99ca8)
/* VisualDSP heap descriptor by index (malloc 0x01aa0318 and free 0x01a9f0f8 use it): free list
 * head at +0x18, nodes { size, heap index, next } */
#define PP_HEAP_DESC    ((const volatile uint8_t *(*)(int heap))0x01a9f2e2)
#define PP_STAT         ((int (*)(const char *path, void *st))0x01a9a69c)   /* yaffs_stat, size at +0x1c */
#define PP_HEAP_LO      0x03040000u                     /* heap 1: 0x03040000 + 15.75 MB */
#define PP_HEAP_HI      0x04000000u
#define PP_MHZ          400u                            /* CCLK, device-measured (cpuinfo) */
#define WIN             172                             /* blocks per window: ~0.25 s */
#define SEC             (PP_MHZ * 1000000u)             /* cycles per second */

typedef void (*cb_fn)(int32_t *tx, const int32_t *rx);

extern volatile uint8_t eh_prof;                        /* enginehost */
extern volatile uint32_t eh_cycles;

static volatile uint8_t open_, started;
static cb_fn orig;
volatile uint32_t pp_rewraps;

static inline uint32_t cycles(void)
{
    uint32_t c;

    __asm__ volatile("%0 = CYCLES;" : "=d"(c));
    return c;
}

/* ------------------------------------------------------------------ audio interrupt */

volatile uint8_t pp_tone_mode;                      /* 0 off, 1 both words, 2 word 0, 3 word 1 */
static volatile uint8_t tone_db = 2;
static const int8_t tone_dbs[] = { -60, -48, -36, -24, -18, -12 };
static const uint16_t tone_amps[] = { 33, 131, 519, 2063, 4125, 8231 };    /* Q15 */
static uint32_t phase;

#define MIX_FRAMES      16384                           /* per stream: 64 KB of stereo 16-bit */
#define MIX_BYTES       (8u * MIX_FRAMES * 4u)          /* 512 KB */
volatile uint8_t pp_mix_n;                          /* streams mixed per block, 0..8 */
int16_t *volatile pp_mix_buf;                       /* sample heap (main thread allocates) */
static volatile uint8_t mix_err;
static uint32_t mix_pos;
static int32_t mix_out[128];                            /* the mix: kept, never output */

volatile uint8_t pp_ccen_was;
static uint32_t last_t0, acc_te, acc_pp, acc_p, acc_e, max_tot, nblk, last_e, tx_peak, tx_low;
static uint8_t have_last;
volatile uint32_t pp_te, pp_pp, pp_p, pp_e, pp_peak, pp_windows, pp_tx_peak, pp_tx_low;
volatile uint32_t pp_tx_at;                             /* TE's output words (L1 = no cache) */

static int32_t sat_add(int32_t a, int32_t b)
{
    int32_t r = (int32_t)((uint32_t)a + (uint32_t)b);

    if (((a ^ r) & (b ^ r)) < 0)
        r = a < 0 ? (int32_t)0x80000000 : 0x7fffffff;
    return r;
}

/* ~441 Hz triangle into TE's 32-bit output words (16-bit sample in the top half) */
static void tone(int32_t *tx, int mode)
{
    int32_t amp = tone_amps[tone_db];
    int f;

    for (f = 0; f < TE_AUDIO_FRAMES; f++) {
        uint32_t p = phase >> 16;
        int32_t t = (int32_t)(p < 32768 ? p : 65535 - p) * 2 - 32767;
        int32_t s = (int32_t)((uint32_t)((t * amp) >> 15) << 16);

        if (mode != 3)
            tx[2 * f] = sat_add(tx[2 * f], s);
        if (mode != 2)
            tx[2 * f + 1] = sat_add(tx[2 * f + 1], s);
        phase += 42949673u;                             /* 441 Hz at 44.1 kHz */
    }
}

/* n stereo streams, 64 frames each: gain, sum, soft clip, as PLAY will (result discarded) */
static void mix(const int16_t *buf, int n)
{
    int k, i;

    for (i = 0; i < 128; i++)
        mix_out[i] = 0;
    for (k = 0; k < n; k++) {
        const int16_t *s = buf + ((uint32_t)k * MIX_FRAMES + mix_pos) * 2;
        int32_t g = 23170 - k * 1500;                   /* -3 dB and down */

        for (i = 0; i < 128; i++)
            mix_out[i] += (s[i] * g) >> 15;
    }
    for (i = 0; i < 128; i++) {
        int32_t x = mix_out[i], a = x < 0 ? -x : x;

        if (a > 24576)                                  /* soft knee above -2.5 dB, then clamp */
            a = 24576 + ((a - 24576) >> 2);
        if (a > 32767)
            a = 32767;
        mix_out[i] = (x < 0 ? -a : a) << 16;
    }
    mix_pos = (mix_pos + TE_AUDIO_FRAMES) % MIX_FRAMES;
}

/* TE's audio block callback, wrapped (IVG11, supervisor) */
static void pp_cb(int32_t *tx, const int32_t *rx)
{
    uint32_t t0, t1, t2, e, tot;
    const int16_t *mb;
    int i, mode, n;

    if (!pp_ccen_was) {
        uint32_t s;

        __asm__ volatile("%0 = SYSCFG;" : "=d"(s));
        if (s & 2) {
            pp_ccen_was = 1;
        } else {
            s |= 2;
            __asm__ volatile("SYSCFG = %0; CSYNC;" : : "d"(s));
            pp_ccen_was = 2;
        }
    }
    t0 = cycles();
    orig(tx, rx);
    t1 = cycles();
    for (i = 0; i < 2 * TE_AUDIO_FRAMES; i++) {
        int32_t v = tx[i];
        uint32_t a = v < 0 ? (uint32_t)(-(v + 1)) : (uint32_t)v;

        if (a > tx_peak)
            tx_peak = a;
        tx_low |= (uint32_t)v & 0xffffu;
    }
    pp_tx_at = (uint32_t)tx;
    mode = pp_tone_mode;
    if (mode) {
        tone(tx, mode);
        if ((uint32_t)tx < 0xff800000u)                 /* cached SDRAM: DMA must see our adds */
            TE_DCACHE_FLUSH(tx, tx + 2 * TE_AUDIO_FRAMES, 0);
    }
    mb = pp_mix_buf;
    n = pp_mix_n;
    if (n && mb)
        mix(mb, n);
    t2 = cycles();
    e = eh_cycles;
    if (have_last) {
        acc_te += t1 - t0;
        acc_pp += t2 - t1;
        acc_p += t0 - last_t0;
        acc_e += e - last_e;
        tot = t2 - t0;
        if (tot > max_tot)
            max_tot = tot;
        if (++nblk == WIN) {
            pp_te = acc_te / WIN;
            pp_pp = acc_pp / WIN;
            pp_p = acc_p / WIN;
            pp_e = acc_e / WIN;
            pp_peak = max_tot;
            pp_tx_peak = tx_peak;
            pp_tx_low = tx_low;
            pp_windows++;
            acc_te = acc_pp = acc_p = acc_e = max_tot = nblk = tx_peak = tx_low = 0;
        }
    }
    have_last = 1;
    last_t0 = t0;
    last_e = e;
}

/* ------------------------------------------------------------------ main thread */

static volatile uint8_t req_scan, req_heap, req_speed;

static int fd = -1;
static fat_vol vol;

static int img_read(void *ctx, uint32_t off, void *buf, uint32_t n)
{
    (void)ctx;
    return PP_PREAD(fd, buf, n, (int32_t)off);
}

static int img_open(void)
{
    if (fd < 0)
        fd = TE_FILE_OPEN(PP_IMAGE, TE_FILE_RDONLY, TE_FILE_MODE);
    return fd >= 0;
}

static void img_close(void)
{
    if (fd >= 0)
        TE_FILE_CLOSE(fd);
    fd = -1;
}

/* aud_parse's reader for a file inside the FAT volume */
static int fat_file_read(void *ctx, uint32_t off, void *buf, uint32_t n)
{
    return fat_read(&vol, (fat_file *)ctx, off, buf, n);
}

/* disk scan */
#define MAXF 16
typedef struct ppf { fat_ent e; aud_info a; int8_t chain; } ppf;
ppf pp_files[MAXF];
volatile uint8_t pp_nfiles, pp_scan_state;             /* 0 never, 1 running, 2 done */
volatile int8_t pp_scan_err;                        /* FAT_* of mount / find; 1 = no image */
static volatile uint8_t mounted;
static volatile uint32_t scan_us, play_total, root_total;
int pp_a1_index = -1;

static void collect(void *ctx, const fat_ent *e)
{
    (void)ctx;
    play_total++;
    if ((e->attr & 0x06) || pp_nfiles >= MAXF)             /* hidden / system: macOS "._" files */
        return;
    pp_files[pp_nfiles].e = *e;
    pp_nfiles++;
}

/* raw root slots (diagnosis when there is no play/) */
#define MAXRAW 6
static uint8_t raw_ent[MAXRAW][32];
static volatile uint8_t nraw;
static volatile uint32_t raw_total, raw_del, raw_lfn, raw_lbl;
static volatile int32_t img_size = -1;

static void raw_root(void *ctx, const uint8_t *d)
{
    int i;

    (void)ctx;
    raw_total++;
    if (d[0] == 0xe5)
        raw_del++;
    if (d[11] == 0x0f) {
        raw_lfn++;
        return;
    }
    if (d[11] & 0x08)
        raw_lbl++;
    if (nraw < MAXRAW) {
        for (i = 0; i < 32; i++)
            raw_ent[nraw][i] = d[i];
        nraw++;
    }
}

static void count_root(void *ctx, const fat_ent *e)
{
    (void)ctx;
    (void)e;
    root_total++;
}

static void speed_stop(void);

static void scan(void)
{
    uint32_t t0 = cycles();
    fat_ent play;
    int i, r;

    speed_stop();                                       /* it reads through the old mount */
    pp_scan_state = 1;
    pp_nfiles = 0;
    play_total = root_total = 0;
    pp_a1_index = -1;
    pp_scan_err = 0;
    mounted = 0;
    nraw = 0;
    raw_total = raw_del = raw_lfn = raw_lbl = 0;
    {
        static uint32_t st[32];                         /* TE's stat buffer is 0x48 bytes */

        img_size = PP_STAT(PP_IMAGE, st) < 0 ? -1 : (int32_t)st[0x1c / 4];
    }
    if (!img_open()) {
        pp_scan_err = 1;
        goto done;
    }
    r = fat_mount(&vol, img_read, 0);
    if (r) {
        pp_scan_err = (int8_t)r;
        goto done;
    }
    mounted = 1;
    fat_list(&vol, 0, count_root, 0);
    fat_dir_raw(&vol, 0, raw_root, 0);
    r = fat_find(&vol, 0, "PLAY", &play);
    if (!r && !(play.attr & 0x10))
        r = FAT_ENOENT;
    if (r) {
        pp_scan_err = (int8_t)r;
        goto done;
    }
    r = fat_list(&vol, play.clus, collect, 0);
    if (r < 0) {
        pp_scan_err = (int8_t)r;
        goto done;
    }
    for (i = 0; i < pp_nfiles; i++) {
        ppf *f = &pp_files[i];
        fat_file pf;

        if (f->e.attr & 0x10)
            continue;
        f->chain = (int8_t)fat_check(&vol, &f->e);
        fat_open(&pf, &f->e);
        aud_parse(fat_file_read, &pf, f->e.size, &f->a);
        if (f->a.err == AUD_OK && !f->chain
            && (pp_a1_index < 0 || (f->e.name[0] == 'A' && f->e.name[1] == '1' && f->e.name[2] == '.')))
            pp_a1_index = i;
    }
done:
    img_close();
    scan_us = (cycles() - t0) / PP_MHZ;
    pp_scan_state = 2;
}

/* read speed */
static const uint32_t chunks[] = { 4096, 16384, 65536, 131072 };
static volatile uint8_t chunk_i = 2;
volatile uint8_t pp_speed_on;
static uint8_t *speed_buf;
static int speed_file;                                  /* index into pp_files[], -1 = raw image */
static fat_file speed_f;
static uint32_t speed_off, sp_win_c, sp_win_bytes, sp_win_reads, sp_win_cyc, sp_win_worst;
volatile uint32_t pp_sp_kbs, pp_sp_avg_us, pp_sp_worst_us, pp_sp_all_worst_us, pp_sp_reads, pp_sp_errs, pp_sp_windows;

static void speed_stop(void)
{
    if (!pp_speed_on)
        return;
    pp_speed_on = 0;
    img_close();
    TE_FREE(speed_buf);
    speed_buf = 0;
}

static void speed_start(void)
{
    speed_buf = TE_HEAP_MALLOC(TE_HEAP_SAMPLES, chunks[3]);
    if (!speed_buf || !img_open()) {
        TE_FREE(speed_buf);
        speed_buf = 0;
        pp_sp_errs++;
        return;
    }
    speed_file = pp_scan_state == 2 && !pp_scan_err ? pp_a1_index : -1;
    if (speed_file >= 0) {
        fat_open(&speed_f, &pp_files[speed_file].e);
        speed_off = pp_files[speed_file].a.data_off;
    } else {
        speed_off = 0;
    }
    sp_win_c = sp_win_bytes = sp_win_reads = sp_win_cyc = sp_win_worst = 0;
    pp_sp_kbs = pp_sp_avg_us = pp_sp_worst_us = pp_sp_all_worst_us = pp_sp_reads = pp_sp_errs = pp_sp_windows = 0;
    pp_speed_on = 1;
}

static void speed_step(uint32_t since_last)
{
    uint32_t n = chunks[chunk_i], t0, c;
    int r;

    sp_win_c += since_last;
    t0 = cycles();
    if (speed_file >= 0) {
        const aud_info *a = &pp_files[speed_file].a;
        uint32_t end = a->data_off + a->data_bytes;

        if (n > end - speed_off)
            n = end - speed_off;
        r = fat_read(&vol, &speed_f, speed_off, speed_buf, n);
        speed_off += n;
        if (speed_off >= end)
            speed_off = a->data_off;                    /* loop the file */
    } else {
        r = PP_PREAD(fd, speed_buf, n, (int32_t)speed_off);
        speed_off = speed_off + n >= (64u << 20) ? 0 : speed_off + n;
    }
    c = cycles() - t0;
    if (r != (int)n) {
        pp_sp_errs++;
    } else {
        sp_win_bytes += n;
        pp_sp_reads++;
    }
    sp_win_reads++;
    sp_win_cyc += c;
    if (c > sp_win_worst)
        sp_win_worst = c;
    if (sp_win_c >= SEC) {                              /* publish once a second */
        uint32_t ms = sp_win_c / (PP_MHZ * 1000u);

        pp_sp_kbs = ms ? sp_win_bytes / ms : 0;            /* bytes per ms = KB/s (1000) */
        pp_sp_avg_us = sp_win_reads ? sp_win_cyc / sp_win_reads / PP_MHZ : 0;
        pp_sp_worst_us = sp_win_worst / PP_MHZ;
        if (pp_sp_worst_us > pp_sp_all_worst_us)
            pp_sp_all_worst_us = pp_sp_worst_us;
        pp_sp_windows++;
        sp_win_c = sp_win_bytes = sp_win_reads = sp_win_cyc = sp_win_worst = 0;
    }
}

/* sample heap */
volatile uint32_t pp_hp_list_sum, pp_hp_list_big, pp_hp_list_n, pp_hp_big, pp_hp_total, pp_hp_blocks, pp_hp_us;
volatile uint8_t pp_hp_state, pp_hp_list_ok;                  /* list: 1 ok, 2 torn / out of range */

static void heap_test(void)
{
    const volatile uint8_t *d = PP_HEAP_DESC(TE_HEAP_SAMPLES);
    uint32_t node, sum = 0, big = 0, n = 0, total = 0, first = 0, t0;
    void *held[8];
    int k = 0;

    pp_hp_list_ok = 1;
    node = d ? *(const volatile uint32_t *)(d + 0x18) : 0;
    while (node && n < 4096) {
        uint32_t size, heap;

        if (node < PP_HEAP_LO || node >= PP_HEAP_HI - 12 || (node & 3)) {
            pp_hp_list_ok = 2;
            break;
        }
        size = *(const volatile uint32_t *)node;
        heap = *(const volatile uint32_t *)(node + 4);
        if (heap == TE_HEAP_SAMPLES) {
            sum += size;
            if (size > big)
                big = size;
        }
        n++;
        node = *(const volatile uint32_t *)(node + 8);
    }
    pp_hp_list_sum = sum;
    pp_hp_list_big = big;
    pp_hp_list_n = n;

    /* the largest block heap_malloc gives, then the next largest, ... (held, then all freed) */
    t0 = cycles();
    while (k < 8) {
        uint32_t lo = 0, hi = 16u << 20;
        void *p;

        while (hi - lo > 16384) {
            uint32_t mid = ((lo + hi) / 2) & ~15u;

            p = TE_HEAP_MALLOC(TE_HEAP_SAMPLES, mid);
            if (p) {
                TE_FREE(p);
                lo = mid;
            } else {
                hi = mid;
            }
        }
        if (lo < 65536 || !(p = TE_HEAP_MALLOC(TE_HEAP_SAMPLES, lo)))
            break;
        held[k++] = p;
        total += lo;
        if (!first)
            first = lo;
    }
    pp_hp_blocks = (uint32_t)k;
    while (k)
        TE_FREE(held[--k]);
    pp_hp_us = (cycles() - t0) / PP_MHZ;
    pp_hp_big = first;
    pp_hp_total = total;
    pp_hp_state = 1;
}

volatile uint32_t pp_loops;                             /* main-loop passes per second */

/* main_tick (main thread) */
void pp_main(void)
{
    static uint32_t last_c, win_c, win_passes;
    uint32_t now, since = 0;
    cb_fn cur;

    if (!started)
        return;
    now = cycles();
    if (last_c) {
        since = now - last_c;
        win_c += since;
        win_passes++;
        if (win_c >= SEC) {
            pp_loops = win_passes;
            win_c = win_passes = 0;
        }
    }
    last_c = now;

    cur = *TE_AUDIO_BLOCK_CB;                           /* wrap (again, if TE re-set it) */
    if (cur && cur != pp_cb) {
        if (orig)
            pp_rewraps++;
        orig = cur;                                     /* before the pointer: the interrupt reads it next */
        __asm__ volatile("" ::: "memory");
        eh_prof = 1;
        *TE_AUDIO_BLOCK_CB = pp_cb;
    }

    if (pp_mix_n && !pp_mix_buf && !mix_err) {
        int16_t *b = TE_HEAP_MALLOC(TE_HEAP_SAMPLES, MIX_BYTES);

        if (b)
            TE_MEMSET(b, 0x11, MIX_BYTES);
        else
            mix_err = 1;
        pp_mix_buf = b;
    } else if (!pp_mix_n && pp_mix_buf) {
        int16_t *b = pp_mix_buf;

        pp_mix_buf = 0;                                    /* the interrupt sees 0 from now on */
        TE_FREE(b);
        mix_err = 0;
    }
    if (req_scan) {
        req_scan = 0;
        scan();
    }
    if (req_heap) {
        req_heap = 0;
        heap_test();
    }
    if (req_speed) {
        req_speed = 0;
        if (pp_speed_on)
            speed_stop();
        else
            speed_start();
    }
    if (pp_speed_on)
        speed_step(since);
}

/* ------------------------------------------------------------------ keys + knobs (KeyHandler) */

#define NLOG 8
static volatile uint8_t klog_key[NLOG], klog_bits[NLOG], klog_mode[NLOG], klog_at;
static volatile uint32_t klog_n;
static volatile uint8_t file_top;

int pp_key(int key, int down)
{
    int i = klog_at;

    klog_key[i] = (uint8_t)key;
    klog_bits[i] = (uint8_t)((down ? 1 : 0) | (*TE_SHIFT_HELD ? 2 : 0));
    klog_mode[i] = (uint8_t)*TE_EDIT_MODE;
    klog_at = (uint8_t)((i + 1) % NLOG);
    klog_n++;

    if (key == TE_KEY_SPEECH && *TE_SHIFT_HELD) {
        if (down) {
            open_ = (uint8_t)!open_;
            started = 1;
        }
        return 1;
    }
    if (!open_ || key < TE_KEY_BLUE_PUSH || key > TE_KEY_ORANGE_PUSH)
        return 0;
    if (down) {
        switch (key) {
        case TE_KEY_BLUE_PUSH: pp_tone_mode = (uint8_t)((pp_tone_mode + 1) & 3); break;
        case TE_KEY_GREEN_PUSH: req_scan = 1; break;
        case TE_KEY_WHITE_PUSH: req_speed = 1; break;
        default: req_heap = 1; break;
        }
    }
    return 1;
}

static int step(int v, int d, int lo, int hi)
{
    v += d;
    return v < lo ? lo : v > hi ? hi : v;
}

int pp_knob(int knob, int16_t *diff)
{
    int d;

    if (!open_)
        return 0;
    d = *diff > 0 ? 1 : *diff < 0 ? -1 : 0;
    switch (knob) {
    case 0: tone_db = (uint8_t)step(tone_db, d, 0, 5); break;
    case 1: file_top = (uint8_t)step(file_top, d, 0, pp_nfiles > 6 ? pp_nfiles - 6 : 0); break;
    case 2: chunk_i = (uint8_t)step(chunk_i, d, 0, 3); break;
    default: pp_mix_n = (uint8_t)step(pp_mix_n, d, 0, 8); break;
    }
    return 1;
}

/* ------------------------------------------------------------------ GUI thread */

#define C_GREY      GFX_RGB(0x8a, 0x8a, 0x99)
#define C_GREEN     GFX_RGB(0x3d, 0xdc, 0x97)
#define C_RED       GFX_RGB(0xff, 0x4f, 0x5e)
#define C_AMBER     GFX_RGB(0xff, 0xb0, 0x3d)
#define C_BLUE      GFX_RGB(0x6e, 0x8b, 0xff)
#define C_WHITE     GFX_WHITE

static uint32_t permille(uint32_t w, uint32_t p)
{
    if (!p)
        return 0;
    while (w > 4000000u) {
        w >>= 1;
        p >>= 1;
    }
    return p ? w * 1000u / p : 0;
}

static int tenths(int x, int y, uint32_t v, uint16_t c)
{
    x = gfx_dec(x, y, (int32_t)(v / 10), c, 1);
    x = gfx_text(x, y, ".", c, 1);
    return gfx_dec(x, y, (int32_t)(v % 10), c, 1);
}

static int pct(int x, int y, uint32_t pm)
{
    return tenths(x, y, pm, pm >= 900 ? C_RED : pm >= 700 ? C_AMBER : C_GREEN);
}

static int mb(int x, int y, uint32_t bytes, uint16_t c)  /* "1.23" MB */
{
    uint32_t h = ((bytes >> 10) * 100 + 512) >> 10;

    x = gfx_dec(x, y, (int32_t)(h / 100), c, 1);
    x = gfx_text(x, y, h % 100 < 10 ? ".0" : ".", c, 1);
    return gfx_dec(x, y, (int32_t)(h % 100), c, 1);
}

static int ms(int x, int y, uint32_t us, uint16_t c)     /* "12.3" ms */
{
    return tenths(x, y, (us + 50) / 100, c);
}

static int label(int x, int y, const char *s)
{
    return gfx_text(x, y, s, C_GREY, 1);
}

static const char *const fat_err[] = {
    "OK", "READ ERROR", "NO FAT VOLUME", "FAT12", "BAD GEOMETRY", "NO PLAY FOLDER", "BROKEN CHAIN",
};

static void draw_files(int y)
{
    int i, x, top = file_top;

    for (i = top; i < pp_nfiles && i < top + 6; i++, y += 8) {
        const ppf *f = &pp_files[i];
        const aud_info *a = &f->a;
        uint16_t c = a->err == AUD_OK && !f->chain ? C_GREEN : C_RED;

        gfx_text(2, y, f->e.name, C_WHITE, 1);
        if (f->e.attr & 0x10) {
            label(60, y, "FOLDER");
            continue;
        }
        x = gfx_text(60, y, a->kind == AUD_WAV ? "WAV" : a->kind == AUD_AIFF ? (a->big_endian ? "AIF" : "AIF LE") : "---",
                     a->kind == AUD_WAV ? C_AMBER : C_GREEN, 1);
        if (a->kind) {
            x = gfx_dec(92, y, a->channels, C_WHITE, 1);
            x = label(x, y, "CH ");
            x = gfx_dec(x, y, (int32_t)a->rate, C_WHITE, 1);
            x = label(x, y, " ");
            x = gfx_dec(x, y, a->bits, C_WHITE, 1);
            x = label(x, y, "B ");
            x = tenths(x, y, a->frames / 4410u, C_WHITE);
            x = label(x, y, " S");
        }
        x = mb(196, y, f->e.size, C_GREY);
        x = label(x, y, " MB ");
        gfx_text(x, y, f->chain ? "BROKEN CHAIN" : aud_err_text(a->err), c, 1);
    }
}

/* image size, sector / cluster size, where FAT, root and data start, data clusters */
static void draw_geometry(int y)
{
    int x = label(2, y, "IMG ");

    x = img_size < 0 ? gfx_text(x, y, "NO STAT", C_RED, 1) : mb(x, y, (uint32_t)img_size, C_WHITE);
    x = label(x, y, " BPS ");
    x = gfx_dec(x, y, vol.bps, C_WHITE, 1);
    x = label(x, y, " SPC ");
    x = gfx_dec(x, y, vol.spc, C_WHITE, 1);
    x = label(x, y, " FAT ");
    x = gfx_hex(x, y, vol.fat_off, 7, C_WHITE, 1);
    x = label(x, y, " ROOT ");
    x = gfx_hex(x, y, vol.root_off, 7, C_WHITE, 1);
    x = label(x, y, " DATA ");
    x = gfx_hex(x, y, vol.data_off, 7, C_WHITE, 1);
    x = label(x, y, " N ");
    gfx_dec(x, y, (int32_t)vol.nclus, C_WHITE, 1);
}

/* the root's raw slots: counts, then the first short entries as stored (deleted ones in red) */
static void draw_raw(int y)
{
    int i, k, x = label(2, y, "ROOT SLOTS ");

    x = gfx_dec(x, y, (int32_t)raw_total, C_WHITE, 1);
    x = label(x, y, "  DELETED ");
    x = gfx_dec(x, y, (int32_t)raw_del, C_WHITE, 1);
    x = label(x, y, "  LONG NAME ");
    x = gfx_dec(x, y, (int32_t)raw_lfn, C_WHITE, 1);
    x = label(x, y, "  LABEL ");
    gfx_dec(x, y, (int32_t)raw_lbl, C_WHITE, 1);
    for (i = 0; i < nraw; i++) {
        const uint8_t *d = raw_ent[i];
        char n[12];
        uint16_t c = d[0] == 0xe5 ? C_RED : C_WHITE;

        y += 8;
        for (k = 0; k < 11; k++)
            n[k] = (char)(d[k] >= 0x20 && d[k] < 0x7f ? d[k] : '?');
        n[11] = 0;
        gfx_text(2, y, n, c, 1);
        x = label(52, y, "B0 ");
        x = gfx_hex(x, y, d[0], 2, c, 1);
        x = label(x, y, " AT ");
        x = gfx_hex(x, y, d[11], 2, C_WHITE, 1);
        x = label(x, y, " CL ");
        x = gfx_dec(x, y, (int32_t)(d[26] | d[27] << 8), C_WHITE, 1);
        x = label(x, y, " SIZE ");
        gfx_dec(x, y, (int32_t)((uint32_t)d[28] | (uint32_t)d[29] << 8 | (uint32_t)d[30] << 16
                                | (uint32_t)d[31] << 24), C_WHITE, 1);
    }
}

void pp_frame(uint16_t *buf)
{
    static const char *const tone_names[] = { "OFF", "BOTH", "WORD 0", "WORD 1" };
    uint32_t p = pp_p;
    int x, y, i;

    if (!open_)
        return;
    gfx_begin(buf);
    gfx_rect(0, 0, gfx_w, gfx_h, GFX_BLACK);

    y = 2;
    x = gfx_text(2, y, "PLAY PROBE  ", C_WHITE, 1);
    x = label(x, y, "LOAD TE ");
    if (!pp_windows) {
        label(x, y, "MEASURING");
    } else {
        x = pct(x, y, permille(pp_te, p));
        x = label(x, y, "  PROBE ");
        x = pct(x, y, permille(pp_pp, p));
        x = label(x, y, "  SDK ENG ");
        x = pct(x, y, permille(pp_e, p));
        x = label(x, y, "  PEAK ");
        x = pct(x, y, permille(pp_peak, p));
        x = label(x, y, "  LOOP ");
        x = gfx_dec(x, y, (int32_t)pp_loops, C_WHITE, 1);
        label(x, y, "/S");
    }

    y = 12;
    x = gfx_text(2, y, "B ", C_BLUE, 1);
    x = label(x, y, "TONE ");
    x = gfx_text(x, y, tone_names[pp_tone_mode & 3], pp_tone_mode ? C_AMBER : C_WHITE, 1);
    x = label(x + 4, y, " ");
    x = gfx_dec(x, y, tone_dbs[tone_db], C_WHITE, 1);
    x = label(x, y, " DB   TX PEAK ");
    x = gfx_hex(x, y, pp_tx_peak, 8, C_WHITE, 1);
    x = label(x, y, " LOW16 ");
    x = gfx_hex(x, y, pp_tx_low, 4, C_WHITE, 1);
    x = label(x, y, " AT ");
    gfx_hex(x, y, pp_tx_at, 8, C_WHITE, 1);

    y = 21;
    x = gfx_text(2, y, "O ", C_AMBER, 1);
    x = label(x, y, "MIX ");
    x = gfx_dec(x, y, pp_mix_n, C_WHITE, 1);
    x = label(x, y, " STREAMS  ");
    if (mix_err)
        gfx_text(x, y, "NO MEMORY", C_RED, 1);
    else if (pp_mix_n)
        label(x, y, pp_mix_buf ? "(IN PROBE LOAD, SILENT)" : "ALLOCATING");

    y = 30;
    x = gfx_text(2, y, "W ", C_WHITE, 1);
    x = label(x, y, "READ ");
    x = gfx_text(x, y, pp_speed_on ? "ON " : "OFF ", pp_speed_on ? C_AMBER : C_WHITE, 1);
    x = gfx_dec(x, y, (int32_t)(chunks[chunk_i] >> 10), C_WHITE, 1);
    x = label(x, y, "K ");
    if (pp_speed_on) {
        x = gfx_text(x, y, speed_file >= 0 ? pp_files[speed_file].e.name : "IMAGE", C_WHITE, 1);
        x = label(x + 4, y, " ");
        x = gfx_dec(x, y, (int32_t)pp_sp_kbs, C_GREEN, 1);
        x = label(x, y, " KB/S  AVG ");
        x = ms(x, y, pp_sp_avg_us, C_WHITE);
        x = label(x, y, " WORST ");
        x = ms(x, y, pp_sp_worst_us, C_WHITE);
        x = label(x, y, " / ");
        x = ms(x, y, pp_sp_all_worst_us, C_AMBER);
        x = label(x, y, " MS");
    }
    if (pp_sp_errs) {
        x = label(x, y, " ERR ");
        gfx_dec(x, y, (int32_t)pp_sp_errs, C_RED, 1);
    }

    y = 39;
    x = gfx_text(2, y, "O ", C_AMBER, 1);
    x = label(x, y, "HEAP ");
    if (pp_hp_state) {
        x = label(x, y, "LIST ");
        x = mb(x, y, pp_hp_list_sum, C_WHITE);
        x = label(x, y, " BIG ");
        x = mb(x, y, pp_hp_list_big, C_WHITE);
        x = label(x, y, " N ");
        x = gfx_dec(x, y, (int32_t)pp_hp_list_n, pp_hp_list_ok == 1 ? C_WHITE : C_RED, 1);
        x = label(x, y, "  ALLOC BIG ");
        x = mb(x, y, pp_hp_big, C_GREEN);
        x = label(x, y, " SUM ");
        x = mb(x, y, pp_hp_total, C_GREEN);
        x = label(x, y, " MB IN ");
        x = gfx_dec(x, y, (int32_t)pp_hp_blocks, C_WHITE, 1);
        x = label(x, y, "  ");
        x = ms(x, y, pp_hp_us, C_WHITE);
        label(x, y, " MS");
    }

    y = 48;
    x = gfx_text(2, y, "G ", C_GREEN, 1);
    x = label(x, y, "DISK ");
    if (pp_scan_state == 1) {
        label(x, y, "SCANNING");
    } else if (pp_scan_state == 2) {
        if (pp_scan_err == 1) {
            x = gfx_text(x, y, "CANNOT OPEN TAPE_C.RAW", C_RED, 1);
        } else if (!mounted) {
            x = gfx_text(x, y, fat_err[-pp_scan_err], C_RED, 1);
        } else {
            x = label(x, y, "FAT");
            x = gfx_dec(x, y, vol.type, C_WHITE, 1);
            x = label(x, y, " ");
            if (vol.clus_bytes < 1024) {
                x = gfx_dec(x, y, (int32_t)vol.clus_bytes, C_WHITE, 1);
                x = label(x, y, "B");
            } else {
                x = gfx_dec(x, y, (int32_t)(vol.clus_bytes >> 10), C_WHITE, 1);
                x = label(x, y, "K");
            }
            x = label(x, y, " CLUSTERS  ROOT ");
            x = gfx_dec(x, y, (int32_t)root_total, C_WHITE, 1);
            x = label(x, y, "  PLAY/ ");
            if (pp_scan_err)
                x = gfx_text(x, y, fat_err[-pp_scan_err], C_RED, 1);
            else {
                x = gfx_dec(x, y, (int32_t)play_total, C_WHITE, 1);
                x = label(x, y, " ENTRIES");
            }
        }
        x = label(x, y, "  ");
        x = ms(x, y, scan_us, C_WHITE);
        label(x, y, " MS");
        if (mounted)
            draw_geometry(57);
        if (!pp_scan_err)
            draw_files(66);
        else if (mounted)
            draw_raw(66);
    }

    y = 124;
    x = label(2, y, "KEYS ID D/U S MODE:");
    for (i = klog_n < NLOG ? NLOG - (int)klog_n : 0; i < NLOG; i++) {
        int k = (klog_at + i) % NLOG;

        x = gfx_dec(x + 6, y, klog_key[k], C_WHITE, 1);
        x = gfx_text(x, y, klog_bits[k] & 1 ? "D" : "U", C_WHITE, 1);
        if (klog_bits[k] & 2)
            x = gfx_text(x, y, "S", C_AMBER, 1);
        x = gfx_dec(x + 1, y, klog_mode[k], C_GREY, 1);
    }
    label(2, 133, "PUSH: B TONE  G SCAN  W READ  O HEAP     SHIFT+SPEECH: CLOSE");
    label(2, 142, "TURN: B LEVEL  G SCROLL  W SIZE  O MIX STREAMS");
}
