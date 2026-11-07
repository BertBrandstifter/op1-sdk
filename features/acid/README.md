# ACID 3 synth engine — spec

Status: **v0.2** (2026-11-07): the sound rebuilt as a fixed-point port of **Open303** (Robin
Schmidt, MIT, see `NOTICE`), the TB-303 model behind JC303: v0.1's own Moog-type ladder "did not
sound like it" on the device (user). A/B against the original Open303 (built on the host, 9
scenarios: plain, low cutoff + resonance, accents, slide, square, full env mod, quick accents, high
notes): level within 0.2 dB, spectral centroid within 0.03 oct (median). Sim-verified; flashed for
listening. Code: `features/acid/` (engine host, extended tag 8).

A bassline voice in the style of the Behringer **TD-3-MO** ("modded out"): the classic 303-type
mono voice (one oscillator, saw or square, a resonant 4-pole filter with its envelope, accent and
slide) with the Devil-Fish-style mods of the MO: sub oscillator, overdrive, slide time, soft attack,
separate normal / accent decay, accent sweep with its speed, filter tracking, filter FM and the
muffler. Monophonic like the original: the newest note sounds, a legato note slides.

## Open it on the OP-1

Part of the default `op1plus` image. Pick **ACID 3** in the synth engine list, or load the preset
`acid 3` (browser folder acid 3). Sounds: **shift + blue turn** (P-- + a bank of 16); knob push
cycles pages.

## What it is

| page | blue | green | white | orange |
|---|---|---|---|---|
| 1 FILTER | **CUTOFF** 0 .. 10 (314 .. 2394 Hz) | **RESONANCE** 0 .. 10 | **ENV MOD** 0 .. 10 | **DECAY** 30 ms .. 3 s (normal notes, time constant) |
| 2 ACCENT | **ACCENT** 0 .. 10 | **ACC DECAY** 30 ms .. 3 s | **SWEEP** OFF / NORM / HIGH | **SPEED** SLOW / NORM / FAST |
| 3 VCO | **WAVE** SAW / SQUARE | **SUB** OFF / LO / MID / HI | **TUNE** −12 .. +12 st | **SLIDE** 10 .. 360 ms |
| 4 MODS | **SOFT ATK** 0.3 .. 30 ms (filter envelope attack) | **TRACKING** 0 .. 10 | **FILTER FM** 0 .. 10 | **OVERDRIVE** 0 .. 10 |
| 5 PLAY | **MUFFLER** OFF / SOFT / HARD | **ACCENTS** OFF / ALL / VEL / 2ND / 3RD / 4TH | **SLIDES** LEGATO / ALL / OFF | **LEVEL** 0 .. 10 |
| 6 | the sound's FX knobs (engine host) | | | |

- **VCO**: band-limited saw or square (polyBLEP); **SUB** adds a square one octave down at three
  levels. **TUNE** in semitones on top of TE's pitch (octave, bend, TE's LFO = vibrato).
- **Sound = Open303's model** (v0.2): the 303 saw / tanh-shaped square (46.9 % high), a 44.5 Hz
  high-pass, the **TB-303 diode-ladder filter** (linear, coupled stages, its coefficient /
  feedback / gain-compensation fits, 150 Hz high-pass in the feedback; 2 × oversampled at
  176.4 kHz), a 24 Hz high-pass after; CUTOFF 314 .. 2394 Hz (the 303's range), resonance skewed
  as Open303, the **measured env-mod mapping** (env mod scales and offsets the envelope in
  octaves, as measured on a real 303); the filter envelope is the MEG through rc1 (SOFT ATK, stock
  3 ms), the accent through rc2 (the sweep circuit); the **amp**: instant attack, 1230 ms decay to 0
  (long notes die away as on a 303), plus MEG × (0.45 + 4 × accent) while the gate is open, a
  200 Hz de-click; key up = 0.5 ms release (accented notes 50 ms). Slide = Open303's pitch slew
  (0.2 × SLIDE on the frequency).
- **MO mods on top**: **OVERDRIVE** (a tanh before the filter, 0 = linear as Open303),
  **TRACKING** (0 .. 1 oct/oct around C3), **FILTER FM** (the output on the cutoff, audio rate),
  **SUB**, **SWEEP** OFF (no sweep circuit) / NORM (Open303) / HIGH (×1.6 accent, + resonance),
  **SPEED** rc2 60 / 15 (stock) / 5 ms, **MUFFLER**.
- **Envelope (MEG)**: starts at every non-slid note, decays exponentially with **DECAY** (normal
  notes) or **ACC DECAY** (accented notes). A slid note keeps the running envelope (no retrigger),
  as the original.
- **Accent**: an accented note opens the filter (ACCENT × 2.5 oct × envelope) and is louder (up to
  +6 dB at 10, following the envelope). **SWEEP** OFF = accent straight on the cutoff; NORM = through
  the sweep circuit (a capacitor: quick accents stack up, the "wow"); HIGH = more sweep and extra
  resonance on accents. **SPEED** = how fast the sweep circuit charges and drains (SLOW 120 ms,
  NORM 40 ms, FAST 12 ms).
- **Where accents come from** (**ACCENTS**): OFF; ALL (every note, as holding the MO's accent
  button); VEL (MIDI notes with velocity ≥ 100; the OP-1's keys have no velocity); 2ND / 3RD / 4TH
  (every 2nd / 3rd / 4th note, counting from the first after a 2 s pause).
- **Slide** (**SLIDES**): LEGATO = a note started while another key (or MIDI note) is still held
  slides from the last pitch, without retriggering the envelopes; ALL = every note slides (for
  the sequencers, whose notes carry no key); OFF. **SLIDE** = the slide time (RC, as the original:
  fast at first, settling).
- **VCA (VEG)**: attack **SOFT ATK** on normal notes, 0.3 ms on accents; then held. TE's envelope
  (the preset sets full sustain, short release) ends the note. **MUFFLER** = soft clipping after the
  VCA, bass kept (SOFT / HARD = more drive). **LEVEL** = output (10 = unity, 15 = ×1.5); the
  passband the resonance takes (~10 dB at full) is made up (×1 .. ×2.5 with RESONANCE), so
  resonance does not make the sound quieter. Output peaks above 20000 go through a tanh up to
  28000; the bank sits at −10 .. −17 dBFS rms (measured in the bench, 4 notes per sound).
- **Monophonic**: one voice state; the TE voice of the newest note carries it, the one before fades
  out within one audio block (1.5 ms). Releasing the newest key while an older one is held does not
  go back to the older note (TE ends the newest voice).
- Page 1 lives in knob slots 1-4, so **TE's LFO can move cutoff, resonance, env mod, decay**.
- **Cost** (simulator, cycles per 128-sample audio block, the whole engine since it is mono): a
  saw with the filter open 34.5k, everything on (sub, overdrive, resonance, filter FM per sample,
  tracking, HIGH accent, slide, muffler) 63k; MONO 3's four voices take ~147k.
- **Recipe order**: ACID 3 must come before `engine_plus` (its page pushes claim every engine
  screen).

## Storage (all inside drwave's ranges: presets load on stock firmware as drwave)

- Slots 1-4: page 1 in bits 6..13 (8 bits, LFO-able), 6 bits of fields below (read from the stored
  knobs, never TE's LFO copy): slot 1 ACCENT (6), slot 2 ACC DECAY (5) + WAVE (1), slot 3 SWEEP (2) +
  SPEED (2) + SUB (2), slot 4 TUNE (5).
- Slots 5-8: extended tag 8 (slots 5-7 `0x7000`, slot 8 `0x1000`) | 12 bits: slot 5 SLIDE (5) +
  SOFT ATK (5) + MUFFLER (2), slot 6 TRACKING (5) + FILTER FM (5) + SLIDES (2), slot 7 OVERDRIVE (5)
  + ACCENTS (3) + LEVEL (4), slot 8 spare.

## Screen

Dot-matrix display look (`eh_dots.c`, as the other engines), param rows below unchanged:

- A green (80s phosphor) LCD 16 × 2: the knob turned last (name + value) over the page name and
  the note sounding. While a sound loads (1 s after a bank pick or when the screen opens): P03 (*
  = edited) over the sound's name on the left, the acid-house smiley drawn in display dots (15 × 15,
  outline, eyes and smile lit) on the right. The FX page shows the sound's number and name.
- A row of 16 step lamps under it: the last 16 notes (newest right); lit = normal, bright red =
  accent, a bar between two lamps = slide; the note sounding by name.
- Below: the page's four knobs, name scale 2 at y 99, 72 × 6 bar at y 112, "PUSH: next" at y 125.

## Bank (shift + blue turn)

P-- (the sound as it was) and 16 own sounds: ACID LINE, SQUELCH, ROLLER, SUB LINE, DEEP SQUARE,
WOW ACCENT, RUBBER, FM GROWL, MUFFLED, PLUCK, RESO RISE, OVERDRIVE, TRACK LEAD, BLIP, SLIDE LEAD,
DRONE.

## Plan / build steps

1. Engine host v1.4: extended tags (slots 5-7 tag 7, slot 8 = tag − 7), `eh_p2s` (done first, all
   engines' benches re-run).
2. `ac_core.c` (pure C, no TE): VCO, filter, MEG, accent sweep, VEG, slide, muffler; `gen_tables.py`
   → `ac_tables.h` (exp2 / log2, tanh, filter coefficient + resonance threshold per semitone from a
   numerical loop-gain search, decay rates, slide coefficients).
3. `test_ac_core.c`: sweep every parameter (its effect moves one way), self-oscillation only above
   the threshold, slide / no-retrigger, accent stacking, levels, CPU-free bounds checks.
4. `acid.c`: engine-host glue, mono voice handling, key / MIDI note observers (legato, velocity),
   pages, bank, screen.
5. `sim/run_acidtest.py` + `driver.c`: routing, pages, bank in range, FX page, engine name, LFO
   copy ignored, mono hand-over, cycles, screens to `build/bench/acidtest`.
6. Recipe `op1plus`, `features.py pack acid`, docs (README, docs/features.md, STATUS.md).
7. Device test (user): engine list, bank, slides with legato keys, accents ALL / 2ND, power-cycle
   with an ACID sound stored (boot check).
