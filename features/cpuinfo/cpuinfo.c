/* cpuinfo: how much of the OP-1's DSP the audio takes, measured on the device (spec: README.md).
 *
 * shift + speech: panel on / off. The first opening wraps TE's audio block callback
 * (TE_AUDIO_BLOCK_CB, called by the IVG11 handler once per 64-frame block) and starts counting;
 * nothing runs before that.
 *   - Per block: CYCLES spent inside TE's callback (work) and CYCLES since the last call (period).
 *     Load = work / period. CCLK = period x 44100 / 64 (if the codec runs at 44.1 kHz).
 *   - SDK engines: the cycles enginehost spends in our engines' renders (eh_cycles), same blocks.
 *   - PLL_CTL / PLL_DIV / VR_CTL as read, and the CLKIN they imply with the measured CCLK.
 *   - SYSCFG.CCEN (the cycle counter): set in the callback (supervisor, IVG11) if it was off. */
#include <op1/te246.h>
#include <op1/gfx.h>

#define CI_PLL_CTL      ((volatile uint16_t *)0xffc00000)       /* MSEL 14:9, DF 0 */
#define CI_PLL_DIV      ((volatile uint16_t *)0xffc00004)       /* CSEL 5:4, SSEL 3:0 */
#define CI_VR_CTL       ((volatile uint16_t *)0xffc00008)

#define WIN             172                     /* blocks per window: ~0.25 s */
#define HIST            150                     /* windows in the graph: ~37 s */

typedef void (*cb_fn)(int32_t *tx, const int32_t *rx);

extern volatile uint8_t eh_prof;                /* enginehost */
extern volatile uint32_t eh_cycles;

static volatile uint8_t open_, started;
static cb_fn orig;
volatile uint32_t ci_rewraps;

/* audio interrupt state */
static uint32_t last_t0, acc_w, acc_p, acc_e, max_w, n, last_e;
static uint8_t have_last;
volatile uint8_t ci_ccen_was;               /* 0 unknown, 1 was on, 2 was off (we set it) */

/* published once per window (audio interrupt), read by the GUI thread */
volatile uint32_t ci_avg_w, ci_avg_p, ci_avg_e, ci_windows;   /* bench reads */
static volatile uint32_t peak_w;
static volatile uint32_t all_peak;              /* the worst block since the panel opened */
static volatile uint16_t hist_avg[HIST], hist_peak[HIST];      /* load x 10 (0.1 %) */
static volatile uint16_t hist_at;

static inline uint32_t cycles(void)
{
    uint32_t c;

    __asm__ volatile("%0 = CYCLES;" : "=d"(c));
    return c;
}

static uint32_t permille(uint32_t w, uint32_t p)
{
    if (!p)
        return 0;
    while (w > 4000000u) {                      /* keep w * 1000 in 32 bits */
        w >>= 1;
        p >>= 1;
    }
    return p ? w * 1000u / p : 0;
}

/* TE's audio block callback, wrapped (IVG11, supervisor) */
static void ci_cb(int32_t *tx, const int32_t *rx)
{
    uint32_t t0, t1, w, e;

    if (!ci_ccen_was) {
        uint32_t s;

        __asm__ volatile("%0 = SYSCFG;" : "=d"(s));
        if (s & 2) {
            ci_ccen_was = 1;
        } else {
            s |= 2;
            __asm__ volatile("SYSCFG = %0; CSYNC;" : : "d"(s));
            ci_ccen_was = 2;
        }
    }
    t0 = cycles();
    orig(tx, rx);
    t1 = cycles();
    w = t1 - t0;
    e = eh_cycles;
    if (have_last) {
        acc_w += w;
        acc_p += t0 - last_t0;
        acc_e += e - last_e;
        if (w > max_w)
            max_w = w;
        if (++n == WIN) {
            uint32_t a = acc_w / WIN, p = acc_p / WIN, h = hist_at;

            ci_avg_w = a;
            ci_avg_p = p;
            ci_avg_e = acc_e / WIN;
            peak_w = max_w;
            if (max_w > all_peak)
                all_peak = max_w;
            hist_avg[h] = (uint16_t)permille(a, p);
            hist_peak[h] = (uint16_t)permille(max_w, p);
            hist_at = (uint16_t)(h + 1 < HIST ? h + 1 : 0);
            ci_windows++;
            acc_w = acc_p = acc_e = max_w = n = 0;
        }
    }
    have_last = 1;
    last_t0 = t0;
    last_e = e;
}

/* main_tick (main thread): wrap the callback once the panel was opened; again if TE re-sets it */
void ci_main(void)
{
    cb_fn cur;

    if (!started)
        return;
    cur = *TE_AUDIO_BLOCK_CB;
    if (!cur || cur == ci_cb)
        return;
    if (orig)
        ci_rewraps++;
    orig = cur;                                 /* before the pointer: the interrupt reads it next */
    __asm__ volatile("" ::: "memory");
    eh_prof = 1;
    *TE_AUDIO_BLOCK_CB = ci_cb;
}

/* on_key filter (KeyHandler thread): shift + speech opens / closes the panel */
int ci_key(int key, int down)
{
    if (key != TE_KEY_SPEECH || !*TE_SHIFT_HELD)
        return 0;
    if (down) {
        open_ = (uint8_t)!open_;
        started = 1;
    }
    return 1;
}

/* ------------------------------------------------------------------ GUI thread */

#define C_GREY      GFX_RGB(0x8a, 0x8a, 0x99)
#define C_DARK      GFX_RGB(0x30, 0x30, 0x38)
#define C_GREEN     GFX_RGB(0x3d, 0xdc, 0x97)
#define C_RED       GFX_RGB(0xff, 0x4f, 0x5e)
#define C_AMBER     GFX_RGB(0xff, 0xb0, 0x3d)

/* "12.3" from tenths */
static int tenths(int x, int y, uint32_t v, uint16_t c, int scale)
{
    x = gfx_dec(x, y, (int32_t)(v / 10), c, scale);
    x = gfx_text(x, y, ".", c, scale);
    return gfx_dec(x, y, (int32_t)(v % 10), c, scale);
}

static int pct(int x, int y, uint32_t pm, int scale)
{
    uint16_t c = pm >= 900 ? C_RED : pm >= 700 ? C_AMBER : C_GREEN;

    x = tenths(x, y, pm, c, scale);
    return x;
}

static int kcyc(int x, int y, uint32_t v)
{
    x = gfx_dec(x, y, (int32_t)((v + 500) / 1000), GFX_WHITE, 1);
    return gfx_text(x, y, "K", GFX_WHITE, 1);
}

void ci_frame(uint16_t *buf)
{
    uint32_t a = ci_avg_w, p = ci_avg_p, e = ci_avg_e, pk = peak_w, pll, div, msel, csel, cclk, clkin;
    int x, y, i, h;

    if (!open_)
        return;
    gfx_begin(buf);
    gfx_rect(0, 0, gfx_w, gfx_h, GFX_BLACK);

    x = gfx_text(2, 2, "AUDIO LOAD ", GFX_WHITE, 2);
    if (!ci_windows) {
        gfx_text(x, 2, ci_ccen_was == 0 && started ? "WAITING" : "MEASURING", C_GREY, 2);
    } else {
        x = pct(x, 2, permille(a, p), 2);
        x = gfx_text(x + 8, 2, "PEAK ", GFX_WHITE, 2);
        pct(x, 2, permille(pk, p), 2);
    }

    y = 22;
    x = gfx_text(2, y, "BLOCK AVG ", C_GREY, 1);
    x = kcyc(x, y, a);
    x = gfx_text(x, y, "  MAX ", C_GREY, 1);
    x = kcyc(x, y, pk);
    x = gfx_text(x, y, "  WORST ", C_GREY, 1);
    x = kcyc(x, y, all_peak);
    x = gfx_text(x, y, "  OF ", C_GREY, 1);
    kcyc(x, y, p);

    y += 9;
    x = gfx_text(2, y, "SDK ENGINES ", C_GREY, 1);
    x = pct(x, y, permille(e, p), 1);
    x = gfx_text(x, y, "  ", C_GREY, 1);
    x = kcyc(x, y, e);
    gfx_text(x, y, e > a && a ? "  OUTSIDE CB?" : "", C_AMBER, 1);

    /* clock: measured, and the PLL as programmed */
    pll = *CI_PLL_CTL;
    div = *CI_PLL_DIV;
    msel = (pll >> 9) & 63;
    msel = msel ? msel : 64;
    csel = (div >> 4) & 3;
    cclk = p * 689u + p / 16u;                  /* Hz, at 44.1 kHz / 64 */
    clkin = ((cclk / 1000u) << csel) * ((pll & 1) ? 2u : 1u) / msel;      /* kHz */
    y += 9;
    x = gfx_text(2, y, "CCLK ", C_GREY, 1);
    x = tenths(x, y, (cclk + 50000) / 100000, C_GREEN, 1);
    x = gfx_text(x, y, " MHZ AT 44.1K  CLKIN ", C_GREY, 1);
    x = tenths(x, y, (clkin + 50) / 100, GFX_WHITE, 1);
    gfx_text(x, y, " MHZ", C_GREY, 1);
    y += 9;
    x = gfx_text(2, y, "PLL CTL ", C_GREY, 1);
    x = gfx_hex(x, y, pll, 4, GFX_WHITE, 1);
    x = gfx_text(x, y, " MSEL ", C_GREY, 1);
    x = gfx_dec(x, y, (int32_t)msel, GFX_WHITE, 1);
    x = gfx_text(x, y, " DF ", C_GREY, 1);
    x = gfx_dec(x, y, (int32_t)(pll & 1), GFX_WHITE, 1);
    x = gfx_text(x, y, "  PLL DIV ", C_GREY, 1);
    x = gfx_hex(x, y, div, 4, GFX_WHITE, 1);
    x = gfx_text(x, y, " CSEL ", C_GREY, 1);
    x = gfx_dec(x, y, (int32_t)csel, GFX_WHITE, 1);
    x = gfx_text(x, y, " SSEL ", C_GREY, 1);
    gfx_dec(x, y, (int32_t)(div & 15), GFX_WHITE, 1);
    y += 9;
    x = gfx_text(2, y, "VR CTL ", C_GREY, 1);
    x = gfx_hex(x, y, *CI_VR_CTL, 4, GFX_WHITE, 1);
    x = gfx_text(x, y, "  CCEN ", C_GREY, 1);
    x = gfx_text(x, y, ci_ccen_was == 1 ? "WAS ON" : ci_ccen_was == 2 ? "SET BY US" : "?", GFX_WHITE, 1);
    x = gfx_text(x, y, "  CB ", C_GREY, 1);
    x = gfx_hex(x, y, (uint32_t)orig, 8, GFX_WHITE, 1);
    x = gfx_text(x, y, "  REWRAP ", C_GREY, 1);
    gfx_dec(x, y, (int32_t)ci_rewraps, GFX_WHITE, 1);

    /* history: one column per window (~0.25 s), peak amber over average green; 100 % at the top */
    y = 80;
    gfx_rect(8, y, HIST * 2, 60, C_DARK);
    gfx_rect(8, y + 60 - 42, HIST * 2, 1, C_GREY);         /* 70 % */
    for (i = 0; i < HIST; i++) {
        int k = (hist_at + i) % HIST;
        int av = hist_avg[k] * 60 / 1000, pv = hist_peak[k] * 60 / 1000;

        av = av > 60 ? 60 : av;
        pv = pv > 60 ? 60 : pv;
        if (pv > av)
            gfx_rect(8 + i * 2, y + 60 - pv, 2, pv - av, C_AMBER);
        gfx_rect(8 + i * 2, y + 60 - av, 2, av, C_GREEN);
    }
    h = y + 63;
    x = gfx_text(8, h, "37 S  ", C_GREY, 1);
    x = gfx_text(x, h, "AVG", C_GREEN, 1);
    x = gfx_text(x, h, " / ", C_GREY, 1);
    x = gfx_text(x, h, "PEAK", C_AMBER, 1);
    gfx_text(x, h, " PER 0.25 S  LINE = 70 PCT", C_GREY, 1);
}
