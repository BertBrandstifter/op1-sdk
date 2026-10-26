# MALLET synth engine — spec

Status: **v0.2** (2026-10-22: screens after the user's "MALLET · <instrument>" art, header left
out, instruments centred), sim-verified;
v0.1 device-booted (2026-10-22). Code: `features/mallet/` (in
recipe `op1plus`, after mono; engine host tag 7). Own code (modal synthesis; mode ratios from the
acoustics literature of bars, pans and tubes).

## Open it on the OP-1

Part of the default `op1plus` image. Pick **MALLET** in the synth engine list, or load the preset
`mallet` (browser folder mallet). Sounds: **shift + blue turn** (bank of 16); knob push cycles pages.

## What it is

Struck instruments by modal synthesis, eight in one: **MARIMBA**, **VIBES** (vibraphone, with the
motor's tremolo), **XYLOPHONE**, **GLOCKENSPIEL**, **STEELPAN**, **HANDPAN**, **KALIMBA** and
**TUBULAR** bells. A mallet pulse excites up to six resonators tuned to the instrument's modes; each
mode rings with its own decay. TE's envelope, velocity, volume, FX, LFO, glide, bend, octave and
sequencers apply as for any stock engine (the preset: full sustain, long release, a little spring).

| page | blue | green | white | orange |
|---|---|---|---|---|
| 1 | **INSTR** instrument (8, one per detent) | **MALLET** soft → hard | **DECAY** ×¼ … ×4 (centre = the instrument's) | **STRIKE** bar centre → edge |
| 2 | **MOTOR** off, then 1.2 → 8 Hz | **TUBE** resonator tube | **DETUNE** overtones −4 % … +4 % | **TONE** dark → open |
| 3 | FX knob 1 | FX knob 2 | FX knob 3 | FX knob 4 |

- **MALLET**: the pulse lasts a share of the note's period (0.6 soft → 0.04 hard), so its timbre is
  the same over the range: soft = the fundamental only, hard = every mode; from 55 % on, a click
  (noise on the pulse) is added.
- **DECAY** scales every mode's T60; higher notes ring shorter (−½ octave of T60 per octave up).
- **STRIKE** scales the overtones (×0.25 at the centre … ×1.75 at the edge).
- **MOTOR** (vibraphone): 80 % tremolo on the fundamental, one motor for all voices.
- **TUBE**: the fundamental up to 1.6× louder and 4× longer (marimba / vibraphone resonators).
- **DETUNE** stretches the overtones (inharmonic / out-of-tune bars, wider pan wobble).

| instrument | mode ratios | T60 at C4 (fundamental) |
|---|---|---|
| MARIMBA | 1 : 3.99 : 10 | 1.2 s |
| VIBES | 1 : 4 : 10 | 7 s |
| XYLOPHONE | 1 : 3 : 6 | 0.45 s |
| GLOCKENSPIEL | 1 : 2.76 : 5.40 : 8.93 | 3.5 s |
| STEELPAN | 1, 1.004 (doublet: the wobble) : 2 : 3 : 4.01 | 1.6 s |
| HANDPAN | 1, 1.0025 : 2 : 3 | 4 s |
| KALIMBA | 1 : 6.27 | 1.8 s |
| TUBULAR | 0.62 : 1.21 : 2 : 2.99 : 4.17 : 5.43 (modes 4–9 of a free tube) | 6 s |

**Bank** (shift + blue turn): MARIMBA, SOFT MARIMBA, BASS MARIMBA, VIBES, VIBES MOTOR, DARK VIBES,
XYLOPHONE, WOOD BLOCK, GLOCKENSPIEL, MUSIC BOX, STEELPAN, TENOR PAN, HANDPAN, KALIMBA, TUBULAR
BELL, CHURCH BELL. One preset `mallet` in the browser (= MARIMBA) and one engine-list row, as every
SDK engine.

## Screen

After the user's "MALLET · <instrument>" art (1280 × 640 = 4 × the screen), drawn from lines,
ellipses and dashes in code (no bitmaps: the eight screens add ~2 KB), without the art's header
(name, number, info: more room), each instrument centred between the screen's top and the knob row.
The sounding notes are lit (a held key its own note; sequencer / MIDI notes
flash ~145 ms) and the mallet sits on the newest one, with dashed waves round it:

- **MARIMBA / VIBES / GLOCKENSPIEL** share one drawing: sharps raised behind the naturals on their
  rails inside the frame (purple / blue / a teal case with a handle and dots on the bars); marimba:
  tubes below as long as TUBE, a rail, legs, a second mallet resting on the frame; vibraphone: the
  motor's shaft with discs that open and shut with its phase, the pedal.
- **XYLOPHONE**: one row of bars on two converging rails (rosewood yellow), mallet from below.
- **STEELPAN / HANDPAN** share the pan from above: steelpan 12 outer fields (C3's octave round the
  circle of fifths, C at the top), 12 inner ones (the octave above), C5 in the middle, and the
  drum on its stand beside it; handpan the ding and 8 fields (key % 9) and its two shells from the
  side with the sound from the port.
- **KALIMBA**: the board with 17 tines (C3 .. E5's naturals, the lowest in the middle, alternating
  out), the sound hole, a tine from the side and its decaying ring.
- **TUBULAR**: 12 tubes (one octave, chromatic) hanging from the red frame on wheels, the hammer.

For 1 s after a sound is picked (or the screen opens) its name over the instrument (`P01 MARIMBA`,
`*` = edited). Below, unchanged (as STATION 1): the page's four knobs, name and bar (INSTR: one
value per detent), `PUSH:` the next page and the page (P1–P3) at the right.

## How

- **Host:** the engine host (`features/enginehost`), tag 7: knob slots 5–8 all `0x7000 | v`
  (v = 12 bits of page 2), page 1 in slots 1–4 as 0..16383. Stock drwave presets carry 0x7d00 /
  0x7fff in slot 5 (tag 7) but 0 in slots 6–8, so they never match (all 16 factory drwave presets
  checked). Every value is inside drwave's ranges: the boot-time validator passes and stock firmware
  loads the sound as drwave.
- **Engine list:** `content_sql` adds `synth_types` row `mallet` (MARIMBA's values) and the preset
  `mallet`; the shared `engine_name` point renames "mallet" → "drwave" (same length).
- **DSP** (`mallet_core.c`): computed at 44.1 kHz, interpolated to TE's 88.2 kHz. Pulse 4u(1 − u)
  with area 1 (every mode starts at its gain, the pulse's spectrum shapes the highs), hard mallets
  add LCG noise on it. Resonators in coupled form (s1 += e·s2, s2 −= e·s1, e = 2 sin(w/2) from a
  polynomial, Q16), states 8 bits finer than the output with 32-bit split products, so low notes
  stay in tune (worst 2.2 cents C2–C6) and tails do not stall. Decay every 16 samples (Q24, 64-bit),
  T60 → rate via exp2 tables; modes above 11 kHz are dropped (and the state bound keeps every
  product in 32 bit). Per-mode set-up cached until pitch or knobs change. Output: one-pole TONE,
  soft knee above half scale. A restrike damps the ringing modes to ¼ (no build-up).
- **Cost (sim):** per 128-sample voice block MARIMBA 10.5k cycles, TUBULAR (6 modes) 17.0k, a
  strike block 23.9k (WHEEL's limits: 30k / 40k).

## Tests

1. `test_mallet_core.c` (9 checks, ASan/UBSan): zones/names; pitch of every instrument C2–C6 within
   3 cents; marimba 4× and xylophone 3× partials (no 2×); T60 order vibes > marimba > xylophone and
   DECAY ×4 / ×¼; soft vs hard mallet; MOTOR tremolo vs decay alone; levels (peaks 12k–17k at C4)
   and TUBE; 40 restrikes at every knob extreme 20 Hz–8 kHz: bounded, short settings decay; a voice
   without note-on is silent.
2. `sim/run_mallettest.py`: stock drwave untouched (slot 5 tag 7 alone); pitch from TE's buffer;
   restrike; pages; MOTOR / INSTR edits, ranges, marker; shift / other screens / other engines are
   TE's; page 3 FX knobs; bank (16, wraps, marked, in range); engine-list rename next to WHEEL's;
   bars lit by a held key; the name for 1 s; a screen per instrument →
   `build/demo/mallet_screen_<instrument>.png`; cycles.

## Risks (device test)

- Levels in chords next to the other engines (one strike peaks 12k–17k, three voices ~25k).
- CPU with six voices of TUBULAR: ~6 × 17k cycles per block (~15 % of the DSP).
- Leave a MALLET sound current, power off and on (boot check), and load it on stock firmware.
