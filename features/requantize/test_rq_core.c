/* Host tests for the requantize core (plain cc; built with its sibling rq_core.c and the
 * quantizer core by tools/test_all.py's convention: see the #include of quant_core.c below). */
#include <stdio.h>
#include <string.h>
#include "rq_core.h"
#include "../quantcore/quant_core.c"             /* single-file host build */

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static qz_state st;
static rq_event in[64], out[64];

static void ev(int i, uint32_t pos, int kind, int key)
{
    memset(&in[i], 0, sizeof(in[i]));
    in[i].pos = pos;
    in[i].ev.kind = (uint8_t)kind;
    in[i].ev.key = (uint8_t)key;
}

#define STEP ((uint64_t)8000 << 16)            /* grid 8000 samples from tape position 0 */

int main(void)
{
    int n, i, ok;

    /* SNAP: late notes go to the next grid point, on-time ones stay, lengths kept */
    ev(0, 16300, QZ_ON, 60);                    /* 300 after 16000: window -> stays */
    ev(1, 20000, QZ_OFF, 60);
    ev(2, 26000, QZ_ON, 62);                    /* 2000 after 24000: -> 32000 (moved 6000) */
    ev(3, 29000, QZ_OFF, 62);                   /* held 3000 -> 35000 */
    n = rq_quantize(in, 4, out, 64, &st, STEP, 0, 1000, 500);
    CHECK(n == 4);
    CHECK(out[0].ev.key == 60 && out[0].pos == 16300 && out[1].pos == 20000);
    CHECK(out[2].ev.key == 62 && out[2].ev.kind == QZ_ON && out[2].pos == 32000);
    CHECK(out[3].ev.key == 62 && out[3].ev.kind == QZ_OFF && out[3].pos == 35000);

    /* QUEUE: a fast run inside one step becomes one note per step; a chord stays together */
    ev(0, 33000, QZ_ON, 60);                    /* in the window of 32000: plays at once */
    ev(1, 34000, QZ_ON, 62);                    /* slot 32000 taken -> 40000 */
    ev(2, 35000, QZ_ON, 64);                    /* -> 48000 */
    ev(3, 35200, QZ_ON, 67);                    /* chord with 64 -> 48000 */
    ev(4, 36000, QZ_OFF, 60);
    ev(5, 36000, QZ_OFF, 62);
    ev(6, 36000, QZ_OFF, 64);
    ev(7, 36000, QZ_OFF, 67);
    n = rq_quantize(in, 8, out, 64, &st, STEP, 1, 1000, 500);
    CHECK(n == 8);
    ok = 1;
    for (i = 0; i < n; i++) {
        if (out[i].ev.kind != QZ_ON)
            continue;
        if (out[i].ev.key == 60) ok &= out[i].pos == 33000;
        if (out[i].ev.key == 62) ok &= out[i].pos == 40000;
        if (out[i].ev.key == 64 || out[i].ev.key == 67) ok &= out[i].pos == 48000;
    }
    CHECK(ok);
    /* every off after its on */
    ok = 1;
    for (i = 0; i < n; i++)
        if (out[i].ev.kind == QZ_OFF) {
            int j, seen = 0;
            for (j = 0; j < i; j++)
                seen |= out[j].ev.kind == QZ_ON && out[j].ev.key == out[i].ev.key;
            ok &= seen;
        }
    CHECK(ok);

    /* unsorted input (e.g. keyboard and MIDI logs merged) is sorted first; output sorted */
    ev(0, 26000, QZ_ON, 70);
    ev(1, 16300, QZ_ON, 71);
    ev(2, 29000, QZ_OFF, 70);
    ev(3, 20000, QZ_OFF, 71);
    n = rq_quantize(in, 4, out, 64, &st, STEP, 0, 1000, 500);
    ok = n == 4;
    for (i = 1; i < n; i++)
        ok &= out[i - 1].pos <= out[i].pos;
    CHECK(ok && out[0].ev.key == 71);

    /* STRICT: notes and chords one grid step after the other, pauses removed; held lengths kept
     * but cut at the next chord's step; the last chord keeps its length */
    ev(0, 16300, QZ_ON, 60);                    /* window of 16000 -> 16000 */
    ev(1, 18000, QZ_OFF, 60);                   /* held 1700 -> 17700 */
    ev(2, 50000, QZ_ON, 62);                    /* long pause -> next step 24000 */
    ev(3, 50200, QZ_ON, 65);                    /* chord with 62 -> 24000 */
    ev(4, 70000, QZ_OFF, 62);                   /* held 20000 -> cut at 32000 */
    ev(5, 70000, QZ_OFF, 65);
    ev(6, 71000, QZ_ON, 62);                    /* same key again -> 32000 (off first) */
    ev(7, 72000, QZ_ON, 64);                    /* 1000 later: own step -> 40000 */
    ev(8, 74000, QZ_OFF, 62);                   /* held 3000 -> 35000 */
    ev(9, 90000, QZ_OFF, 64);                   /* last chord, held 18000 -> 58000 */
    n = rq_quantize(in, 10, out, 64, &st, STEP, RQ_STRICT, 1000, 500);
    CHECK(n == 10);
    {
        static const struct { uint32_t pos; int kind, key; } want[] = {
            { 16000, QZ_ON, 60 }, { 17700, QZ_OFF, 60 }, { 24000, QZ_ON, 62 }, { 24000, QZ_ON, 65 },
            { 32000, QZ_OFF, 62 }, { 32000, QZ_OFF, 65 }, { 32000, QZ_ON, 62 }, { 35000, QZ_OFF, 62 },
            { 40000, QZ_ON, 64 }, { 58000, QZ_OFF, 64 },
        };
        ok = n == 10;
        for (i = 0; ok && i < n; i++)
            ok &= out[i].pos == want[i].pos && out[i].ev.kind == want[i].kind
                  && out[i].ev.key == want[i].key;
        CHECK(ok);
    }
    /* STRICT: a late first note goes to the next grid point; a note-off without its note-on
     * (pressed before the take) stays as played; very short notes keep the minimum length */
    ev(0, 3000, QZ_OFF, 50);
    ev(1, 10000, QZ_ON, 60);                    /* 2000 after 8000: -> 16000 */
    ev(2, 10010, QZ_OFF, 60);                   /* held 10 -> QZ_MIN_LEN */
    n = rq_quantize(in, 3, out, 64, &st, STEP, RQ_STRICT, 1000, 500);
    CHECK(n == 3 && out[0].pos == 3000 && out[0].ev.key == 50 && out[1].pos == 16000
          && out[2].pos == 16000 + QZ_MIN_LEN);

    /* output limit respected */
    CHECK(rq_quantize(in, 4, out, 2, &st, STEP, 0, 1000, 500) == 2);

    printf("%d/%d checks passed\n%s\n", checks - fails, checks, fails ? "FAIL" : "PASS");
    return fails != 0;
}
