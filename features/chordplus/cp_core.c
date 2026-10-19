/* CHORD+ core (see cp_core.h, README.md). Own code; the chord-keyboard idea (white keys =
 * scale chords, black keys = modifiers) from sloop's sequencer, ideas only. */
#include "cp_core.h"

const char *const cp_key_names[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",
};
const char *const cp_scale_names[CP_SCALES] = {
    "MAJOR", "MINOR", "DORIAN", "MIXOLYDIAN", "HARM MINOR", "PHRYGIAN", "LYDIAN",
};
const char *const cp_voicing_names[CP_VOICINGS] = { "CLOSE", "LEAD", "OPEN", "BASS" };

static const uint8_t scales[CP_SCALES][7] = {
    { 0, 2, 4, 5, 7, 9, 11 },   /* major */
    { 0, 2, 3, 5, 7, 8, 10 },   /* natural minor */
    { 0, 2, 3, 5, 7, 9, 10 },   /* dorian */
    { 0, 2, 4, 5, 7, 9, 10 },   /* mixolydian */
    { 0, 2, 3, 5, 7, 8, 11 },   /* harmonic minor */
    { 0, 1, 3, 5, 7, 8, 10 },   /* phrygian */
    { 0, 2, 4, 6, 7, 9, 11 },   /* lydian */
};

/* pitch class -> white-key degree (C D E F G A B = 0..6), -1 for black keys */
static const int8_t degree_of[12] = { 0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6 };
static const uint8_t black_mod[12] = { 0, CP_FLIP, 0, CP_SEVEN, 0, 0, CP_SUS4, 0, CP_NINE, 0, CP_INV1, 0 };
static const uint16_t mod_at[10] = {            /* F# G# A# C# D#, lower then upper octave */
    CP_SUS4, CP_NINE, CP_INV1, CP_FLIP, CP_SEVEN, CP_VOPEN, CP_VWIDE, CP_INV2, CP_POWER, CP_SUS2,
};

int cp_is_black(int n) { return n >= 0 && degree_of[n % 12] < 0; }
int cp_black_mod(int n) { return n >= 0 ? black_mod[n % 12] : 0; }
int cp_black_mod_at(int place) { return place >= 0 && place < 10 ? mod_at[place] : -1; }

/* semitones from the degree-d root to the chord tone k scale steps above it */
static int interval(const uint8_t *sc, int d, int k)
{
    return sc[(d + k) % 7] + 12 * ((d + k) / 7) - sc[d];
}

/* the chord's intervals above its root (before inversion / voicing), ascending */
static int intervals(const cp_settings *s, int d, int mods, int *iv)
{
    const uint8_t *sc = scales[s->scale < CP_SCALES ? s->scale : 0];
    int third = interval(sc, d, 2), c = 0;

    if (mods & CP_POWER) {                      /* root, fifth, octave */
        iv[c++] = 0;
        iv[c++] = 7;
        iv[c++] = 12;
        return c;
    }
    if (mods & CP_FLIP)
        third = third == 4 ? 3 : third == 3 ? 4 : third;
    if (mods & CP_SUS2)
        third = interval(sc, d, 1);
    if (mods & CP_SUS4)
        third = 5;
    iv[c++] = 0;
    iv[c++] = third;
    iv[c++] = interval(sc, d, 4);
    if (mods & CP_SEVEN)
        iv[c++] = interval(sc, d, 6);
    if (mods & CP_NINE)
        iv[c++] = interval(sc, d, 8);
    return c;
}

static void sort(int *v, int c)
{
    int i, j, t;

    for (i = 1; i < c; i++)
        for (j = i; j > 0 && v[j - 1] > v[j]; j--) {
            t = v[j];
            v[j] = v[j - 1];
            v[j - 1] = t;
        }
}

static void invert(int *v, int c, int times)
{
    while (times-- > 0) {
        v[0] += 12;
        sort(v, c);
    }
}

static int center16(const int *v, int c)
{
    int i, s = 0;

    for (i = 0; i < c; i++)
        s += v[i];
    return c ? s * 16 / c : 0;
}

int cp_center(const uint8_t *notes, int count)
{
    int i, s = 0;

    for (i = 0; i < count; i++)
        s += notes[i];
    return count ? s * 16 / count : 0;
}

static int root_of(const cp_settings *s, int n, int *d)
{
    const uint8_t *sc = scales[s->scale < CP_SCALES ? s->scale : 0];

    *d = degree_of[n % 12];
    return 12 * (n / 12) + (s->key % 12) + sc[*d];
}

int cp_root_pc(const cp_settings *s, int n)
{
    int d;

    return n < 0 || n > 127 || cp_is_black(n) ? -1 : root_of(s, n, &d) % 12;
}

int cp_chord(const cp_settings *s, int n, int mods, int prev, uint8_t *out)
{
    int iv[CP_MAX_NOTES], v[CP_MAX_NOTES], c, i, d, root, out_c = 0;
    int voicing = mods & CP_VOPEN ? CP_OPEN : mods & CP_VWIDE ? CP_VOICINGS : s->voicing;  /* WIDE */

    if (n < 0 || n > 127 || cp_is_black(n))
        return 0;
    root = root_of(s, n, &d);
    c = intervals(s, d, mods, iv);
    for (i = 0; i < c; i++)
        v[i] = root + iv[i];

    if (voicing == CP_LEAD && prev > 0) {
        /* the inversion / octave whose average is nearest the previous chord's */
        int best[CP_MAX_NOTES], bd = 1 << 30, inv, o, k;

        for (inv = 0; inv < c; inv++)
            for (o = -12; o <= 12; o += 12) {
                int t[CP_MAX_NOTES], dist;

                for (k = 0; k < c; k++)
                    t[k] = v[k] + o;
                invert(t, c, inv);
                dist = center16(t, c) - prev;
                dist = dist < 0 ? -dist : dist;
                if (dist < bd) {
                    bd = dist;
                    for (k = 0; k < c; k++)
                        best[k] = t[k];
                }
            }
        for (i = 0; i < c; i++)
            v[i] = best[i];
    } else {
        invert(v, c, ((mods & CP_INV1) ? 1 : 0) + ((mods & CP_INV2) ? 2 : 0));
        if (voicing == CP_OPEN && c >= 3) {
            v[c - 2] -= 12;                     /* drop 2 */
            sort(v, c);
        } else if (voicing == CP_VOICINGS && c >= 3) {
            v[0] -= 12;                         /* WIDE: spread over three octaves */
            v[1] += 12;
            sort(v, c);
        } else if (voicing == CP_BASS && c < CP_MAX_NOTES) {
            v[c++] = root - 12;
            sort(v, c);
        }
    }
    for (i = 0; i < c; i++)
        if (v[i] >= 0 && v[i] <= 127 && (out_c == 0 || out[out_c - 1] != v[i]))
            out[out_c++] = (uint8_t)v[i];
    return out_c;
}

static char *put(char *b, const char *s)
{
    while (*s)
        *b++ = *s++;
    *b = 0;
    return b;
}

void cp_name(const cp_settings *s, int n, int mods, char *buf)
{
    const uint8_t *sc = scales[s->scale < CP_SCALES ? s->scale : 0];
    int iv[CP_MAX_NOTES], d, root, third, fifth, seventh, minor;
    char *b = buf;

    *buf = 0;
    if (n < 0 || n > 127 || cp_is_black(n))
        return;
    root = root_of(s, n, &d);
    if (mods & CP_POWER) {
        b = put(b, cp_key_names[root % 12]);
        put(b, "5");
        return;
    }
    intervals(s, d, mods & ~(CP_SEVEN | CP_NINE), iv);
    third = iv[1];
    fifth = iv[2];
    seventh = interval(sc, d, 6);
    minor = third == 3;
    b = put(b, cp_key_names[root % 12]);
    if (third == 5)
        b = put(b, (mods & CP_SEVEN) ? (mods & CP_NINE ? "9SUS4" : "7SUS4") : (mods & CP_NINE) ? "SUS4ADD9" : "SUS4");
    else if (third == 2 || (third == 1 && (mods & CP_SUS2)))
        b = put(b, (mods & CP_SEVEN) ? "7SUS2" : "SUS2");
    else if (minor && fifth == 6)
        b = put(b, (mods & CP_SEVEN) ? (seventh == 9 ? "DIM7" : "MIN7B5") : "DIM");
    else if (!minor && fifth == 8)
        b = put(b, (mods & CP_SEVEN) ? (seventh == 11 ? "AUGMAJ7" : "AUG7") : "AUG");
    else {
        const char *q = minor ? "MIN" : "";

        b = put(b, q);
        if (mods & CP_SEVEN)
            b = put(b, seventh == 11 ? "MAJ" : "");
        if (mods & CP_SEVEN)
            b = put(b, (mods & CP_NINE) ? "9" : "7");
        else if (mods & CP_NINE)
            b = put(b, "ADD9");
    }
}
