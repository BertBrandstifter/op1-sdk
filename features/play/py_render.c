/* py_render: PLAY's song rendered to an AIFF file on flash (README.md, new_pattern_tape.md 2.3).
 *
 * rec in the song view asks for a length in bars, rec again starts. The song then plays from the
 * cursor row "offline": not into TE's master bus but into a file, as fast or as slow as the CPU
 * allows, with every SYNTH track at full quality (no ECO, all voices). The file (RENDnn.AIF, 16
 * bit, 44.1 kHz; stereo if an AUDIO track is in the mix, else mono) lands in PLAY's render folder
 * and shows up in the AUDIO tracks' file list: a few heavy SYNTH tracks become one cheap stream.
 *
 *   audio interrupt  py_rd_audio, every block, instead of PLAY's mix: it renders in steps (one
 *                    pattern track of the next block per step, then the buses and the mix) until
 *                    RD_BUDGET cycles are used, so TE's own audio and the UI keep running. The
 *                    engines are only ever called from the interrupt, as when playing. A block is
 *                    only started when every AUDIO ring holds it (no underruns in a render) and
 *                    the capture ring has room.
 *   main loop        py_rd_main: start (file, header, heads at the start row), the capture ring
 *                    to the file (big-endian, RD_WRITE frames a pass), the end, cancel (the file
 *                    is deleted), disk full.
 * What is rendered: the tracks as they sound (levels, mute, solo), PLAY's own soft clip, the
 * master at 0 dB; not TE's master EQ / FX / drive (they act again when the file plays). */
#include <op1/te246.h>
#include "py_core.h"
#include "py_song.h"
#include "play.h"
#include "py_ui.h"
#include "py_yaffs.h"

#define RD_RING     16384u                      /* capture ring, frames (stereo int16: 64 KB) */
#define RD_WRITE    2048u                       /* frames written per main-loop pass at most */
#define RD_BUDGET   230000u                     /* CYCLES per audio block spent rendering (~40 %) */
#define RD_HEADER   54

enum { RD_IDLE, RD_RUN, RD_FLUSH };

static volatile uint8_t state;
static volatile uint8_t req_start, req_cancel;
static volatile uint8_t req_row;
static volatile uint16_t req_bars;

static int16_t *ring;                           /* RD_RING x 2, native int16 */
static volatile uint32_t ring_wr, ring_rd;      /* frames; the interrupt writes, the main loop reads */
static uint32_t total;                          /* frames to render */
static volatile uint32_t done;                  /* frames rendered */
static uint32_t written;                        /* frames in the file */
static uint8_t step;                            /* interrupt: 0..PS_TRACKS-1 a track, PS_TRACKS the mix */
static uint8_t stereo;
static int fd = -1;
static char name[13], path[64], msg[48];
static uint8_t wbuf[RD_WRITE * 4] __attribute__((aligned(4)));   /* yaffs: even addresses */

static inline uint32_t cycles(void)
{
    uint32_t c;

    __asm__ volatile("%0 = CYCLES;" : "=d"(c));
    return c;
}

/* ------------------------------------------------------------------ any thread */

int py_rd_busy(void)
{
    return state != RD_IDLE || req_start;
}

void py_rd_request(int row, int bars)
{
    if (py_rd_busy())
        return;
    req_row = (uint8_t)row;
    req_bars = (uint16_t)(bars < 1 ? 1 : bars);
    req_cancel = 0;
    req_start = 1;
}

void py_rd_cancel(void)
{
    req_cancel = 1;
}

int py_rd_percent(void)
{
    uint32_t t = total / 100u;

    return t ? (int)(done / t) : 0;
}

const char *py_rd_name(void)
{
    return name;
}

/* ------------------------------------------------------------------ audio interrupt */

int py_rd_audio(void)
{
    static int32_t l[PY_BLOCK], r[PY_BLOCK];
    uint32_t c0;
    int k, f;

    if (state == RD_IDLE)
        return 0;
    if (state != RD_RUN || !py_mx.playing)
        return 1;
    c0 = cycles();
    do {
        if (step == 0) {                        /* a new block: only when nothing can run short */
            int wait = 0;

            if (done >= total) {
                state = RD_FLUSH;
                py_mx.playing = 0;
                break;
            }
            if (RD_RING - (ring_wr - ring_rd) < PY_BLOCK)
                break;
            for (k = 0; k < PY_TRACKS; k++)
                wait |= py_mx.set->t[k].used && py_mx.ring[k].buf && py_ahead(&py_mx, k) < PY_BLOCK;
            if (wait)
                break;
        }
        if (step < PS_TRACKS) {
            py_pat_step(py_mx.pos, step);
            step++;
        } else {
            uint32_t w = ring_wr;

            py_pat_step(py_mx.pos, PS_TRACKS);
            for (f = 0; f < PY_BLOCK; f++)
                l[f] = r[f] = 0;
            py_mix(&py_mx, l, r, 1);
            for (f = 0; f < PY_BLOCK; f++) {
                int16_t *o = ring + ((w + (uint32_t)f) & (RD_RING - 1)) * 2;

                o[0] = (int16_t)(l[f] >> 16);
                o[1] = (int16_t)(r[f] >> 16);
            }
            __asm__ volatile("" ::: "memory");
            ring_wr = w + PY_BLOCK;
            done += PY_BLOCK;
            step = 0;
        }
    } while (cycles() - c0 < RD_BUDGET);
    return 1;
}

/* ------------------------------------------------------------------ main loop */

static void be16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void be32(uint8_t *p, uint32_t v) { be16(p, v >> 16); be16(p + 2, v); }

/* the AIFF header for `frames` frames: FORM / AIFF, COMM (18), SSND */
static void header(uint8_t *h, uint32_t frames, int ch)
{
    static const uint8_t rate[10] = { 0x40, 0x0e, 0xac, 0x44, 0, 0, 0, 0, 0, 0 };   /* 44100, 80-bit */
    uint32_t data = frames * (uint32_t)ch * 2u;
    int i;

    h[0] = 'F'; h[1] = 'O'; h[2] = 'R'; h[3] = 'M';
    be32(h + 4, RD_HEADER - 8 + data);
    h[8] = 'A'; h[9] = 'I'; h[10] = 'F'; h[11] = 'F';
    h[12] = 'C'; h[13] = 'O'; h[14] = 'M'; h[15] = 'M';
    be32(h + 16, 18);
    be16(h + 20, (uint32_t)ch);
    be32(h + 22, frames);
    be16(h + 26, 16);
    for (i = 0; i < 10; i++)
        h[28 + i] = rate[i];
    h[38] = 'S'; h[39] = 'S'; h[40] = 'N'; h[41] = 'D';
    be32(h + 42, 8 + data);
    be32(h + 46, 0);
    be32(h + 50, 0);
}

/* the render is over: back to the stopped song; ok = keep the file */
static void finish(int ok, const char *why)
{
    py_mx.playing = 0;
    state = RD_IDLE;
    if (fd >= 0)
        TE_FILE_CLOSE(fd);
    fd = -1;
    if (!ok)
        PY_UNLINK(path);
    if (ring)
        TE_FREE(ring);
    ring = 0;
    py_pat_hd(0);
    py_pat_set_start(0);
    py_song_audio();
    py_update_gains();
    py_seek_to(0, 0);
    if (ok) {
        TE_SNPRINTF(msg, sizeof(msg), "%s READY: PICK IT ON AN AUDIO TRACK", name);
        py_req_rescan();                        /* the file list knows it */
        py_note(msg);
    } else {
        py_note(why);
    }
}

static void start(void)
{
    static uint32_t st[32];                     /* TE's stat buffer is 0x48 bytes */
    int i, k;

    py_mx.playing = 0;
    total = (uint32_t)req_bars * py_clock_bar();
    done = written = 0;
    step = 0;
    ring_wr = ring_rd = 0;
    if (!total)
        return;
    PY_MKDIR(PY_DIR_R, 0);
    for (i = 1; i < 100; i++) {                 /* the first free name */
        TE_SNPRINTF(name, sizeof(name), "REND%d%d.AIF", i / 10, i % 10);
        TE_SNPRINTF(path, sizeof(path), "%s/%s", PY_DIR_R, name);
        if (PY_STAT(path, st) < 0)
            break;
    }
    if (i == 100) {
        py_note("RENDER: 99 FILES ALREADY");
        return;
    }
    ring = TE_HEAP_MALLOC(TE_HEAP_SAMPLES, RD_RING * 4u);
    if (!ring) {
        py_note("RENDER: NO MEMORY");
        return;
    }
    py_pat_audition(-1, 0);
    py_pat_set_start(req_row);
    py_pat_hd(1);
    state = RD_RUN;                             /* (the gains: master at 0 dB while rendering) */
    py_song_audio();
    py_update_gains();
    stereo = 0;
    for (k = 0; k < PY_TRACKS; k++)
        stereo |= py_song.trk[k].type == PS_AUDIO && py_mx.gain[k] && py_mx.set->t[k].used;
    fd = TE_FILE_OPEN(path, TE_FILE_CREATE, TE_FILE_CREATE_MODE);
    header(wbuf, total, stereo ? 2 : 1);
    if (fd < 0 || TE_FILE_WRITE(fd, wbuf, RD_HEADER) != RD_HEADER) {
        finish(0, "RENDER: CANNOT WRITE THE FILE");
        return;
    }
    py_seek_to(0, 1);                           /* heads and rings at bar 1; the interrupt renders */
}

/* 1 = a render is running: PLAY's main loop does nothing else that touches the song or the set */
int py_rd_main(void)
{
    uint32_t n, i, bytes;

    if (req_start && state == RD_IDLE) {
        req_start = 0;
        start();
    }
    if (state == RD_IDLE)
        return 0;
    if (req_cancel) {
        req_cancel = 0;
        finish(0, "RENDER CANCELLED");
        return 0;
    }
    n = ring_wr - ring_rd;
    if (n > RD_WRITE)
        n = RD_WRITE;
    if (n) {
        uint32_t keep = n < total - written ? n : total - written;     /* the last block: up to the bar */

        for (i = 0; i < keep; i++) {
            const int16_t *s = ring + ((ring_rd + i) & (RD_RING - 1)) * 2;

            if (stereo) {
                be16(wbuf + 4 * i, (uint16_t)s[0]);
                be16(wbuf + 4 * i + 2, (uint16_t)s[1]);
            } else {
                be16(wbuf + 2 * i, (uint16_t)s[0]);
            }
        }
        bytes = keep * (stereo ? 4u : 2u);
        written += keep;
        if (bytes && TE_FILE_WRITE(fd, wbuf, bytes) != (int)bytes) {
            finish(0, "RENDER: DISK FULL");
            return 0;
        }
        __asm__ volatile("" ::: "memory");
        ring_rd += n;
    }
    if (state == RD_FLUSH && ring_wr == ring_rd)
        finish(1, 0);
    return state != RD_IDLE;
}
