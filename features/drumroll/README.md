# DRUM ROLL — step sequencer for drum kits — spec

Status: **v0.8** (2026-10-14; "drum roll" in the drum engine list; tape-screen play / stop; back-to-back hits + ratchet fixed; v0.3 device-tested: "nice"; v0.2 device-tested; v0.1 device-tested same day, user: "love it"), `features/drumroll`, in `op1plus` (image `ba877d1c`, not flashed);
host-tested (`test_dr_core.c`, `test_dr_dsp.c`), sim-verified (`sim/run_drtest.py`, mutation-tested);
awaiting device test. Screens from the simulator: `build/demo/drumroll_step.png`, `drumroll_sound.png`.

## Open it on the OP-1

Part of the default `op1plus` image. Pick **drum roll** in the drum engine list (next to dbox), or **shift + green press** on the drum screen. On the tape screen (drum playmode): **shift + white press** = play / stop, to record it.

## Idea

A channel-rack style drum sequencer (FL Studio's step sequencer + its per-step event graph) in a
Teenage Engineering look: black screen, thin lines, the four knob colours, small type. It plays
the current drum kit (drum playmode). Every **lane** is one drum sample (one of the kit's 24 keys)
with its **own length and rate**, so lanes run polymetric (a 3-step hat against a 16-step kick).
Every step carries its own **velocity, tone, length, nudge, probability and ratchet**. Every drum
sample can get a **filter** (LP / BP / HP, cutoff, resonance) and a level.

Like the requantize piano roll, the screen draws time to scale: bars and beats are lines, every
step is as wide as its duration, so a 1/32 hat lane shows twice as many narrow cells as a 1/16
kick lane, and where a short lane loops a coloured loop line marks its end and the repeats are drawn
dimmer. Every lane has its own playhead.

## How it plays (route c + a voice post-process)

- **Clock**: the Seq thread tick (once per 64-frame block). While the tape plays or records, the
  sequencer follows the **tape position** (bar 1 = tape position 0, the bars TE draws), so it lines
  up with what is on tape and records into it. With the tape stopped it runs on its own clock.
  Tempo = TE's BPM (tempo screen). Lanes start together at clock 0 and loop on their own lengths.
- **Notes**: VoiceCtl note-on/off (`0x01a85750` / `0x01a85840`) with drum note 53 + key; TE adds the
  kit's octave. Only while TE is in drum playmode **and not switching** (`0x01de4c40 == 1` and
  `0xff801038 == 1`: TE's drum note-on asserts, i.e. halts, otherwise). In synth playmode the
  sequencer keeps time silently.
- **Tone** = the note-on's `extra` argument: TE's drum note-on turns it into the voice's pitch factor
  2^(extra/12) (RE, `0x01a84ba0`). -24 .. +24 semitones per step.
- **TE ignores a drum note-on while that note is still held** (0x01a84ba0 checks its sounding list
  first). A hit therefore first releases DRUM ROLL's still-held copy of the same drum (back-to-back
  steps, ratchets; v0.6 fix), and due note-offs go out before new hits in each tick.
- **Every note-on gets its note-off** (TE keeps a 128-entry list of sounding drum notes and halts
  when it overflows).
- **Voice post-process** (filter, level, length gate): drumroll owns the drum sampler voice's render
  and note_on vtable slots (`0x0190fad8`, `0x0190fae0`; 12 voices of 0x14 bytes at `0xffb00000`,
  128 samples per block, the 2x rate TE renders voices at). After TE renders a voice the voice's
  sample (key) gets its filter; voices started by the sequencer also get the step's velocity x lane
  level and the length gate (fade-out over 3 ms when the gate ends). The note_on slot sees the key
  directly (`params - 0x01aaadb4`, 16 bytes per key, 0x180 per kit slot). Drum keys played by hand
  get the sample's filter and level too. dbox (synth drum) kits are sequenced but not post-processed.

## Data

- 4 patterns **A B C D**, 8 lanes each, up to 64 steps per lane. RAM only (lost at power-off, v0.1).
- Lane: sample key 1..24 (default lanes = keys 1..8), length 1..64 steps (16), rate 1/4 1/8 1/8T
  **1/16** 1/16T 1/32, level 0..100 (100 = as TE plays it), mute.
- Step: on/off + VEL 1..100 % (100), TONE -24..+24 (0), LEN 1/8 1/4 1/2 3/4 1 1.5 2 3 4 6 8 16 steps or
  **FULL** (default: the sample plays out, the note-off follows one step later), NUDGE -50..+50 % of
  a step (0), PROB 0..100 % (100), RATCH 1..4 hits per step (1). Parameters stay when a step is
  switched off and on again.
- Per sample (key) filter: OFF / LP / BP / HP, cutoff 0..127 (20 Hz .. 20 kHz, exponential), reso
  0..127 (Simper trapezoidal SVF, the TRIO filter). Lanes on the same key share it.
- Swing 0..100 (global): odd steps of every lane late by up to half a step.

## Engine list (v0.8)

"drum roll" sits in the drum engine list next to dbox (a `drum_types` row, id 64, via `content_sql`).
Picking it turns DRUM ROLL on for that drum sound: an entry hook on TE's pick handler (`0x0193f86c`,
msg +4 drum sound, +8 row id or -kit index) skips TE, so the sound keeps its kit and no unknown type
is ever saved (a saved unknown type would halt the next boot: patch validator, RE 2026-10-14).
Kits picked afterwards from the same list are DRUM ROLL's sounds (it stays on); dbox turns it off.
DRUM ROLL is then that sound's engine page (T1), like a synth engine's screen; T2-T4 (envelope, FX,
LFO) stay TE's, so the SDK effects' pages work there. The flag lives in RAM like the patterns (after a
power cycle the sound is a plain kit). Shift + green press still opens DRUM ROLL on any drum sound's
engine page and closes it.

## Controls

v0.2 (2026-10-14, user: "more consistent, like our synths"): the same handling as the SDK synth
engines (WHEEL, WARP, TRIO): **a knob turn edits the value under that knob, blue press goes to the
next page, shift + blue turn browses** (the engines' sound bank; here the lanes). v0.3 (user): only
blue press turns pages, **white press plays the cursor step**, orange press does nothing.
v0.4 (user): **green press switches the cursor step on / off on every page**.
v0.5 (user, "more ergonomic"): swapped: **blue press = step on / off** (any page), **green press = next
page**. Every page puts
four functions on the four knobs, drawn as four columns in the knob colours (name, value, bar).

Open / close: **shift + green press** on the drum screen (drum playmode). The sequencer keeps running
when the screen is closed (small badge top right). Drum keys still play and select their lane.

| page | blue | green | white | orange |
|---|---|---|---|---|
| P1 STEP | step | ON (right = hit, left = rest) | VEL | PROB |
| P2 NOTE | step | TONE | LEN | RATCH |
| P3 TIME | step | NUDGE | SWING (all lanes) | ZOOM 1 / 2 / 4 bars |
| P4 LANE | SAMPLE (drum key) | LENGTH (steps) | RATE | MUTE |
| P5 SOUND | FILTER OFF LP BP HP | CUTOFF | RESO | LEVEL |

| with shift | |
|---|---|
| blue turn | lane (browse) |
| green turn | pattern A B C D |
| green press | close the screen |
| white press | play / stop |
| orange press | clear the lane (again: undo) |
| blue press | nothing while the screen is open (v0.3: used to start a requantize take) |
| white press **on the tape screen** | play / stop (v0.7), drum playmode, not while the requantize screen is open |

Recording (v0.7, user): on the tape screen, start DRUM ROLL with shift + white press and record with
TE's own keys (rec + play). While the tape runs DRUM ROLL follows the tape position (bar 1 = tape
position 0), so the hits land on TE's bars; the badge top right shows that it runs.

- White press plays the cursor step with all its settings; stopped, editing a step (ON, VEL, TONE,
  LEN, RATCH) plays it too.
- Step pages show the last edited step parameter as a graph over the lane's steps; P5 the filter
  curve; P4 a line about the lane. Header: pattern, PLAY / TAPE / STOP, bar.beat, BPM, page dots +
  "P1 STEP" as on the engines.
- v0.1 had two focuses (STEP / LANE) flipped by green press and shift layers on every knob.

## Screen (320 x 156 UI)

```
 A  PLAY  3.2  120 BPM  SW 20                 STEP   header (scale 2), focus lit
 1 K01 16 |##  ..  ##  ..|##  ..  ##  ..|     lanes: label | cells to scale; bar lines bright,
 2 K02  3 |..##..|..##..|..##..                beat lines mid; loop line + dim repeats
 3 K03 32 |#.#.#.#.#.#.#.#.#.#.#.#.#.#.          for short lanes; cursor = white frame;
 ...                                             playhead per lane = white bar under the cell
 VEL      |▮ ▮ ▯ ▮ ▮ ▯ ▮ ▮ ...                  graph of the parameter (STEP focus) or
 100      |                                     lane panel KEY FILTER CUTOFF RESO + curve
```

- Lane colours cycle blue / green / white / orange; cell brightness = velocity, a step with tone
  shows a small tick (up/down), a ratchet step is split into its hits.
- The view (1 / 2 / 4 bars) scrolls to keep the cursor step visible.

## Cost (simulator cycles)

- Filtered voice block (128 samples): ~8.1k cycles for the SVF (Q31 coefficients, 32-bit states:
  a Q15 version leaked ~25 % at 20 Hz), ~2.1k more with gain + gate; unfiltered unity voices cost
  nothing extra. Idle voices (silent block, settled filter) are skipped. In TRIO's range per voice.
- Seq tick: one 32-bit divide per lane, step lengths cached until the tempo or a rate changes
  (the whole seq-tick chain of op1plus: ~8.5k cycles per block with DRUM ROLL playing).
- Screen: ~0.4M cycles per frame (full-screen overlay; GUI thread). `lib/gfx.c` got a clipped
  column fill for `gfx_rect` and direct glyph stores (3x faster for every overlay).

## Out of scope (v0.1)

Saving patterns to flash, pattern chaining / song mode, copy / paste, Euclid fill, dbox
post-processing, MIDI out of the sequence, per-step sample choice, tape speeds other than 1x.

## Code

- `dr_core.h/.c` (pure C, `test_dr_core.c`): data model, defaults, parameter ranges and labels, the
  scheduler `dr_schedule` (which hits fall into a window of the clock: rates, lengths, swing, nudge,
  ratchet, probability; every hit exactly once across consecutive windows).
- `dr_dsp.h/.c` (pure C, `test_dr_dsp.c`): filter coefficients (cutoff table at 88.2 kHz), per-voice
  post-process (SVF, gain, gate with fade).
- `drumroll.c`: Seq tick (clock, notes, note-offs, preview), the two voice slots, keys / knobs, screen.
- `sim/run_drtest.py`: the composed op1plus image against stub TE functions.

## Plan

1. Spec (this file).
2. `dr_core` + host tests: grid timing, polymeter lanes, rates incl. triplets, swing, nudge across
   window edges, ratchet, probability 0 / 100, no double hits over consecutive windows.
3. `dr_dsp` + host tests: LP passes low / damps high, HP the reverse, gate ends with a fade to
   silence, gain, filter OFF + unity = untouched.
4. `drumroll.c`: tick + notes (playmode guard, note-off for every note-on, tape sync), voice slots,
   controls, drawing.
5. Manifest, recipe `op1plus`, docs (`features.md`); sim bench `sim/run_drtest.py`; full suite; build.
6. Device test: drum screen edit mode (assumed 1; the gesture also works in drum playmode on any
   screen that is not synth / tape / mixer / tempo), timing against the metronome, tone and gate on
   a few kits, filter sound + CPU with 12 voices, badge position, power-cycle (nothing is stored).
