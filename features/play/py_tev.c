/* py_tev: TE's own synth engines on PLAY's SYNTH tracks (py_tev.h). */
#include <op1/te246.h>
#include "py_tev.h"
#include "py_synth.h"

#define WORDS       (PY_TEV_REGION / 4)
#define SCR         ((volatile uint32_t *)0xffb00000)   /* TE's voices of the current sound */
#define TEV_INC     ((volatile uint32_t *)0xff801950)   /* TE's per-voice increments (128) */
#define TEV_ENV     ((volatile int16_t *)0xff801d50)    /* TE's per-voice envelope (128) */
#define TEV_SAMPLE  ((void *const volatile *)0x01df6f78) /* [8] SynthSampler sample object per slot */

typedef struct { int32_t delta; void *fn; } te_vslot;  /* C++ vtable slot */
typedef void (*te_vfn)(void *self, void *a, void *b);
enum { VT_RENDER = 2, VT_NOTE_ON = 3, VT_RELEASE = 5 };  /* slot index: fn at vptr + 8 k + 4 */

static uint32_t proto[PY_TEV_PROTOS][WORDS];
static void *proto_voice[PY_TEV_PROTOS][6];
static volatile int8_t proto_type[PY_TEV_PROTOS] = { -1, -1, -1, -1, -1, -1, -1, -1 };

static uint32_t region[PY_TEV_INST][WORDS];
static void *voice[PY_TEV_INST][6];
static int8_t inst_type[PY_TEV_INST] = { -1, -1, -1, -1, -1, -1 };
static int8_t inst_slot[PY_TEV_INST];
static uint32_t te_save[WORDS];
static int16_t kb[8];                           /* the knobs, copied (TE hands its voices a copy) */

int py_tev_type_ok(int type)
{
    switch (type) {
    case TE_INSTR_FM: case TE_INSTR_DIGITAL: case TE_INSTR_STRING: case TE_INSTR_CLUSTER:
    case TE_INSTR_PULSE: case TE_INSTR_SAMPLER: case TE_INSTR_PHASE: case TE_INSTR_DNA:
    case TE_INSTR_DSYNTH: case TE_INSTR_VOLTAGE: case TE_INSTR_ITER:
        return 1;
    }
    return 0;
}

static void copy(volatile uint32_t *d, const volatile uint32_t *s)
{
    int i;

    for (i = 0; i < WORDS; i++)
        d[i] = s[i];
}

static int find_proto(int type)
{
    int p;

    for (p = 0; p < PY_TEV_PROTOS; p++)
        if (proto_type[p] == type)
            return p;
    return -1;
}

static int slot_holds(int type)
{
    int s;

    for (s = 0; s < 8; s++)
        if (TE_SYNTH_TYPE[s] == type)
            return 1;
    return 0;
}

static void build(int p, int type)
{
    void *v[6];
    uint32_t mask;
    int i, slot = 0;

    if (type == TE_INSTR_SAMPLER) {             /* its factory asserts a sample object: one that exists
                                                 * (the voices get their slot's in py_tev_enter) */
        while (slot < 8 && !TEV_SAMPLE[slot])
            slot++;
        if (slot == 8)
            return;
    }
    proto_type[p] = -1;                         /* (the audio side ignores it meanwhile) */
    __asm__ volatile("cli %0;" : "=d"(mask) : : "memory");
    copy(te_save, SCR);
    TE_ENGINE_NEW(type, v, slot);
    copy(proto[p], SCR);
    copy(SCR, te_save);
    __asm__ volatile("sti %0;" : : "d"(mask) : "memory");
    for (i = 0; i < 6; i++)
        proto_voice[p][i] = v[i];
    proto_type[p] = (int8_t)type;
}

void py_tev_main(void)
{
    int s, p;

    for (s = 0; s < 8; s++) {
        int type = TE_SYNTH_TYPE[s];

        if (!py_tev_type_ok(type) || find_proto(type) >= 0)
            continue;
        for (p = 0; p < PY_TEV_PROTOS; p++)     /* an empty one, or one no slot needs any more */
            if (proto_type[p] < 0 || !slot_holds(proto_type[p]))
                break;
        if (p < PY_TEV_PROTOS)
            build(p, type);
    }
}

static int sample_ok(int slot)
{
    void *obj = TEV_SAMPLE[slot];

    return obj && *(void *const volatile *)obj;     /* SampleBuffer +0: m_buffer */
}

int py_tev_why(int slot)
{
    int type = slot >= 0 && slot < 8 ? TE_SYNTH_TYPE[slot] : -1;

    return !py_tev_type_ok(type) ? PY_TEV_NOT
         : type == TE_INSTR_SAMPLER && !sample_ok(slot) ? PY_TEV_NO_SAMPLE : 0;
}

int py_tev_bind(int inst, int slot)
{
    int type, p, i, fresh = 0;

    if (inst <= 0 || inst >= PY_TEV_INST || slot < 0 || slot > 7)
        return 0;
    type = TE_SYNTH_TYPE[slot];
    if (!py_tev_type_ok(type) || (type == TE_INSTR_SAMPLER && !sample_ok(slot)))
        return 0;
    if (inst_type[inst] != type) {
        p = find_proto(type);
        if (p < 0)
            return 0;                           /* the main loop builds it */
        copy(region[inst], proto[p]);
        for (i = 0; i < 6; i++)
            voice[inst][i] = proto_voice[p][i];
        inst_type[inst] = (int8_t)type;
        fresh = 1;
    }
    inst_slot[inst] = (int8_t)slot;
    return 1 + fresh;
}

void py_tev_enter(int inst)
{
    int i;

    copy(te_save, SCR);
    copy(SCR, region[inst]);
    if (inst_type[inst] == TE_INSTR_SAMPLER)    /* the voices play this slot's sample */
        for (i = 0; i < 6; i++)
            ((void **)voice[inst][i])[1] = TEV_SAMPLE[inst_slot[inst]];
}

void py_tev_leave(int inst)
{
    copy(region[inst], SCR);
    copy(SCR, te_save);
}

static void vcall(void *self, int k, void *a, void *b)
{
    const te_vslot *vt = *(te_vslot *const *)self;

    ((te_vfn)vt[k].fn)((char *)self + (int16_t)vt[k].delta, a, b);
}

static void knobs_in(const int16_t *knobs)
{
    int i;

    for (i = 0; i < 8; i++)
        kb[i] = knobs[i];
}

int py_tev_render(int inst, int v, int16_t *out, const int16_t *knobs, const uint32_t *inc,
                  const int16_t *env, int flags)
{
    void *self;
    int i;

    if (inst <= 0 || inst >= PY_TEV_INST || inst_type[inst] < 0 || v < 0 || v > 5)
        return 0;
    self = voice[inst][v];
    knobs_in(knobs);
    if (flags & PY_SYN_FRESH)
        vcall(self, VT_NOTE_ON, kb, 0);
    for (i = 0; i < 128; i++)
        TEV_INC[i] = inc[i];
    if (env)
        for (i = 0; i < 128; i++)
            TEV_ENV[i] = env[i];
    vcall(self, VT_RENDER, out, kb);
    return 1;
}

void py_tev_key_up(int inst, int v, const int16_t *knobs)
{
    if (inst <= 0 || inst >= PY_TEV_INST || inst_type[inst] < 0 || v < 0 || v > 5)
        return;
    knobs_in(knobs);
    vcall(voice[inst][v], VT_RELEASE, kb, 0);
}
