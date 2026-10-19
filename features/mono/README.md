# MONO 3 synth engine — spec

Status: **v0.4** (2026-10-19): page knobs as STATION 1 (name + bar), the sound's name over the
panel for 1 s, keys lit as STATION 1 (a held key its own note). Code: `features/mono/` (engine host,
tag 2). A classic three-oscillator mono synth voice made polyphonic: three oscillators with range,
wave and level each, noise, a 4-pole ladder filter with its contour, the loudness contour, glide.
16 sounds in the bank, from fat basses to leads, brass and pads. Own code and data.

## Open it on the OP-1

Part of the default `op1plus` image. Pick **MONO 3** in the synth engine list, or load the preset
`mono 3` (browser folder mono 3). Sounds: **shift + blue turn** (P-- + a bank of 16); knob push cycles
pages.

## What it is

| page | blue | green | white | orange |
|---|---|---|---|---|
| 1 FILTER | **CUTOFF** −5 .. +5 (41 Hz .. open) | **EMPHASIS** 0 .. 10 (self-oscillates from 9.1) | **CONTOUR** 0 .. 10 (amount) | **GLIDE** off, 10 ms .. 2 s |
| 2 OSC 1 | **RANGE** LO 32' 16' 8' 4' 2' | **WAVE** | **VOLUME** 0 .. 10 | **NOISE** off, white / pink 0 .. 10 |
| 3 OSC 2 | **RANGE** | **FREQUENCY** −7 .. +7 st | **WAVE** | **VOLUME** |
| 4 OSC 3 | **RANGE** | **FREQUENCY** | **WAVE** (ramp in place of shark) | **VOLUME** |
| 5 FILTER CONTOUR | **ATTACK** 1 ms .. 10 s | **DECAY** 1 ms .. 10 s | **SUSTAIN** 0 .. 10 | **KEYBOARD** off, 1/3, 2/3, full |
| 6 LOUDNESS CONTOUR | **ATTACK** | **DECAY** | **SUSTAIN** | **RELEASE** off (21 ms) / on (= DECAY) |
| 7 | the sound's FX knobs (engine host) | | | |

- **Oscillators**: waves triangle, shark (osc 3: ramp, a rising saw), saw, square, wide pulse (1/3),
  narrow pulse (0.23), band-limited (polyBLEP); 8' = the pitch TE plays (octave, bend, TE's LFO =
  vibrato), LO = 3 octaves below. FREQUENCY of osc 2 / 3 in 31 steps: unison, ±3 .. ±75 cents,
  ±1 .. ±5 and ±7 semitones. All three start together at every note (the same attack every
  time); no drift.
- **Mixer**: linear, each level 0 .. 10; noise white or pink.
- **Filter**: a 4-pole ladder, 24 dB/oct, linear below self-oscillation; EMPHASIS takes the bass
  down (−10 dB at 5), self-oscillates in tune from 9.1, its level limited. The
  CONTOUR adds Hertz (not octaves): up to +4.95 kHz at 10, its peak at the end of the attack.
  KEYBOARD: the cutoff follows the key by 1/3, 2/3 or fully, around F2.
- **Contours** (filter and loudness): attack, decay to the sustain level; RELEASE on = a key up
  releases both with their DECAY times, off = in 21 ms. TE's envelope
  shapes the note afterwards as for every engine; the preset `mono 3` sets it to full sustain, so the
  loudness contour is the sound's own. Notes without a key (sequencer, MIDI) do not release the
  contours: TE's envelope ends them.
- **Glide**: constant time (the same for any interval), linear in pitch; a new note slides from
  where the last one was (poly portamento).
- **Level** (0.3, after the first device test: half as loud as TE's engines): a saw at full peaks
  ~12k per voice, the bank sustains at −10 .. −16 dBFS rms; above 20000 a tanh limits to 28000
  (three full oscillators, self-oscillation), as LOFI's 24576 / WHEEL's 18000 peaks.
- **Polyphonic, 4 voices**: every note its own oscillators, filter and contours; the 4 newest sound
  (a 5th fades the oldest out within 1.5 ms).
- Page 1 lives in knob slots 1-3, so **TE's LFO can move cutoff, emphasis, contour** (the classic
  filter wobble).
- Engine list **mono 3**; preset browser folder **mono 3** with one preset **mono 3** (FAT BASS,
  octave 0); **shift + blue turn** browses P-- (the sound as it was) and 16 sounds: FAT BASS, PLUCK
  BASS, SUB BASS, RESO BASS, FUNK BASS, TAURUS, LUCKY LEAD, SAW LEAD, SHARK LEAD, OCTAVE LEAD,
  WHISTLE, BRASS, FLUTE, SOFT PAD, ZAP, WIND.

Changed from 0.2 (sounds saved with 0.2 load as different MONO 3 sounds): the 8 wave sets became
per-oscillator range / wave / level; DETUNE became osc 2 / 3 FREQUENCY; DRIVE went (the mixer is
linear); the loudness contour, noise colour, keyboard control and the release switch came;
the bank was redone (0.2's cutoffs sat at the bottom of a scale that started at 20 Hz: plucks
closed the filter within ~60 ms, a click).

## Screen

As STATION 1: no header line, the
panel smaller with room around it, the page's four knobs below it.

- The panel (red outline, white control box): **CONTROL** (glide), **OSCILLATORS**
  (a row per oscillator: range, frequency, wave), **MIXER** (the three levels with their switches,
  noise with white / pink), **MODIFIERS** (keyboard control 1 / 2; cutoff, emphasis, contour; the
  filter contour; the loudness contour), **OUT**. Every knob shows its value; the current page's
  four are larger and in their knob colours (blue, green, white, orange), the others grey.
- Under it the wheel box (glide and decay switches, pitch / mod), the page (P1-P7)
  and osc 1's wave above the keyboard; the keyboard lights the held keys as STATION 1: a held key
  lights its own note (the octave from its voice, rounded, so TE's glide / LFO / bend at the note
  start do not move it); notes without a key (sequencer, MIDI, CHORD+'s extra notes) flash ~145 ms.
- The sound's name over the panel for 1 s when a sound is picked (shift + blue) or the screen opens:
  STATION 1's LCD (same place, scale-2 text, green): `P02 PLUCK BASS`, `*` when edited, or `P--`
  with `SHIFT+BLUE: SOUNDS`. Its clock is the Seq tick (`mo_tick`, 64 samples).
- Below, as STATION 1: the page's four knob names (scale 2, knob colours) over a bar each, then
  `PUSH:` the next page; page 7 = the sound's FX. (Values are read off the panel's knobs.)

## How

- Host: `features/enginehost`, tag 2. 24 parameters in the 8 knob slots (80 of 81 bits), every value
  inside drwave's ranges (presets load on stock): slots 1-3 = CUTOFF EMPHASIS CONTOUR in bits 6..13
  (0..16383: TE's LFO adds to them) and 6 bits of fields below; slot 4 = 15 bits of fields;
  slots 5-8 = `0x2000 | 12 bits`. The render takes the fields from the sound's stored knobs (found
  by its slots 5-8, which TE's LFO never touches): the LFO's offset on slots 1-4 would scramble
  them in the copy TE hands over. The field table is in `mono.c` (`fields[]`).
- A voice (`mo_core.c`) per TE voice; key events (`on_key` observe, as STATION 1's keyboard) pair a
  key with the voice it starts; its key up releases that voice's contours.
- Per 128-sample block (88.2 kHz): glide (a constant step in log pitch), each oscillator into the
  mixer (polyBLEP at its edges; 16-bit mixer multiplies), noise; per 32 samples both contours and
  the cutoff (in Hz: the exponential part + the contour's Hertz, then the table position); per
  sample the ladder: four one-pole stages, feedback from stage 4 two samples back through a tanh (the self-oscillation's level), the loudness ramped.
- The ladder table (`gen_tables.py`): per semitone the stage coefficient that puts the loop's
  resonance (−180°) on the cutoff, and the loop gain there (the threshold of self-oscillation), so
  EMPHASIS 0.91 oscillates at every cutoff.

## Tests

1. `test_mo_core.c` (host, 51 checks, ASan / UBSan clean): log2 / exp2; every wave in tune, every
   range an octave apart; osc 2 / 3 FREQUENCY steps (±1 cent); polyBLEP (aliasing); the waves'
   harmonics and levels; the filter: 24 dB/oct, self-oscillation on the cutoff law (±20 cents), the threshold (0.88 rings out, 0.95 oscillates), −10 dB bass loss at
   emphasis 5, stable at every cutoff with everything at full; keyboard control; the contour's Hertz; both contours' curves (±8 %) and the release switch;
   glide (constant time, half way at T/2 for one octave and two); every note the same start; every
   parameter through its range moves its effect one way (the filter ones on white noise through the
   filter alone).
2. `sim/run_monotest.py`: drwave untouched, the `mono 3` preset = bank entry 1, tag 2 routing, 441 Hz
   pitch, poly in TE's render order (held notes together, a key up releases its voice), 7 pages,
   fields (clamps, one step per detent, the tag kept), bank with P--, page-7 FX knob, engine-list
   rename, cycles, TE's LFO scrambling slot 3's low bits leaves the sound alone, the 4-voice limit,
   screens.
3. Screens `build/demo/mono_screen.png`, `mono_screen_p5.png`.

Cost (sim, per voice block of 128 samples): a saw, filter open 22.9k; three oscillators, pink noise,
emphasis, contour, keyboard control and a glide 36.8k cycles; 4 sounding = 147k (TRIO's 6 x 25k).
