/* Host tests for the CHORD+ core (plain cc, built with cp_core.c by tools/test_all.py). */
#include <stdio.h>
#include <string.h>
#include "cp_core.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint8_t out[CP_MAX_NOTES];

/* chord notes as a comparable string, e.g. "60 64 67" */
static const char *chord(const cp_settings *s, int n, int mods, int prev)
{
    static char b[64];
    int c = cp_chord(s, n, mods, prev, out), i;
    char *p = b;

    *p = 0;
    for (i = 0; i < c; i++)
        p += snprintf(p, (size_t)(b + sizeof(b) - p), i ? " %d" : "%d", out[i]);
    return b;
}

static const char *name(const cp_settings *s, int n, int mods)
{
    static char b[16];

    cp_name(s, n, mods, b);
    return b;
}

int main(void)
{
    cp_settings s = { 0, CP_MAJOR, CP_CLOSE };
    int i, c, prev;

    /* white / black keys, modifier bits */
    CHECK(!cp_is_black(60) && cp_is_black(61) && cp_is_black(70) && !cp_is_black(71));
    CHECK(cp_black_mod(61) == CP_FLIP && cp_black_mod(63) == CP_SEVEN && cp_black_mod(66) == CP_SUS4
          && cp_black_mod(68) == CP_NINE && cp_black_mod(70) == CP_INV1 && cp_black_mod(60) == 0);
    CHECK(cp_chord(&s, 61, 0, 0, out) == 0);

    /* C major: the seven diatonic triads */
    CHECK(!strcmp(chord(&s, 60, 0, 0), "60 64 67") && !strcmp(name(&s, 60, 0), "C"));
    CHECK(!strcmp(chord(&s, 62, 0, 0), "62 65 69") && !strcmp(name(&s, 62, 0), "DMIN"));
    CHECK(!strcmp(chord(&s, 64, 0, 0), "64 67 71") && !strcmp(name(&s, 64, 0), "EMIN"));
    CHECK(!strcmp(chord(&s, 65, 0, 0), "65 69 72") && !strcmp(name(&s, 65, 0), "F"));
    CHECK(!strcmp(chord(&s, 67, 0, 0), "67 71 74") && !strcmp(name(&s, 67, 0), "G"));
    CHECK(!strcmp(chord(&s, 69, 0, 0), "69 72 76") && !strcmp(name(&s, 69, 0), "AMIN"));
    CHECK(!strcmp(chord(&s, 71, 0, 0), "71 74 77") && !strcmp(name(&s, 71, 0), "BDIM"));

    /* modifiers */
    CHECK(!strcmp(chord(&s, 60, CP_FLIP, 0), "60 63 67") && !strcmp(name(&s, 60, CP_FLIP), "CMIN"));
    CHECK(!strcmp(chord(&s, 62, CP_FLIP, 0), "62 66 69") && !strcmp(name(&s, 62, CP_FLIP), "D"));
    CHECK(!strcmp(chord(&s, 60, CP_SEVEN, 0), "60 64 67 71") && !strcmp(name(&s, 60, CP_SEVEN), "CMAJ7"));
    CHECK(!strcmp(chord(&s, 67, CP_SEVEN, 0), "67 71 74 77") && !strcmp(name(&s, 67, CP_SEVEN), "G7"));
    CHECK(!strcmp(chord(&s, 62, CP_SEVEN, 0), "62 65 69 72") && !strcmp(name(&s, 62, CP_SEVEN), "DMIN7"));
    CHECK(!strcmp(name(&s, 71, CP_SEVEN), "BMIN7B5"));
    CHECK(!strcmp(chord(&s, 60, CP_SUS4, 0), "60 65 67") && !strcmp(name(&s, 60, CP_SUS4), "CSUS4"));
    CHECK(!strcmp(chord(&s, 60, CP_NINE, 0), "60 64 67 74") && !strcmp(name(&s, 60, CP_NINE), "CADD9"));
    CHECK(!strcmp(chord(&s, 62, CP_SEVEN | CP_NINE, 0), "62 65 69 72 76") && !strcmp(name(&s, 62, CP_SEVEN | CP_NINE), "DMIN9"));
    CHECK(!strcmp(chord(&s, 60, CP_INV1, 0), "64 67 72"));            /* first inversion */
    CHECK(!strcmp(chord(&s, 60, CP_INV1 | CP_INV2, 0), "67 72 76"));  /* second */

    /* key and scale: D major (white C = D), A minor (white C = A minor) */
    s.key = 2;
    CHECK(!strcmp(chord(&s, 60, 0, 0), "62 66 69") && !strcmp(name(&s, 60, 0), "D"));
    CHECK(!strcmp(name(&s, 62, 0), "EMIN") && !strcmp(name(&s, 71, 0), "C#DIM"));
    s.key = 9;
    s.scale = CP_MINOR;
    CHECK(!strcmp(chord(&s, 60, 0, 0), "69 72 76") && !strcmp(name(&s, 60, 0), "AMIN"));
    CHECK(!strcmp(name(&s, 62, 0), "BDIM") && !strcmp(name(&s, 64, 0), "C") && !strcmp(name(&s, 67, 0), "EMIN"));
    s.scale = CP_HARMONIC;
    CHECK(!strcmp(name(&s, 67, 0), "E") && !strcmp(name(&s, 67, CP_SEVEN), "E7"));   /* V of harmonic minor */
    s.scale = CP_DORIAN;
    s.key = 2;
    CHECK(!strcmp(name(&s, 65, 0), "G"));                             /* dorian IV is major */

    /* voicings */
    s.key = 0;
    s.scale = CP_MAJOR;
    s.voicing = CP_OPEN;
    CHECK(!strcmp(chord(&s, 60, 0, 0), "52 60 67"));                  /* drop 2: E down */
    s.voicing = CP_BASS;
    CHECK(!strcmp(chord(&s, 60, 0, 0), "48 60 64 67"));
    s.voicing = CP_LEAD;
    c = cp_chord(&s, 60, 0, 0, out);
    prev = cp_center(out, c);
    CHECK(!strcmp(chord(&s, 65, 0, prev), "60 65 69"));               /* F after C: 2nd inversion */
    CHECK(!strcmp(chord(&s, 67, 0, prev), "59 62 67"));               /* G after C: 1st inversion, down */
    for (i = 55; i < 73; i++)                                       /* within an octave: near the previous chord */
        if (!cp_is_black(i)) {
            c = cp_chord(&s, i, CP_SEVEN, prev, out);
            CHECK(c == 4 && cp_center(out, c) > prev - 7 * 16 && cp_center(out, c) < prev + 7 * 16);
        }

    /* range: never outside 0..127, ascending, no duplicates */
    s.voicing = CP_BASS;
    for (i = 0; i < 128; i++) {
        int k;

        c = cp_chord(&s, i, CP_SEVEN | CP_NINE, 0, out);
        for (k = 1; k < c; k++)
            CHECK(out[k] > out[k - 1]);
        CHECK(c <= CP_MAX_NOTES);
    }

    printf("%d/%d checks passed\n", checks - fails, checks);
    return fails != 0;
}
