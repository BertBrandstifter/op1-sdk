/* PLAY load bench: three SYNTH tracks on ACID 3 (one pattern, a note every 1/16), ECO off then on;
 * per audio block the cycles of PLAY's hook (max, sum), per track and in ACID's renders. The
 * stand-ins and helpers are driver.c's (its main is renamed and unused). Results in ld[]. */
#define main play_checks_main
#include "driver.c"
#undef main

extern void ac_bank_knobs(int e, int16_t *k);
extern volatile uint32_t py_trk_cyc[8], eh_cycles;
extern volatile uint8_t eh_prof;
extern const ps_head *py_pat_head(int t);
volatile uint32_t ld[40];                       /* per run (0 = ECO off, 1 = on): 20 words */

static inline uint32_t cyc(void)
{
    uint32_t c;

    __asm__ volatile("%0 = CYCLES;" : "=d"(c));
    return c;
}

static void run(int eco, volatile uint32_t *r)
{
    uint32_t rb[3] = { 0, 0, 0 }, tc[3] = { 0, 0, 0 };
    uint32_t mx = 0, sum = 0, e0 = eh_cycles, notes0 = py_syn_notes, t0[3], over = 0;
    int b, t, k;
    static uint32_t hist[8];

    for (k = 0; k < 8; k++)
        hist[k] = 0;
    for (t = 0; t < 3; t++) {
        py_song.trk[t].eco = (uint8_t)eco;
        t0[t] = py_trk_cyc[t];
    }
    press(TE_KEY_STOP);
    py_main();
    press(TE_KEY_STOP);                         /* bar 1 */
    py_main();
    press(TE_KEY_PLAY);
    py_main();
    for (b = 0; b < 1400; b++) {
        uint32_t c0 = cyc(), d;

        uint32_t bt[3];

        for (t = 0; t < 3; t++)
            bt[t] = py_trk_cyc[t];
        clear_master();
        hook_volume();
        d = cyc() - c0;
        for (t = 0; t < 3; t++)                 /* blocks the track rendered (its window reset: skipped) */
            if (py_trk_cyc[t] >= bt[t] && py_trk_cyc[t] - bt[t] > 20000) {
                rb[t]++;
                tc[t] += py_trk_cyc[t] - bt[t];
            }
        sum += d;
        mx = d > mx ? d : mx;
        over += d > 120000;                     /* blocks above ~3 ACID renders */
        k = (int)(d / 40000);
        hist[k > 7 ? 7 : k]++;
        if (!(b % 4))
            py_main();
    }
    r[0] = mx;
    r[1] = sum / 1400;
    r[2] = eh_cycles - e0;                      /* ACID renders (eh_render_inst), all */
    r[3] = py_syn_notes - notes0;
    for (t = 0; t < 3; t++) {
        r[4 + t] = rb[t];
        r[11 + t] = rb[t] ? tc[t] / rb[t] : 0;
    }
    (void)t0;
    r[7] = over;
    for (k = 0; k < 3; k++)
        r[8 + k] = hist[k + 3];
    r[17] = py_song.cell[0][0] | (py_song.cell[0][1] << 8) | (py_song.cell[0][2] << 16) | ((uint32_t)py_mx.playing << 24);
    r[18] = py_song.chain[py_song.cell[0][0] & 127][0].pat | (py_mx.pos & 0xffffff) << 8;
    r[19] = py_pat_head(0)->active | py_pat_head(0)->pat << 8 | (py_pat_head(0)->len & 0xffff) << 16;
    r[15] = nr_owned | nr_loaded << 16 | nr_pat[0] << 24;
    r[14] = nr_pat[4] | nr_pat[5] << 8 | nr_pat[6] << 16 | nr_pat[29] << 24;
    r[16] = py_song.trk[0].type | (py_song.trk[1].type << 8) | (py_song.trk[2].type << 16);
}

int main(void)
{
    int i, t;
    uint8_t *p = nr_pat;

    *TE_EDIT_MODE = TE_EDIT_MODE_TAPE;
    *TE_BPM = 120.0f;
    image = add("/yaffs2/user/tape_c.raw", 0);
    fs[image].data = (uint8_t *)0x02200000;
    hook_volume();                              /* the first hook runs every feature's init (NOTE ROLL's resets nr_pat) */
    mains(4);
    for (t = 0; t < 3; t++) {                   /* ACID 3 bank sounds on synth slots 1..3 */
        int16_t k[8];
        volatile int16_t *kn = (int16_t *)(0xff8013c4 + t * 16);

        ac_bank_knobs(t, k);
        for (i = 0; i < 8; i++)
            kn[i] = k[i];
    }
    *(volatile int32_t *)0xff8016d4 = 0;
    for (i = 0; i < 8; i++)
        *(volatile int32_t *)(0x01dfe4e0 + 4 * i) = 0;
    for (i = 0; i < 1796; i++)
        p[i] = 0;
    p[0] = 16;
    for (i = 0; i < 64; i++) {
        uint8_t *sp = p + 4 + i * 28;
        int n;

        for (n = 0; n < 6; n++)
            sp[n * 4] = 0xff;
        sp[25] = 100;
        sp[26] = 1;
        if (i < 16) {
            sp[0] = (uint8_t)(36 + (i * 5) % 13);   /* a note every step */
            sp[1] = 100;
            sp[2] = 5;                          /* 1.5 steps: legato overlaps (voice stealing) */
        }
    }
    nr_owned = 1;
    nr_loaded = 1;
    for (t = 0; t < 3; t++) {                   /* three SYNTH tracks from the ADD row */
        turn(0, 9);
        press(TE_KEY_WHITE_PUSH);
    }
    for (t = 0; t < 3; t++) {
        turn(0, -9);
        turn(0, t);
        turn(3, 1);                             /* orange: pattern 1 */
        py_song.trk[t].sound = (uint8_t)t;
        py_song.trk[t].voices = 2;
    }
    mains(40);
    eh_prof = 1;
    run(0, ld);
    run(1, ld + 20);
    press(TE_KEY_STOP);
    py_main();
    return 0;
}
