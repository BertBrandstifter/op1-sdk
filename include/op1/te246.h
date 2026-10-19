/* TE OP-1 firmware R246: known addresses (image SHA-256 c5315218...13a9).
 * Calling convention (TE's VisualDSP++ code and bfin-elf-gcc agree): args in R0, R1, R2 then stack
 * (4th arg at [SP+12] after the caller reserves 12 bytes of outgoing arg space), result in R0,
 * R4-R7/P3-P5/FP callee-saved, L0-L3 must be 0. Assembly that calls C/TE functions must do
 * SP += -12 before CALL and SP += 12 after.
 *
 * Signatures below were verified against the R246 disassembly; addresses come from
 * analysis/stock_sdram.dis. "asserts" = on bad input TE calls TE_ASSERT_PRINT with the quoted
 * message. After boot that is FATAL: interrupts off, audio stopped, message + stack frames drawn
 * on screen, then an endless IDLE loop (power-cycle to recover). Only pass known-good values. */
#ifndef OP1_TE246_H
#define OP1_TE246_H

#include <stdint.h>

/* ------------------------------------------------------------------ types */

/* VisualDSP++ Dinkumware std::string, 28 bytes: 16-byte SSO buffer at +4,
 * heap pointer at +4 when cap >= 16, size at +0x14, capacity at +0x18. */
typedef struct te_string {
    uint32_t reserved;          /* +0x00 */
    union {
        char sso[16];           /* +0x04 inline buffer, used when cap < 16 */
        const char *heap;       /* +0x04 heap pointer, used when cap >= 16 */
    } bx;
    uint32_t size;              /* +0x14 */
    uint32_t cap;               /* +0x18 */
} te_string;

static inline const char *te_str_data(const te_string *s)
{
    return s->cap >= 16 ? s->bx.heap : s->bx.sso;
}

/* {const char *name, int id}, the format of every TE name/id table. */
typedef struct te_name_id {
    const char *name;
    int32_t id;
} te_name_id;

/* VisualDSP++ Dinkumware std::vector<unsigned char>: begin at +4, end at +8.
 * TE passes MIDI messages as one of these (VoiceCtl.cpp: message.at(1) etc). */
typedef struct te_bytes {
    uint32_t reserved;          /* +0x00 */
    const uint8_t *begin;       /* +0x04 */
    const uint8_t *end;         /* +0x08 */
    const uint8_t *cap;         /* +0x0c */
} te_bytes;

static inline uint32_t te_bytes_size(const te_bytes *b)
{
    return (uint32_t)(b->end - b->begin);
}

/* ------------------------------------------------------------------ enums */

enum {                              /* fx_types.cpp */
    TE_FX_DELAY = 0, TE_FX_GRID, TE_FX_PUNCH, TE_FX_PHONE,
    TE_FX_SPRING, TE_FX_FILTER, TE_FX_NITRO, TE_FX_CWO
};

enum {                              /* SeqIds.cpp; NOTE table order != id order */
    TE_SEQ_PATTERN = 0, TE_SEQ_ENDLESS = 1, TE_SEQ_TOMBOLA = 2,
    TE_SEQ_FINGER = 3, TE_SEQ_SKETCH = 4, TE_SEQ_ARPEGGIO = 5
};

/* TE_SEQ_TABLE is stored in lookup order: pattern(0), tombola(2), endless(1),
 * finger(3), sketch(4), arpeggio(5). */

enum {                              /* mode ids start at 1 */
    TE_MODE_SYNTH = 1, TE_MODE_DRUM, TE_MODE_TAPE, TE_MODE_MIXER, TE_MODE_TEMPO,
    TE_MODE_MIC, TE_MODE_ALBUM, TE_MODE_SEQ, TE_MODE_BROWSER
};

enum {                              /* instr_types.cpp, filled at boot in this order */
    TE_INSTR_FM = 0, TE_INSTR_DIGITAL, TE_INSTR_DRWAVE, TE_INSTR_STRING,
    TE_INSTR_CLUSTER, TE_INSTR_PULSE, TE_INSTR_SAMPLER, TE_INSTR_PHASE,
    TE_INSTR_DRUM, TE_INSTR_DBOX, TE_INSTR_LS, TE_INSTR_DNA, TE_INSTR_DSYNTH,
    TE_INSTR_VOLTAGE, TE_INSTR_ITER, TE_INSTR_COUNT
};

enum {                              /* MIDI status bytes the dispatcher routes */
    TE_MIDI_NOTE_OFF = 0x80, TE_MIDI_NOTE_ON = 0x90,
    TE_MIDI_CC = 0xb0, TE_MIDI_PITCHBEND = 0xe0
};

/* ------------------------------------------------------------------- data */

#define TE_BOOT_TEXT        ((char *)0x018f738a)    /* "R. 00246" under the TE logo, 8 bytes, no NUL */
#define TE_FX_TABLE         ((const te_name_id *)0x018dd810)    /* x8, static */
#define TE_SEQ_TABLE        ((const te_name_id *)0x018fa634)    /* x6, static, lookup order (see above) */
#define TE_MODE_TABLE       ((const te_name_id *)0x018de8ec)    /* x9, static, ids 1..9 */
#define TE_INSTR_TABLE      ((const te_name_id *)0x01dd4d98)    /* x15, BSS, filled at boot; valid only after boot */
#define TE_INSTR_NAMES      ((const char *)0x018df7bc)  /* packed NUL-terminated names, fm ... iter */

/* ---------------------------------------------------------------- functions
 * Cast to a function pointer type and call, e.g. TE_SEQIDS_NAME(3) -> "finger". */

/* shared/SeqIds.cpp */
#define TE_SEQIDS_NAME      ((const char *(*)(int id))0x01a422ec)               /* asserts "unknown sequencer: %i", returns 0 */
#define TE_SEQIDS_LOOKUP    ((int (*)(const te_string *name))0x01a42344)        /* asserts "unknown sequencer name", returns 0 */

/* shared/fx_types.cpp */
#define TE_FX_NAME          ((const char *(*)(int id))0x019aad38)               /* asserts "unknown fx", returns 0 */
#define TE_FX_LOOKUP        ((int (*)(const te_string *name))0x019aae44)        /* asserts "unknown fx name", returns 0 */

/* shared/instr_types.cpp */
#define TE_INSTR_TYPE_FROM_NAME ((int (*)(const char *name))0x019b9810)         /* asserts "not found %s", returns 0 */
#define TE_IS_INSTRUMENT_TYPE   ((int (*)(const char *name))0x019b97c4)         /* 1 if name is a known instrument type */

/* string helpers used by the tables above */
#define TE_STREQ            ((int (*)(const char *cstr, const te_string *s))0x019b975c)   /* 1 if equal */
#define TE_STRNCMP          ((int (*)(const char *a, const char *b, uint32_t n))0x01a9f37c)

/* the assertion printer all of the above call: fmt + up to 2 string args (extra args ignored).
 * No-op while TE_ASSERT_ARMED is 0 (early boot); main sets it to 1 at 0x019f01ae, after that it
 * halts the device (see top). Do not call it to print debug text: use op1/gfx.h instead. */
#define TE_ASSERT_ARMED     ((volatile uint8_t *)0xff9002f4)
#define TE_ASSERT_PRINT     ((void (*)(const char *fmt, const char *a, const char *b))0x01919aa8)

/* dsp/VoiceCtl.cpp MIDI path. The dispatcher (0x01a85c20, entry bytes 00 e8 04 00 =
 * LINK 0x10) gets every message as te_bytes* in R0 and routes status&0xf0:
 * 0x80 note off, 0x90 -> note-on handler, 0xb0 -> CC handler, 0xe0 -> pitch-bend handler.
 * It is registered as a callback pointer at 0x01aadb24 during boot (0x019e2d62) and
 * called from code we cannot see (L1 is encrypted); hook its entry, not its callers. */
#define TE_MIDI_DISPATCH    ((void (*)(const te_bytes *msg))0x01a85c20)
#define TE_MIDI_HANDLE_NOTE_ON ((void (*)(const te_bytes *msg))0x01a859e4)
#define TE_MIDI_HANDLE_CC   ((void (*)(const te_bytes *msg))0x01a84f8c) /* VoiceCtl.cpp:635; op1r hooks its call site 0x01a85c8a */
#define TE_MIDI_HANDLE_BEND ((void (*)(const te_bytes *msg))0x01a84efa)
#define TE_MIDI_CC_TABLE    ((volatile uint32_t *)0x01dff138)   /* [128] last CC value per CC number (BSS) */

/* ---------------------------------------------------------------- runtime
 * TE heap (te_new.cpp, 8.25 MB, mutex-protected, so callable from any thread; out of memory is
 * a fatal assert). malloc/free use a separate VisualDSP heap. */
#define TE_NEW              ((void *(*)(uint32_t size))0x01a5e3b0)
#define TE_DELETE           ((void (*)(void *p))0x01a5e310)             /* NULL ok */
#define TE_MALLOC           ((void *(*)(uint32_t size))0x01a9f348)
#define TE_FREE             ((void (*)(void *p))0x01aa0040)
/* VisualDSP heaps (heap table 0x018e628c): heap 0 = 0x00000100 + 16 MB (malloc), heap 1 =
 * 0x03040000 + 15.75 MB: TE's sample memory (SampleBuffer.cpp sample buffers, tar.cpp updates).
 * heap_malloc returns 0 when the heap has no room (TE's callers assert, the allocator does not).
 * Thread context (it locks), never the audio thread. Measured with features/meminfo. */
#define TE_HEAP_MALLOC      ((void *(*)(int heap, uint32_t size))0x01aa0318)
#define TE_HEAP_SAMPLES     1
#define TE_MEMCPY           ((void *(*)(void *dst, const void *src, uint32_t n))0x01aa6e10)
#define TE_MEMMOVE          ((void *(*)(void *dst, const void *src, uint32_t n))0x01aa6e64)
#define TE_MEMSET           ((void *(*)(void *dst, int c, uint32_t n))0x01a9f3d8)
#define TE_SNPRINTF         ((int (*)(char *buf, uint32_t n, const char *fmt, ...))0x01a4ccd8)
/* there is no out-of-line strlen in the image (always inlined) */

/* std::string: build one to pass to TE APIs, destroy it afterwards (frees the heap buffer). */
#define TE_STRING_INIT      ((te_string *(*)(te_string *s, const char *cstr))0x019baac6)   /* ctor */
#define TE_STRING_ASSIGN    ((te_string *(*)(te_string *s, const char *p, uint32_t n))0x019ba9c0)
#define TE_STRING_DESTROY   ((void (*)(te_string *s))0x01a17bd8)

/* yaffs2 files (paths "/yaffs2/...", e.g. "/yaffs2/content/op1_factory.db"), as SampleBuffer.cpp
 * reads a sample (0x01a3492a): open(path, 0, 0x100) = read only, returns a descriptor or -1; read
 * returns the bytes read (0 at the end, < 0 on an error); close. Thread context (yaffs locks),
 * never the audio thread: a read can take milliseconds. */
#define TE_FILE_OPEN        ((int (*)(const char *path, int oflag, int mode))0x01a997f4)
#define TE_FILE_READ        ((int (*)(int fd, void *buf, uint32_t n))0x01a99c90)
#define TE_FILE_CLOSE       ((int (*)(int fd))0x01a99908)
#define TE_FILE_RDONLY      0
#define TE_FILE_MODE        0x100

/* data cache: SDRAM is cached; DMA (screen, audio) only sees data after a flush */
#define TE_DCACHE_FLUSH     ((void (*)(void *start, void *end, int invalidate))0x01aa185c)

/* ---------------------------------------------------------------- database (static RE 2026-10-15)
 * TE keeps its state in SQLite files on yaffs2 (db2/db2.cpp): user/op1.db (slots, sequencers: e.g.
 * EtchASketchData saves "UPDATE easseq SET data='%s' ..." after "CREATE TABLE IF NOT EXISTS easseq"),
 * user/tape.db, content/op1_factory.db. Open at boot (0x0196f6ca), handles at 0x01bab720 (op1.db),
 * +4 op1_factory.db, +8 kerntable.db, +0xc tape.db. One VDK semaphore guards every query.
 * DANGER: TE's helpers assert on any SQLite error (fatal after boot), and they format the SQL
 * printf-style into one static 8 KB buffer (0x01bab8e0): pass only SQL without '%', under 8 KB,
 * on tables that exist. Call from a thread (main loop), never from the audio / sequencer path. */
#define TE_DB_OP1           ((void *(*)(void))0x0196dd44)       /* op1.db handle (0 before boot opens it) */
#define TE_DB_EXEC          ((void (*)(void *db, const char *sql))0x0196f950)   /* locks; prepare + step,
                                                                 * asserts unless SQLITE_DONE */
typedef struct te_db_query { void *db; void *stmt; } te_db_query;  /* db2 statement holder */
#define TE_DB_PREPARE       ((void (*)(te_db_query *q, const char *fmt, ...))0x0196e2f0)   /* no lock;
                                                                 * asserts if prepare fails */
#define TE_SQLITE_STEP      ((int (*)(void *stmt))0x01a7bb20)   /* 100 ROW, 101 DONE */
#define TE_SQLITE_COLUMN_TEXT ((const char *(*)(void *stmt, int col))0x01a7c6b8)
#define TE_SQLITE_FINALIZE  ((int (*)(void *stmt))0x01a7b202)   /* 0 ok */
#define TE_DB_LOCK          ((void *volatile *)0x01bab718)      /* the semaphore (TE creates it on first use) */
#define TE_SEM_PEND         ((int (*)(void *sem, int timeout))0xffa0b6e8)   /* VDK, timeout 0 = forever */
#define TE_SEM_POST         ((void (*)(void *sem))0xffa0b96c)

/* Hook points for app lifecycle (thread context; the targets are empty 2-byte RTS functions,
 * so use call-site hooks, never entry hooks):
 *   TE_SITE_BOOT_DONE   main thread, once, right before the main loop  -> "init" work
 *   TE_SITE_MAIN_TICK   main thread, every main-loop pass (after VDK Sleep(8) + controller work)
 *   TE_SITE_FRAME       GUI thread, TE's frame render (R0 = back buffer); hook with "after": true
 *                       and draw with op1/gfx.h before TE flushes + presents it
 *   TE_SITE_SEQ_TICK    sequencer thread, woken by the audio interrupt (high rate) */
#define TE_SITE_BOOT_DONE   0x019f09d6      /* CALL 0x019d2d1e */
#define TE_SITE_MAIN_TICK   0x019f0a6a      /* CALL 0x019d2d1c */
#define TE_SITE_FRAME       0x019ab8b2      /* CALL 0x019b2674 render(buf) */
#define TE_SITE_SEQ_TICK    0x01a431aa      /* CALL 0x01a0d2c0 */
#define TE_FRAME_PRESENT    ((void (*)(uint16_t *buf))0x01a2fb18)      /* repoints DMA to buf */

/* Keys and knobs (KeyHandler.cpp, Keys.cpp). The raw scan (TWI GPIO expanders 0x20/0x27,
 * bf/gpio.cpp) runs in encrypted L1 and pushes events into a queue; the KeyHandler VDK thread
 * (0x019c2cc0, thread context, not an ISR) drains it and calls the two functions below.
 * Both start with LINK 0 (entry bytes 00 e8 00 00) and have exactly one SDRAM call site, so
 * either an entry hook or a call-site hook works. Hook with "filter": true to consume events.
 *   on_key(key, down)     key = id 0..57 (TE_KEY_*), down = 0 up / nonzero down.
 *                         Drops everything while TE_KEYS_ENABLED is 0; then routes to the
 *                         current mode's controller (vtable +0x10 down, +0x18 shift+down,
 *                         +0x20 up) with global fallbacks.
 *   on_knob(knob, diff)   knob = 0..3 (4..7 is applied inside when shift is held),
 *                         *diff = signed step, (raw count & ~1) << 9, i.e. +-1024 per
 *                         2 raw counts (one detent, unconfirmed on device).
 * Both hooks device-verified (examples/key_probe, 2026-10-11). */
#define TE_ON_KEY           ((void (*)(int key, int down))0x019b6c74)           /* call site 0x019c2d3e */
#define TE_ON_KNOB          ((void (*)(int knob, int16_t *diff))0x019b6c0c)     /* call site 0x019c2e94 */
#define TE_KEY_PUSH         ((void (*)(uint16_t ev))0x019c2f24)     /* inject: ev = key | down << 8 */
#define TE_KEY_WAKE         ((void (*)(void))0x019c2f04)    /* post the KeyHandler semaphore: call after
                                                             * TE_KEY_PUSH, or the key waits for the next
                                                             * real key press */
#define TE_KNOB_ADD         ((void (*)(int knob, int delta))0x019c3044)

#define TE_KEYS_ENABLED     ((volatile uint8_t *)0x01dd4d94)    /* on_key gate: 0 = ignore all keys */
#define TE_SHIFT_HELD       ((volatile uint8_t *)0x01dd7f78)    /* shift key state */
#define TE_KNOB_TOUCHED     ((volatile uint8_t *)0x01dd7f74)    /* [4] set by on_knob */
#define TE_KNOB_ACC         ((volatile int32_t *)0x01dd5250)    /* [4] raw accumulators, not yet sent */
#define TE_NOTE_KEYS_ON     ((volatile uint8_t *)0x01aad488)    /* [24] by note index; global fallback path only */

enum {                              /* key ids passed to on_key; device-verified with key_probe 2026-10-11
                                     * (sheet: "op-1 key mapping/"), unless marked */
    TE_KEY_SPEECH = 0,              /* speech bubble (top row, left of the knobs) */
    TE_KEY_METRONOME = 1,           /* key-down table 0x018ddcac: id 1 -> tempo mode (0x019ac564) */
    TE_KEY_SYNTH = 2, TE_KEY_DRUM = 3, TE_KEY_TAPE = 4, TE_KEY_MIXER = 5,
    TE_KEY_T1 = 6, TE_KEY_T2 = 7, TE_KEY_T3 = 8, TE_KEY_T4 = 9,
    TE_KEY_CUT = 10, TE_KEY_INSERT = 11, TE_KEY_JOIN = 12,
    TE_KEY_SOUND1 = 45, TE_KEY_SOUND2 = 46, TE_KEY_SOUND3 = 47, TE_KEY_SOUND4 = 16,  /* keys 1-8 right */
    TE_KEY_SOUND5 = 17, TE_KEY_SOUND6 = 18, TE_KEY_SOUND7 = 19, TE_KEY_SOUND8 = 20,  /* of T1-T4 */
    TE_KEY_AUTO = 21,
    TE_KEY_REC = 33, TE_KEY_PLAY = 34, TE_KEY_STOP = 35,
    TE_KEY_BACK = 36, TE_KEY_FORWARD = 37, TE_KEY_SHIFT = 38,     /* re-checked on device */
    TE_KEY_MIC = 43, TE_KEY_COM = 44,
    TE_KEY_ALBUM = 44,              /* the same key: on the device it opens the album (edit mode 6) */
    TE_KEY_BLUE_PUSH = 59, TE_KEY_GREEN_PUSH = 60, TE_KEY_WHITE_PUSH = 61, TE_KEY_ORANGE_PUSH = 62,
    TE_KEY_MAX = 62
    /* 56/57: virtual keys injected by OP_Main. The volume knob sends no key event. */
};

/* note index 0..23 (low F upwards) -> key id, from 0x019c3322. TE plays note idx + 53 + 12*octave. */
#define TE_NOTE_KEY_IDS { 39, 13, 40, 14, 41, 15, 42, 28, 32, 29, 22, 30, \
                          31, 23, 48, 24, 49, 25, 50, 51, 26, 52, 27, 53 }

/* Tempo + transport (tempo.cpp, TapeController). Sample rate 44.1 kHz (TE's tempo maths use
 * 2646000 = 44100 * 60). The Seq thread runs TE_SITE_SEQ_TICK once per 64-frame audio block. */
#define TE_SAMPLE_RATE      44100
#define TE_BPM              ((volatile float *)0x018c1878)     /* 40..220; set via 0x01a5e9a4 (asserts) */
#define TE_GET_BPM          ((float (*)(void))0x01a5e7a0)
#define TE_TAPE_STATE       ((volatile int32_t *)0xff802960)   /* 0 stopped, 1 recording, 2 playing, 3 cue */
#define TE_TAPE_MOVING      ((volatile int32_t *)0xff802968)   /* != 0 while moving */
#define TE_TAPE_POS         ((volatile int32_t *)0xff802974)   /* tape position, samples * 64 */
#define TE_TAPE_MODE        ((volatile int32_t *)0x018c17d4)   /* key-driven: 0 idle, 3/6 rec armed, 4 rec,
                                                                 * 5 punch-in, 9 play held, 10 playing */
#define TE_TAPE_TRACK       ((volatile int32_t *)0xff802964)   /* selected track 0..3 (T1-T4 select it) */
#define TE_TAPE_MUTE        ((volatile uint8_t *)0xff8010a0)   /* [4] mute flags (shift + T toggles) */
#define TE_TAPE_GAINS_UPDATE ((void (*)(void))0x01967ed0)     /* rebuild track gains after a mute change */
#define TE_TAPE_SEEK        ((void (*)(int32_t pos))0x01a55024)  /* samples, 0..15876000 (asserts!);
                                                                 * only while stopped; applied async */
#define TE_TAPE_MAX         15876000                            /* 6 minutes */
/* TE R246 has no tape undo: recording overwrites in place. */
#define TE_METRO_TO_BEAT    ((volatile int32_t *)0x01aadadc)   /* samples to next beat * 2^14, counts down */
#define TE_METRO_BEAT_LEN   ((volatile int32_t *)0x01aadad8)   /* samples per beat * 2^14 */
#define TE_PLAYMODE         ((volatile int32_t *)0x01de4c40)   /* 0 synth, 1 drum, 2 game */
#define TE_PLAYMODE_TARGET  ((volatile int32_t *)0xff801038)   /* != TE_PLAYMODE while switching */
#define TE_DRUM_SLOT        ((volatile int32_t *)0xff8016d8)    /* current drum sound 0..7 */
#define TE_SYNTH_OCTAVE     ((volatile int32_t *)0x01dfe4e0)    /* [8] octave per synth sound (finger
                                                                 * player adds 12 x it; RE 2026-10-17) */
#define TE_DRUM_OCTAVE      ((volatile int32_t *)0x01dfe500)    /* [8] octave per drum sound, -4..4;
                                                                 * drum note-on adds 12 * this + extra */
#define TE_DRUM_SHIFT_NOTE  ((volatile int32_t *)0x018c2180)    /* key idx held with shift, else -1;
                                                                 * read by the drum note-on */

/* Edit mode (ModeSwitcher.cpp): which screen/controller has the keys and knobs. Not the same
 * numbering as TE_MODE_*. Tempo screen = 4 (TempoController 0x01b60d90, vtable 0x01911ba4):
 * knobs blue BPM, green sync mode, white tape speed, orange metronome level, shift+blue fine BPM,
 * shift+green sync type; shift+white and shift+orange are unused there. */
#define TE_EDIT_MODE        ((volatile int32_t *)0xff907d58)
#define TE_EDIT_MODE_TEMPO  4
#define TE_EDIT_MODE_TAPE   2                                   /* device-checked (diskinfo MODE) */
#define TE_EDIT_MODE_ALBUM  6                                   /* device-checked: album key 44 */
#define TE_EDIT_MODE_SEQ    7                                   /* sequencer screen (sequencer key 21,
                                                                 * 0x019ac724; RE 2026-10-17) */
#define TE_SYNTH_SEQ_TYPE   ((volatile int32_t *)0x01ab3144)    /* TE_SEQ_* of the synth sequencer */
#define TE_DRUM_SEQ_TYPE    ((volatile int32_t *)0x01ab3148)    /* TE_SEQ_* of the drum sequencer
                                                                 * (synth: 0x01ab3144), PlayerManager */
#define TE_EDIT_MODE_DRUM   1                                   /* DrumController 0x01aabaa8, vtable 0x0190fa64
                                                                 * (ctor 0x01977774 -> id 1; RE 2026-10-14) */

/* Drum sounds (DrumController.cpp, RE 2026-10-14): as the synth ones, per drum slot (TE_DRUM_SLOT). */
#define TE_DRUM_SUBSCREEN   ((volatile int32_t *)0x01aab9f4)    /* 0 engine, 1 envelope, 2 fx, 3 lfo */
#define TE_DRUM_FX_TYPE     ((volatile int32_t *)0xff801754)    /* [8] TE_FX_* per drum sound */
#define TE_DRUM_FX_ON       ((volatile uint8_t *)0x01aaac30)    /* [8] */
#define TE_DRUM_FX_PARAMS   ((volatile int16_t *)0xff8015c4)    /* [8][8] */
#define TE_DRUM_PATCH(s)    ((void *)(0x01ddd9a8 + (s) * 0x38))  /* patch object per drum sound (after the
                                                                 * 8 synth ones); 0x019ff664 saves its FX */
#define TE_EDIT_MODE_MIXER  3                                   /* MasterController 0x01aad608 */

/* Mixer (MasterController, vtable 0x01910aac; static RE 2026-10-12). Pages via T1-T4. The FX page
 * draws the same per-effect screen as the synth FX page, from the master fx params; TE's knobs
 * edit params 1-4 only (range table 0x01de5c50), knob pushes 59-62 are unused there. */
#define TE_MIXER_PAGE       ((volatile int32_t *)0x01aad5f0)    /* 0 levels, 1 EQ, 2 FX, 3 compressor */
#define TE_MIXER_PAGE_FX    2
#define TE_MASTER_FX_TYPE   ((volatile int32_t *)0xff801090)    /* TE_FX_* of the master FX */
#define TE_MASTER_FX_ON     ((volatile uint8_t *)0x01aaac40)
#define TE_MASTER_FX_PARAMS ((volatile int16_t *)0xff8016c4)    /* [8], stored + saved */
#define TE_MASTER_TRICK     ((volatile uint8_t *)0x01aad5dc)    /* [2]: nonzero = edits go to a
                                                                 * scratch copy (trick slots) */
#define TE_MASTER_CHANGED   ((void (*)(void *))0x0193ef3c)      /* (TE_MASTER_STATE): mark dirty ->
                                                                 * TE saves the master row */
#define TE_MASTER_STATE     ((void *)0x01aad610)
#define TE_CONTROLLER       ((void *volatile *)0x01dd8bc8)       /* active controller object */

/* The 16-byte note record VoiceCtl takes (built by 0x01a85014 on the caller's stack). */
typedef struct te_note {
    int32_t note;               /* MIDI note; synth: octave already applied, drum: added on play */
    int32_t vel;                /* -1 = 127 */
    uint8_t midi_out;           /* 1 = echo to MIDI out (keyboard), 0 = came from MIDI */
    uint8_t pad[3];
    int32_t extra;              /* 0 for keyboard and MIDI */
} te_note;

/* Note-on/off call sites (all tail calls, return value unused):
 *   keyboard  0x019ac290 CALL 0x01a85750 (R1 = 0)   0x019ac3a6 CALL 0x01a85840
 *   MIDI in   0x01a85bc6 CALL 0x01a85750            0x01a8599e CALL 0x01a85840 */

/* VoiceCtl note on/off: reached by the piano keys, MIDI and the sequencers (~60 call sites).
 * Note: the MIDI note-on handler 0x01a859e4 starts with a 2-byte instruction (P1 = R0);
 * do not entry-hook it as is. */
#define TE_VOICE_NOTE_ON    ((void (*)(te_note *note, int extra))0x01a85750)    /* entry 00 e8 04 00 */
#define TE_VOICE_NOTE_OFF   ((void (*)(te_note *note))0x01a85840)               /* entry 00 e8 08 00 */

/* Audio (bf/audio.cpp init 0x0191ab40). SPORT0 I2S, codec is clock master, 32-bit slots.
 * DMA moves 128 words = 64 stereo frames per block; the IVG11 handler (L1) calls the block
 * callback pointer below as cb(tx_out, rx_in). Default 0xffa04804 (L1); 0x0191a7cc sets it.
 * Wrap it from an init hook: save the old value, store yours, call the old one first.
 * Sample rate probably 44.1 kHz (unconfirmed). */
#define TE_AUDIO_BLOCK_CB   ((void (**)(int32_t *tx, const int32_t *rx))0xff80036c)
#define TE_AUDIO_FRAMES     64

/* FX and synth engines (dsp/producer_factory.cpp). C++ vtables: each slot is {int32 this_delta,
 * fn}; a "slot address" below is the address of fn (all deltas are 0). Redirect one with a
 * "pointers" entry in project.json, as op1r does. Audio is int16 fract16, one buffer per channel.
 *   FX   +0x0c dtor(this, flags)   +0x14 process   +0x1c init(this, pos)
 *        mono:   process(this, int16 *buf, int16 *params), in place, 64 samples
 *        stereo: process(this, int16 *l, int16 *r, int16 *params)  (params = 4th arg, stack)
 *   voice +0x14 render(this, int16 *out, int16 *params) 128 samples  +0x1c note_on(this, params)
 *         +0x2c release(this) ; 6 voices per engine (drum/dbox 12), objects at 0xffb00000 + i*size
 * FX factories reject ids >= 8; op1r adds effects by multiplexing the filter FX on a param
 * (W[params + 8] - 0x5300 > 0 -> its own table) and engines by multiplexing the iter voice. */
#define TE_FX_STEREO_NEW    ((void *(*)(int fx_id))0x01a1b5f0)      /* builds at 0xffb00790; entry 41 60 00 e8 04 00 */
#define TE_FX_MONO_NEW      ((void *(*)(int fx_id, int pos))0x01a1b6d0)     /* builds at 0xffb00600 */
#define TE_ENGINE_NEW       ((void (*)(int type, void **voices, int arg))0x01a1b7d4)

typedef void (*te_fx_mono_process)(void *self, int16_t *buf, int16_t *params);
#define TE_FX_FILTER_MONO_PROCESS_SLOT   0x01910ca8     /* stock value 0x019e7934 */
#define TE_FX_FILTER_STEREO_PROCESS_SLOT 0x01910cc8     /* stock value 0x019e7ee4 */
#define TE_FX_FILTER_MONO_PROCESS   ((te_fx_mono_process)0x019e7934)
#define TE_FX_FILTER_STEREO_PROCESS ((void (*)(void *, int16_t *, int16_t *, int16_t *))0x019e7ee4)
/* other FX process slots (mono / stereo): delay 0190fb80/0190fba0, grid 01910218/01910238,
 * punch 01910c30/01910c50, phone 01911308/01911328, spring 019113d0/019113f0,
 * nitro 01910028/01910048, cwo 0190f588/0190f5a8. Iter voice slots 019103e0..019103f8. */

/* Synth sounds (main/ctls/synth_controller.cpp, db2/patch.cpp). 8 synth sound slots; each has an
 * engine type (TE_INSTR_*) and 8 int16 knob values ("knobs" in the preset JSON). The audio code
 * copies a sound's knob block into its voices every block; voices get it as render's params.
 * Knob slots 5-8 (index 4..7) are only used by dsynth and sampler (Ranges table 0x01de5890 +
 * type*64 + id*8 = {int16 min, max, scale, kind}); other engines ignore them. */
#define TE_SYNTH_SLOT       ((volatile int32_t *)0xff8016d4)    /* current synth sound 0..7 */
#define TE_SYNTH_TYPE       ((volatile int32_t *)0xff8016e4)    /* [8] engine type per sound */
#define TE_SYNTH_KNOBS      ((volatile int16_t *)0xff8013c4)    /* [8][8] knob values per sound */
#define TE_SYNTH_PATCH(s)   ((void *)(0x01ddd7e8 + (s) * 0x38))  /* patch object per sound */
#define TE_PATCH_SET_KNOBS  ((void (*)(void *patch, int16_t *knobs))0x01a00d14)  /* -> JSON + save */
#define TE_SYNTH_SUBSCREEN  ((volatile int32_t *)0x01b4ee88)    /* 0 engine, 1 envelope, 2 fx, 3 lfo */
#define TE_SYNTH_CONTROLLER ((void *)0x01bab530)                /* edit mode 0, vtable 0x01911994 */

/* Voice render (vtable slot +0x14, this-delta 0 for all): render(this, int16 *out, int16 *knobs),
 * 128 samples, WRITES out (verified by running dsynth's render in the simulator).
 * Voice vtables (render fn ptr at vptr + 0x14):
 *   fm 0x0190ff84 -> 0x019a23d4   digital 0x01911a34 -> 0x01a51f7a   string 0x019118c4 -> 0x01a4f098
 *   cluster 0x0190f67c -> 0x01960288   phase 0x0191113c -> 0x01a0bc5c   dna 0x019102a4 -> 0x019b48d8
 *   voltage 0x0191185c -> 0x01a49bf4   iter 0x019103cc -> 0x019b99d4   dsynth 0x0190fbbc -> 0x0197e1d8
 *   drwave 0x0190fb2c -> 0x0197c870   sampler 0x019119f4 -> 0x01a51a7c   ls 0x019109ec -> 0x019d33a0 */
#define TE_VOICE_SAMPLES    128

/* Framebuffer (bf/RGB.cpp). DOUBLE-BUFFERED: TE renders into 0x02000000 / 0x03000000 in turn,
 * clears the back buffer before every frame and repoints DMA0 at it (TE_FRAME_PRESENT), so
 * pixels written here directly vanish or flicker. Draw via TE_SITE_FRAME + op1/gfx.h instead.
 * PPI/DMA0 scans the current buffer continuously.
 * Geometry from TE's own boot-screen fill loop (0x01a2fbe8): 16-bit pixels,
 * 160 px per line, 320 lines, 512-byte stride. The panel is mounted rotated:
 * the 320x160 UI (SVG canvas) maps sideways onto it. Pixel format assumed
 * RGB565 (device-verified with examples/key_probe). */
#define TE_FRAMEBUFFER      ((volatile uint16_t *)0x02000000)
#define TE_FB_WIDTH         160     /* pixels per line */
#define TE_FB_HEIGHT        320     /* lines */
#define TE_FB_STRIDE        256     /* pixels (512 bytes) */
#define TE_RGB565(r, g, b)  ((uint16_t)((((r) & 0x1f) << 11) | (((g) & 0x3f) << 5) | ((b) & 0x1f)))

#endif
