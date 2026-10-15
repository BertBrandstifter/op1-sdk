# Input quantizer for the OP-1 (R246) — spec

Status: **v1.6 device-verified** (2026-10-11): QUEUE default, phrase start, held lengths kept —
"works now perfectly".
input are marked *(default)*. Implementation: `features/quantize/`.

## 1. Problem

The OP-1 records the synth to tape as audio. Notes played on the keyboard (or arriving over MIDI)
sound exactly when they are pressed, so every timing error ends up on tape and cannot be moved
afterwards. TE's sequencers are in time, live playing is not.

## 2. Goal

An **input quantizer**: while it is on, keyboard (and MIDI) note-ons and note-offs are held back and
played on the next point of a musical grid that follows the OP-1 tempo. The tape records the delayed,
on-grid sound. Off = stock behaviour, bit for bit.

Non-goals (v1): quantizing audio already on tape; changing sequencer timing; swing/groove;
quantizing knob movements.

## 3. Behaviour

### 3.1 Grid

- Grid sizes, cycled in this order: **1/16** *(default start)*, 1/8, 1/4, 1/2 (added after the
  first device test), 1/32, 1/8T, 1/16T, 1/32T (added v1.9).
- Grid length follows the current OP-1 tempo live: float BPM at `0x018c1878` (40–220), sample
  rate 44.1 kHz (TE's tempo maths use 2646000 = 44100·60).
- Alignment:
  - **tape running** (`[0xff802960]` = 1/2): TE's own tape grid, measured from tape position 0
    (`[0xff802974]` = samples·64), the grid the bar markers show — recordings line up with bars.
  - **tape stopped**: the metronome phase (`[0x01aadadc]` = samples to next beat ·2^14, counts
    down every block; samples per beat ·2^14 at `0x01aadad8`).
  - Known limit: TE restarts the metronome on PLAY, so if play starts off-grid the click and the
    tape grid differ; while the tape runs, the quantizer follows the tape (what gets recorded).
  - 1/2 with the tape stopped: the metronome only gives the next beat, not the bar, so half notes
    sit on every other beat of the quantizer's own beat count — possibly one beat off the bar.
    With the tape running 1/2 is exact (tape grid).

### 3.2 Timing rule (live, so notes can only be delayed, never moved earlier)

For an event at position `t`, with the previous grid point `g0 <= t` and the next one `g1 > t`:

- `t - g0 <= late_window` → play **now** (it was meant for `g0`, a tiny delay is better than a big one).
- otherwise → hold and play at `g1`.

`late_window` = **25 ms** *(default)*, capped at 1/4 of the grid step so 1/32 at fast tempos still works.
Effect in practice: play slightly ahead of the beat and every note snaps exactly onto it.

### 3.2a Phrase start (v1.5, user request)

- The first note after a pause plays **immediately** and becomes a grid point: the rest of the
  phrase is quantized at the chosen interval counting from that note (in SNAP and QUEUE mode).
- Pause = no note-on/off input for one beat (or one grid step if longer) and nothing waiting.
- Trade-off: while the tape runs, a phrase's grid is offset from the tape bars by how early/late
  its first note was (constant within the phrase). The blink dot follows the phrase grid.
- Leaving synth mode or turning the quantizer off resets everything (no stale state).

### 3.2b Queue mode (added after device test, user request)

- **QUEUE** (default since v1.6, after the device test showed fast notes colliding in SNAP): each
  note-on takes the **next free** grid slot after the previous note-on's slot, so a
  fast run plays one note per step in the order played. Note-ons within **40 ms** of the chord's
  first note (at most half a grid step) share its slot: chords stay chords even with the key
  scanner's stagger, while a fast run still splits into steps (v1.7, device report: chords in
  poly were sometimes queued). Once the queue has caught up, notes snap normally.
- **SNAP**: every note goes to the grid point the timing rule gives it; notes played faster than
  the grid stack on the same point.
- Note-offs as in 3.3: each queued note keeps the length it was held, from its slot.
- Queue depth: up to 32 waiting note-ons; beyond that notes play unquantized.

### 3.3 Note-offs (v1.4: held length is kept)

- A note-off moves by **exactly as much as its note-on was moved**, so every note sounds for the
  time the key was held, starting on the grid. (v1–v1.3 quantized note-offs on their own; a note
  pressed late and held briefly could then end in the late window right after its delayed start —
  e.g. pressed 40 ms late, held 100 ms -> sounded 15 ms. Found on the device, regression-tested.)
- Minimum note length: one audio block (64 samples), so on and off never land in the same call.
- A note-off never overtakes a queued event of the same key.
- Note-ons use at most half the 64-event queue, so their note-offs always fit; beyond that new
  notes play unquantized.
- Turning the quantizer off, or a full queue, releases everything that is pending immediately, in
  order (no stuck notes, ever). Off -> on resets the per-key state.

### 3.4 What is quantized

- **Keyboard notes** (the 24 piano keys): yes.
- **MIDI notes in** (USB MIDI from a computer/controller): yes *(default)*.
- **Sequencer output** (pattern/finger/endless/…): no, already on the grid.
- Notes that a running sequencer *records* (e.g. finger/endless record): passed through unchanged
  (the sequencer quantizes or records them itself). Solved by the hook position: TE's key logic
  (sequencer offer, tape trigger, note bookkeeping) runs unchanged and immediately; only the
  final voice note-on/off call is moved, so a sequencer that takes the key never produces one.
- **Synth and drum playmodes** (v1.8; v1–v1.7 synth only). Drum notes get the octave at play
  time (`12 * TE_DRUM_OCTAVE[slot] + extra`) and read TE's shift state, so a delayed drum note gets
  `extra += 12 * (octave at press - octave now)` and the press-time shift state around its
  note-on. Notes queued in one playmode are dropped (note-ons) if the other is active when they
  are due; note-offs always go out. A playmode switch in progress releases everything.

### 3.5 Controls

- **Toggle on/off and cycle grid**: **shift + insert** — the only combination found unused in every
  TE controller (global shift-down table `0x018dddc0` + per-mode controllers).
  Each press: off → 1/16 → 1/8 → 1/4 → 1/2 → 1/32 → 1/8T → 1/16T → 1/32T → off.
- The combo itself is consumed (TE never sees it).
- **Tempo screen** (metronome key; edit mode `[0xff907d58]` = 4): **shift + white knob** steps
  through the grids (no wrap); **shift + orange knob** right = QUEUE, left = SNAP. TE does nothing
  with shift+white / shift+orange there (TempoController knob
  handler `0x01a5ef8c` only handles ids 0–5). A `QUANT 1/16  SHIFT+WHITE` line is drawn in the free
  strip under the tempo boxes (tempo.svg boxes end at y 147).
- Everywhere: a **blinking dot** top right while on — flashes on every grid point (60 ms, at most
  half a step), **green on beats**, white on subdivisions. After a change via shift+insert the grid
  name shows top right for 1.5 s.
- State is not saved across power-off *(default)*: always boots with quantize off.

### 3.6 Limits and safety

- Pending-event queue: 32 events; if full, new events pass through unquantized.
- Max hold = one grid step (+ the minimum-length rule for offs): 1/4 at 40 BPM = 1.5 s worst case.
- Everything that is held is released in time even if the UI is busy (release runs on the sequencer
  thread, not the GUI thread).
- No TE assert may be reachable from our code with bad input (asserts are fatal on this firmware).

## 4. Design

```
 piano key / MIDI note ──► [hook: note in]  ── quantizer off ──► TE plays it now (unchanged path)
                                │ on
                                ▼
                        qz_input(t, event) ── within late window ──► play now
                                │ later
                                ▼
                         pending queue (32)
                                │
 Seq thread tick ──────► [hook: tick] qz_poll(t) ──► due events ──► te_orig_<note fn>(...)
 GUI frame ────────────► [hook: frame, after] ──► draw "Q 1/16"
 key combo ────────────► [hook: on_key, filter] ──► cycle grid, consume key
```

- `quant_core.c` (since 2026-10-13 its own library feature `features/quantcore`, shared with requantize): pure C, no TE dependencies; grid maths, timing rule, queue, note-off ordering.
  Unit-tested natively on the Mac (fast, many cases) and runs unchanged on the device.
- `quantize.c`: TE glue (hooks, reading tempo/position, replaying notes, overlay).
- Time base: own counter `now += 64` per Seq-thread tick. The tick (`0x01a0d2c0`, call site
  `0x01a431aa`) runs once per 64-frame audio block (~689 Hz) whether or not the tape moves.
  Each tick: compute samples to the next grid point from the reference above, set the core's
  origin to `now + that`, drain new events, poll. Timing error <= 1 block (1.45 ms).
- Threads: key/MIDI hooks run on other threads; they only push stamped events into a lock-free
  single-producer ring. The core runs only on the Seq thread (same thread TE's sequencers play
  notes from), so it needs no locking.
- Hook points (`features/quantize/project.json`):
  - keyboard voice calls `0x019ac290` → note-on `0x01a85750`, `0x019ac3a6` → note-off `0x01a85840`
  - MIDI-in voice calls `0x01a85bc6` → note-on, `0x01a8599e` → note-off (vel-0 note-ons included)
  - all four are tail calls (return value unused), hooked with "filter"; the 16-byte note record
    lives on the caller's stack and is copied (synth octave is already baked in)
  - Seq tick `0x01a431aa` ("after"), frame `0x019ab8b2` ("after"), `on_key` `0x019b6c74` (filter).
  - Not chosen: filtering the outer key functions `0x019ac0b0` / `0x019ac2f8` — a key-up passing a
    queued key-down trips TE's `s_note_ons` assert (fatal).
- Time base: audio-sample position as uint32 (wraps after ~27 h; all comparisons use signed
  differences). Grid length kept in 16.16 fixed-point samples so triplets do not drift.

## 5. Tests

1. Host unit tests (`tests/test_quant_core.c`, plain `cc`): grid maths at several tempos, window rule,
   triplets without drift over 1000 bars, note-off ordering + minimum length, queue overflow,
   flush on disable, uint32 wrap.
2. Simulator bench: the generated hooks + glue with stub TE functions: held note is replayed through
   `te_orig` with the original arguments, combo is consumed, off = pass-through.
3. Device: play against the metronome with tape recording, quantize on/off, each grid size, MIDI
   notes from the Mac (`tools/midi_send.swift`), long session for stuck notes.

## 6. Plan

1. ~~RE: tempo, beat phase, seq tick rate, note path, free key combo~~
2. ~~Spec update with the RE answers~~
3. ~~`quant_core` + host unit tests~~ (`test_quant_core.c`, 24k checks)
4. ~~TE glue `features/quantize` + simulator bench~~ (`sim/run_quanttest.py`, 14 checks, mutation-tested)
5. Device flash + test session, then fix-ups.
