/* host test for py_drum.c (tools/test_all.py builds it with its sibling py_drum.c).
 *
 * Pitch maths, TE's position encoding, and the voice: a one-shot plays to its end past the FULL
 * release, a gated hit fades out over 128 samples, reverse, loop while held, an octave up, a start
 * delay, the level law, voice stealing, positions past the buffer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "py_drum.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int16_t pcm[PY_DRUM_FRAMES + 8];

static void encode(uint32_t frame, int16_t *hi, int16_t *lo)
{
    uint32_t pos32 = (uint32_t)((((uint64_t)frame << 31) + PY_DRUM_FRAMES - 1) / PY_DRUM_FRAMES);

    pos32 = (pos32 + 1) & ~1u;                  /* TE keeps it in W1:W5 without bit 0: even, >= exact */

    *hi = (int16_t)(pos32 >> 16);
    *lo = (int16_t)((pos32 & 0xffff) >> 1);
}

/* a key block: pitch semis, start / end frames, playmode, reverse, volume */
static void key(int16_t k[8], int semis, uint32_t start, uint32_t end, int mode, int rev, int vol)
{
    k[0] = (int16_t)(semis * 512);
    encode(start, &k[1], &k[5]);
    encode(end, &k[2], &k[6]);
    k[3] = (int16_t)(mode << 13);
    k[4] = (int16_t)(rev ? 16384 : 8192);
    k[7] = (int16_t)vol;
}

static int32_t out[4096];

static void run(py_drums *d, int n)
{
    memset(out, 0, sizeof(out));
    py_drum_render(d, out, n);
}

/* gain of a hit at gain 32767 and volume 8192 */
static int32_t unity(void) { return (int32_t)(((int64_t)(((int64_t)32767 * 32768) >> 15) * PY_DRUM_CAL) >> 15); }

int main(void)
{
    py_drums d;
    int16_t k[8];
    int i, ok;
    int32_t g;

    /* pitch */
    CHECK(py_pow2_q16(0) == 65536 && py_pow2_q16(12 * 512) == 131072 && py_pow2_q16(-12 * 512) == 32768);
    CHECK(py_pow2_q16(7 * 512) == 98193 && py_pow2_q16(-5 * 512) == 98193 / 2);
    CHECK(py_pow2_q16(256) > 65536 && py_pow2_q16(256) < 69433);
    CHECK(py_pow2_q16(200 * 512) == py_pow2_q16(96 * 512));
    /* positions round-trip */
    for (i = 0; i < 529200; i += 7919) {
        int16_t hi, lo;

        encode((uint32_t)i, &hi, &lo);
        CHECK(py_drum_frames((uint16_t)hi, (uint16_t)lo) == (uint32_t)i);
    }

    for (i = 0; i < PY_DRUM_FRAMES; i++)
        pcm[i] = (int16_t)((i * 37) % 30000 - 15000);
    g = unity();

    /* one-shot, FULL: plays 100 frames past its 10-sample release, then silence */
    memset(&d, 0, sizeof(d));
    key(k, 0, 1000, 1100, 1, 0, 8192);
    py_drum_hit(&d, pcm, k, 0, 32767, 10, 1, 0);
    run(&d, 200);
    for (ok = 1, i = 0; i < 200; i++)
        ok &= out[i] == (i < 100 ? (int32_t)(((int64_t)pcm[1000 + i] * g) >> 15) : 0);
    CHECK(ok && !py_drum_active(&d));

    /* gated (not FULL): held 20 samples, then a 128-sample fade */
    memset(&d, 0, sizeof(d));
    key(k, 0, 0, 100000, 1, 0, 8192);
    py_drum_hit(&d, pcm, k, 0, 32767, 20, 0, 0);
    run(&d, 300);
    CHECK(out[19] == (int32_t)(((int64_t)pcm[19] * g) >> 15) && out[150] == 0 && !py_drum_active(&d));
    CHECK(abs(out[20 + 64]) < abs((int32_t)(((int64_t)pcm[20 + 64] * g) >> 15)) || pcm[84] == 0);

    /* reverse: from the last frame down */
    memset(&d, 0, sizeof(d));
    key(k, 0, 2000, 2050, 1, 1, 8192);
    py_drum_hit(&d, pcm, k, 0, 32767, 0xfffffffeu, 1, 0);
    run(&d, 60);
    for (ok = 1, i = 0; i < 60; i++)
        ok &= out[i] == (i < 50 ? (int32_t)(((int64_t)pcm[2049 - i] * g) >> 15) : 0);
    CHECK(ok);

    /* loop while held (playmode 2, FULL): wraps until the release, then fades */
    memset(&d, 0, sizeof(d));
    key(k, 0, 3000, 3010, 2, 0, 8192);
    py_drum_hit(&d, pcm, k, 0, 32767, 35, 1, 0);
    run(&d, 300);
    CHECK(out[25] == (int32_t)(((int64_t)pcm[3005] * g) >> 15) && out[200] == 0 && !py_drum_active(&d));

    /* an octave up (TONE 12): every second frame */
    memset(&d, 0, sizeof(d));
    key(k, 0, 4000, 4100, 1, 0, 8192);
    py_drum_hit(&d, pcm, k, 12, 32767, 1, 1, 0);
    run(&d, 60);
    for (ok = 1, i = 0; i < 60; i++)
        ok &= out[i] == (i < 50 ? (int32_t)(((int64_t)pcm[4000 + 2 * i] * g) >> 15) : 0);
    CHECK(ok);

    /* a start delay of 10 samples; the level law: volume 4096 = a quarter */
    memset(&d, 0, sizeof(d));
    key(k, 0, 5000, 5100, 1, 0, 4096);
    py_drum_hit(&d, pcm, k, 0, 32767, 1, 1, 10);
    run(&d, 30);
    CHECK(out[9] == 0 && out[10] == (int32_t)(((int64_t)pcm[5000] * (int32_t)(((int64_t)(((int64_t)32767 * 8192) >> 15) * PY_DRUM_CAL) >> 15)) >> 15));

    /* stealing: 9 hits on 8 voices: the first is gone */
    memset(&d, 0, sizeof(d));
    key(k, 0, 0, 500000, 1, 0, 8192);
    for (i = 0; i < 9; i++)
        py_drum_hit(&d, pcm, k, i, 32767, 1, 1, 0);
    CHECK(py_drum_active(&d) == 8 && d.v[0].inc == py_pow2_q16(8 * 512));

    /* end past the buffer: clamped; start after end: no voice */
    memset(&d, 0, sizeof(d));
    key(k, 0, 529100, 529199, 1, 0, 8192);
    k[2] = 0x7fff; k[6] = 0x7fff;
    py_drum_hit(&d, pcm, k, 0, 32767, 1, 1, 0);
    CHECK(d.v[0].on && d.v[0].start + d.v[0].len <= PY_DRUM_FRAMES);
    memset(&d, 0, sizeof(d));
    key(k, 0, 5000, 4000, 1, 0, 8192);
    py_drum_hit(&d, pcm, k, 0, 32767, 1, 1, 0);
    CHECK(!py_drum_active(&d));

    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
