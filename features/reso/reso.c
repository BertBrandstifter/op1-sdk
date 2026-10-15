/* RESO effect (spec: README.md): four tuned strings that resonate at the notes being played,
 * hosted on TE's CWO effect and picked as "reso" in TE's FX list.
 *
 * An FX is RESO when its stored fx_params 5-8 all have the form 0x2000 | v (8192..16383): v
 * (13 bit) is RESO's page-2 value. Every value stays in 0..32767, the range TE allows for all
 * eight CWO params, so a RESO sound passes TE's patch validator (boot included) and loads on
 * stock firmware as plain CWO. No stock FX reads params 5-8, so the marker never changes CWO.
 *
 * rs_te_mono / rs_te_stereo  CWO process slots: RESO when the stored params carry the marker
 *                (synth and drum FX: mono; master FX: stereo, strings on the mid signal),
 *                otherwise TE's CWO, untouched. Page 1 = the four FX knobs as TE stores and LFO-
 *                modulates them; strings tuned from the synth voices' notes
 * rs_te_fx_fetched / rs_te_master_fetched  FX list: the picked row's type "reso" becomes "cwo"
 *                (its default_params, added by content_sql, carry RESO's defaults + marker)
 * rs_key / rs_knob  synth FX screen of a RESO sound: knob push flips page 1 (TE's knobs) / page
 *                2 (GLIDE OCTAVE DETUNE DRIVE, saved through TE_PATCH_SET_FX)
 * rs_frame       RESO page over CWO's on the synth FX screen */
#include <op1/te246.h>
#include <op1/gfx.h>
#include "reso_core.h"
#include "rs_te246.h"

#define KEY_KNOB_PUSH   59                      /* blue 59 .. orange 62 */
#define STEP_SCALE      10485                   /* ~100 clicks full range */
#define MARK            0x2000
#define MARK_MASK       0xe000

enum { INST_SYNTH = 0, INST_DRUM, INST_MASTER, INSTS };

static struct {
    rs_state s;
    uint8_t on;                                 /* RESO ran last block (else reset on start) */
    uint8_t von[RS_TE_VOICES];                  /* voices the strings were last tuned from */
    uint32_t vinc[RS_TE_VOICES];
    int16_t chord_knobs[3];                     /* CHORD, OCTAVE, DETUNE they were tuned with */
} inst[INSTS];

volatile uint32_t rs_n_mono, rs_n_stereo, rs_n_stereo_marked;   /* call counters (fxdiag) */
volatile uint8_t rs_page;                       /* synth FX screen: 0 page 1, 1 page 2 (bench) */
static volatile uint8_t consumed;

static int marked(const volatile int16_t *f)
{
    return (f[4] & MARK_MASK) == MARK && (f[5] & MARK_MASK) == MARK
           && (f[6] & MARK_MASK) == MARK && (f[7] & MARK_MASK) == MARK;
}

/* page 1 from TE's per-block params (stored + LFO), page 2 from the stored marker slots */
static void params(int16_t *p, const int16_t *te, const volatile int16_t *stored)
{
    int i;

    for (i = 0; i < 4; i++) {
        p[i] = te[i] < 0 ? 0 : te[i];
        p[4 + i] = (int16_t)((stored[4 + i] & 0x1fff) << 2);
    }
}

/* run the strings on one mono block; retune only when the voices or the voicing changed */
static void run(int which, int16_t *buf, const int16_t *p)
{
    int i, changed;

    if (!inst[which].on) {
        rs_reset(&inst[which].s);
        inst[which].on = 1;
        inst[which].chord_knobs[0] = -1;        /* force a tuning */
    }
    changed = inst[which].chord_knobs[0] != p[RS_CHORD] || inst[which].chord_knobs[1] != p[RS_OCTAVE]
              || inst[which].chord_knobs[2] != p[RS_DETUNE];
    for (i = 0; i < RS_TE_VOICES; i++) {
        uint8_t on = RS_TE_VOICE_ON[i];
        uint32_t inc = RS_TE_VOICE_INC[i];

        if (on != inst[which].von[i] || (on && inc != inst[which].vinc[i])) {
            inst[which].von[i] = on;
            inst[which].vinc[i] = inc;
            changed = 1;
        }
    }
    if (changed) {
        rs_chord(&inst[which].s, inst[which].vinc, inst[which].von, RS_TE_VOICES, p);
        inst[which].chord_knobs[0] = p[RS_CHORD];
        inst[which].chord_knobs[1] = p[RS_OCTAVE];
        inst[which].chord_knobs[2] = p[RS_DETUNE];
    }
    rs_process(&inst[which].s, buf, 64, p);
}

void rs_te_mono(void *self, int16_t *buf, int16_t *te)
{
    int32_t pos = RS_TE_CWO_POS(self);
    const volatile int16_t *stored;
    int16_t p[RS_PARAMS];

    rs_n_mono++;
    if (pos == 0)
        stored = RS_TE_SYNTH_FX + (*TE_SYNTH_SLOT & 7) * 8;
    else if (pos == 1)
        stored = RS_TE_DRUM_FX + (*TE_DRUM_SLOT & 7) * 8;
    else
        stored = 0;
    if (!stored || !marked(stored)) {
        if (pos == 0 || pos == 1)
            inst[pos].on = 0;
        RS_TE_CWO_MONO(self, buf, te);          /* stock CWO */
        return;
    }
    params(p, te, stored);
    run(pos, buf, p);
}

void rs_te_stereo(void *self, int16_t *l, int16_t *r, int16_t *te)
{
    int16_t p[RS_PARAMS], m[64];
    int32_t mix;
    int i;

    rs_n_stereo++;
    if (!marked(RS_TE_MASTER_FX)) {
        inst[INST_MASTER].on = 0;
        RS_TE_CWO_STEREO(self, l, r, te);       /* stock CWO */
        return;
    }
    rs_n_stereo_marked++;
    params(p, te, RS_TE_MASTER_FX);
    mix = p[RS_MIX];
    if (!mix)
        return;
    for (i = 0; i < 64; i++)
        m[i] = (int16_t)((l[i] + r[i]) >> 1);   /* strings on the mid signal */
    p[RS_MIX] = 32767;                          /* wet only, mixed into both sides below */
    run(INST_MASTER, m, p);
    for (i = 0; i < 64; i++) {
        int32_t a = l[i] + (((m[i] - l[i]) * mix) >> 15), b = r[i] + (((m[i] - r[i]) * mix) >> 15);

        l[i] = (int16_t)(a > 32767 ? 32767 : a < -32768 ? -32768 : a);
        r[i] = (int16_t)(b > 32767 ? 32767 : b < -32768 ? -32768 : b);
    }
}

/* FX list row "reso" -> "cwo" right after TE reads its type name (TE knows no FX "reso": fatal) */
static void rename(te_string *out)
{
    char *d = (char *)te_str_data(out);

    if (out->size == 4 && d[0] == 'r' && d[1] == 'e' && d[2] == 's' && d[3] == 'o') {
        d[0] = 'c'; d[1] = 'w'; d[2] = 'o'; d[3] = 0;
        out->size = 3;
        rs_page = 0;
    }
}

void rs_te_fx_fetched(void *out, void *db, const char *sql)     { (void)db; (void)sql; rename(out); }
void rs_te_master_fetched(void *out, void *db, const char *sql) { (void)db; (void)sql; rename(out); }

/* ------------------------------------------------------------------ FX browser: current row */

/* TE's FX browsers find the current row from the stored type name ("cwo"); for a RESO sound
 * that row would be cwo's, so the cursor jumps there and a confirm loads cwo's defaults (our
 * marker gone). Answer with our own row name instead. */
static void *rs_cur_patch;

static const volatile int16_t *rs_fx_of_patch(void *patch)
{
    uint32_t a = (uint32_t)patch, s;

    if (a >= 0x01ddd7e8u && a < 0x01ddd7e8u + 16 * 0x38 && (a - 0x01ddd7e8u) % 0x38 == 0) {
        s = (a - 0x01ddd7e8u) / 0x38;           /* synth patches 0..7, then drum patches 0..7 */
        return s < 8 ? RS_TE_SYNTH_FX + s * 8 : RS_TE_DRUM_FX + (s - 8) * 8;
    }
    return 0;
}

static void rs_name_current(te_string *out, const char *name)
{
    static const char host[] = "cwo";
    char *d = (char *)te_str_data(out);
    unsigned int i, n;

    if (out->size != sizeof(host) - 1)
        return;
    for (i = 0; i < sizeof(host) - 1; i++)
        if (d[i] != host[i])
            return;
    for (n = 0; name[n]; n++)
        ;
    if (n > out->cap)
        return;
    for (i = 0; i <= n; i++)
        d[i] = name[i];
    out->size = n;
}

void rs_te_fx_patch(void *patch) { rs_cur_patch = patch; }

void rs_te_fx_current(void *node, void *out)
{
    const volatile int16_t *f = rs_fx_of_patch(rs_cur_patch);

    (void)node;
    if (f && marked(f))
        rs_name_current(out, "reso");
    rs_cur_patch = 0;
}

void rs_te_master_current(void *out)
{
    if (marked(RS_TE_MASTER_FX))
        rs_name_current(out, "reso");
}

/* ------------------------------------------------------------------ KeyHandler thread */

/* where the effect's page is drawn and edited: the synth FX screen of the current sound
 * (*slot = sound), the drum FX screen (*slot = 8 + drum sound) or the mixer's master FX page (*slot = -1); 0 when neither shows a RESO */
static volatile int16_t *synth_fx_reso(int *slot)
{
    volatile int16_t *f;

    *slot = *TE_SYNTH_SLOT & 7;
    if (*TE_EDIT_MODE == 0 && *TE_SYNTH_SUBSCREEN == 2 && RS_TE_SYNTH_FX_TYPE[*slot] == RS_TE_CWO_ID && RS_TE_SYNTH_FX_ON[*slot]) {
        f = RS_TE_SYNTH_FX + *slot * 8;
        return marked(f) ? f : 0;
    }
    *slot = *TE_DRUM_SLOT & 7;                  /* drum FX screen: slot 8..15 */
    if (*TE_EDIT_MODE == TE_EDIT_MODE_DRUM && *TE_DRUM_SUBSCREEN == 2 && TE_DRUM_FX_TYPE[*slot] == RS_TE_CWO_ID
        && TE_DRUM_FX_ON[*slot]) {
        f = TE_DRUM_FX_PARAMS + *slot * 8;
        *slot += 8;
        return marked(f) ? f : 0;
    }
    if (*TE_EDIT_MODE == TE_EDIT_MODE_MIXER && *TE_MIXER_PAGE == TE_MIXER_PAGE_FX
        && *TE_MASTER_FX_TYPE == RS_TE_CWO_ID && *TE_MASTER_FX_ON && !TE_MASTER_TRICK[0] && !TE_MASTER_TRICK[1]
        && marked(TE_MASTER_FX_PARAMS)) {
        *slot = -1;
        return TE_MASTER_FX_PARAMS;
    }
    return 0;
}

static void rs_save(volatile int16_t *f, int slot)
{
    if (slot >= 8)
        RS_TE_PATCH_SET_FX(TE_DRUM_PATCH(slot - 8), (int16_t *)f);
    else if (slot >= 0)
        RS_TE_PATCH_SET_FX(TE_SYNTH_PATCH(slot), (int16_t *)f);
    else
        TE_MASTER_CHANGED(TE_MASTER_STATE);
}

int rs_key(int key, int down)
{
    int b = key - KEY_KNOB_PUSH, slot;

    if (b < 0 || b > 3)
        return 0;
    if (!down) {
        if (consumed & (1 << b)) {
            consumed &= (uint8_t)~(1 << b);
            return 1;
        }
        return 0;
    }
    if (*TE_SHIFT_HELD || !synth_fx_reso(&slot))
        return 0;
    rs_page ^= 1;
    consumed |= (uint8_t)(1 << b);
    return 1;
}

int rs_knob(int knob, int16_t *diff)
{
    int slot, v;
    volatile int16_t *f;

    if ((unsigned int)knob > 3 || !rs_page || !(f = synth_fx_reso(&slot)))
        return 0;                               /* page 1: TE's own CWO knobs */
    if (*TE_SHIFT_HELD)
        return 1;
    v = ((f[4 + knob] & 0x1fff) << 2) + ((*diff * STEP_SCALE) >> 15);
    v = v < 0 ? 0 : v > 32767 ? 32767 : v;
    f[4 + knob] = (int16_t)(MARK | (v >> 2));   /* always 8192..16383: in TE's range */
    rs_save(f, slot);
    return 1;
}

/* ------------------------------------------------------------------ GUI thread */

static const char *const voicings[RS_CHORDS] = {
    "PLAYED", "ROOT", "FIFTH", "MAJ", "MIN", "SUS4", "MAJ7", "MIN7",
};
static const char *const names[RS_PARAMS] = {
    "DECAY", "TONE", "CHORD", "MIX", "GLIDE", "OCTAVE", "DETUNE", "DRIVE",
};
static const char *const notes[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B",
};

/* note number of a string period (16.16 samples at 44.1 kHz) */
static int note_of(uint32_t per_q16)
{
    /* C-1 = 8.1758 Hz -> period 5394 samples; each octave halves it */
    uint32_t per = per_q16 >> 4, ref = 5394u << 12;   /* both in 1/4096 sample */
    int n = 0, s;

    while (per && per <= ref >> 1) {
        per <<= 1;
        n += 12;
    }
    /* within the octave: semitone s where per ~ ref * 2^(-s/12) */
    for (s = 0; s < 12; s++) {
        static const uint16_t r[13] = { 65535, 61858, 58386, 55109, 52016, 49097, 46341, 43740,
                                        41285, 38968, 36781, 34716, 32768 };
        uint32_t mid = (uint32_t)(((uint64_t)ref * ((r[s] + r[s + 1]) / 2)) >> 16);

        if (per > mid)
            break;
    }
    return n + s;
}

void rs_frame(uint16_t *buf)
{
    static const uint16_t col[4] = {
        GFX_RGB(0x69, 0x8e, 0xff), GFX_RGB(0x00, 0xed, 0x95),
        GFX_RGB(0xe6, 0xe6, 0xe6), GFX_RGB(0xff, 0x6a, 0x3d),
    };
    static const uint16_t dim = GFX_RGB(0x5a, 0x58, 0x66), track = GFX_RGB(0x35, 0x32, 0x38);
    int16_t v[RS_PARAMS];
    volatile int16_t *f;
    int slot, i, x, pg = rs_page, who;

    if (!(f = synth_fx_reso(&slot)))
        return;
    who = slot < 0 ? INST_MASTER : slot >= 8 ? INST_DRUM : INST_SYNTH;
    for (i = 0; i < 4; i++) {
        v[i] = f[i] < 0 ? 0 : f[i];
        v[4 + i] = (int16_t)((f[4 + i] & 0x1fff) << 2);
    }
    gfx_begin(buf);
    gfx_rect(0, 0, gfx_w, gfx_h, GFX_BLACK);
    gfx_text(4, 4, "RESO", col[1], 2);
    gfx_text(60, 4, voicings[rs_voicing(v[RS_CHORD])], GFX_WHITE, 2);
    gfx_text(272, 4, pg ? "P2" : "P1", GFX_WHITE, 2);

    /* the four strings: note name + a line that shakes with the string's level */
    for (i = 0; i < RS_STRINGS; i++) {
        int y = 30 + i * 20, n = note_of(inst[who].s.delay_q16[i]), a, k;
        int32_t lv = inst[who].on ? inst[who].s.lp[i] : 0;

        lv = lv < 0 ? -lv : lv;
        a = (int)(lv >> 11);                    /* 0..16 px */
        if (a > 8)
            a = 8;
        x = gfx_text(4, y - 3, notes[n % 12], GFX_WHITE, 2);
        gfx_dec(x, y - 3, n / 12 - 1, dim, 2);
        for (k = 0; k < 256; k += 4)
            gfx_rect(56 + k, y + ((k / 4) & 1 ? a : -a) / 2, 4, 1, col[i]);
    }

    for (i = 0; i < 4; i++) {
        int p = pg * 4 + i, o = (1 - pg) * 4 + i;

        x = 4 + i * 80;
        gfx_text(x, 118, names[p], col[i], 2);
        gfx_rect(x, 132, 72, 6, track);
        gfx_rect(x, 132, v[p] * 72 / 32767, 6, col[i]);
        gfx_text(x, 144, names[o], dim, 1);
        gfx_rect(x + 48, 146, 24, 3, track);
        gfx_rect(x + 48, 146, v[o] * 24 / 32767, 3, dim);
    }
}
