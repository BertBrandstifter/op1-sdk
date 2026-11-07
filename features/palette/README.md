# PALETTE synth engine — spec

Status: **v0.1** (2026-11-07): first build, sim-verified (host unit tests + simulator bench); not yet
played on the device (the sound of the bank is unheard: listen before calling it done). Code: `features/palette/` (engine host, extended tag 9). Own code and data:
no Roland waveforms or factory patches are included.

A layered digital synth in the style of the Roland **JD-08** (the JD-800 in a Boutique box): every
sound (a *patch*) is up to **four tones A-D** played together, each a full synth voice: a wave
generator (WG) with pitch envelope, a filter (TVF: LPF 24 dB, BPF, HPF) with its envelope, an
amplifier (TVA) with its envelope, both patch LFOs on pitch / cutoff / level. The OP-1's four knobs
are the JD's **palette**: each page is one parameter row, blue / green / white / orange = tone A /
B / C / D.

## Open it on the OP-1

Part of the default `op1plus` image. Pick **PALETTE** in the synth engine list, or load the preset
`palette` (browser folder palette). Patches: **shift + blue turn** (P-- + the bank); knob push
cycles pages.

## What it is

| page | blue (tone A) | green (B) | white (C) | orange (D) |
|---|---|---|---|---|
| 1 LEVEL | 0 .. 100 (0 = tone off) | | | |
| 2 WAVE | one of 24 waves | | | |
| 3 PITCH | −24 .. +31 semitones (32 steps) | | | |
| 4 CUTOFF | −35 .. +40 on the patch's cutoff | | | |
| 5 TIME | ×¼, ½, 0.7, 1, 1.4, 2, 3, 4 on the patch's TVF + TVA envelope times | | | |
| 6 | the sound's FX knobs (engine host) | | | |

- **Patch** (bank entry): defines all four tones completely (below); the palette pages change
  level, wave, pitch, cutoff and envelope time per tone on top of it. Picking a patch writes its
  own levels / waves / pitches and zero offsets.
- **Waves** (24): live band-limited (polyBLEP) SAW, FAT SAW (two saws ±8 cents), SQUARE, PULSE 25,
  PULSE 12, PWM (width 10 .. 90 % moved by LFO 1, ramped per sample); additive single cycles from
  mip-mapped tables (one level per octave, harmonics below 20 kHz; below 43 Hz the next level's, to
  10 kHz; 53 KB): TRIANGLE, SINE, DIGI 1, DIGI 2, WIRE, VOX A, VOX O, ORGAN, E.PIANO, CLAV, SYN BASS,
  GLASS, REED, BRASS, SOFT PAD, SPECTRAL; noise WHITE, PINK. Measured: inharmonic content below
  20 kHz under −45 dB (tables, at 1 / 3 / 6 kHz) and −40 dB (polyBLEP, to 3 kHz). Attack transients and wind sounds are
  made the JD way: a noise tone through the TVF with a short TVA envelope; bells from sine / glass
  tones at non-integer intervals.
- **Tone** (per patch, per tone): wave, level, coarse (palette PITCH) + fine (cents), key follow;
  pitch envelope (L0 → T1 → L1 → T2 → 0); TVF mode, cutoff 0 .. 100 (20 Hz .. 14 kHz), resonance
  0 .. 100, key follow (around C4), envelope depth ±50 with T1 L1 T2 L2 T3 SUS T4 L4; TVA level, envelope
  T1 L1 T2 L2 T3 SUS T4 (→ 0 after the key is up); LFO depths on pitch / TVF / TVA, each from LFO 1
  or 2.
- **LFO 1 / 2** (per patch, running per note): rate 0.1 .. 51 Hz, delay, fade in / out, wave TRI /
  SAW / SQR / S&H / RND (smooth random), offset − / 0 / +, key trigger (off = a random start).
- **TVF**: Chamberlin state-variable filter at 88.2 kHz (cutoff capped at 14 kHz), states Q31
  saturating (a soft ceiling under high resonance), single-cycle fractional ops on the Blackfin;
  LPF = a second SVF without resonance after it (24 dB); BPF normalised to unity at its peak.
- **Envelope times** 0 .. 100: 0 ms .. 20 s, exponential (50 ≈ 140 ms, 70 ≈ 1 s).
- **Key up**: the TVF / TVA envelopes release (T4) on the key's up; notes without a key (sequencer,
  MIDI) hold their sustain until TE's envelope ends them (the preset sets full sustain, a long
  release so the tone's own release is heard).
- **Polyphony counts tones** (as the JD's): a budget of 12 tones, so the newest 12 / (tones at
  LEVEL > 0) notes sound: 1-2 tones 6 notes, 3 tones 4, 4 tones 3 (an older note fades out within
  one audio block); tones at level 0 cost nothing. Cost per note ~11k cycles per filtered tone
  (bench: 4-tone VOX CHOIR 46k, x 3 notes = 138k, MONO 3's 4 x 37k = 148k).
- Not done (the JD-08 has them): velocity (the OP-1's keys have none), TVA bias, pitch random, LFO
  delay REL, the JD's own effects (TE's FX page instead), the arpeggiator / sequencer.
- Page 1 lives in the upper bits of knob slots 1-4, so **TE's LFO can move the tone levels**
  (crossfades between tones, as the JD's palette sliders).

## Storage (all inside drwave's ranges: presets load on stock firmware as drwave)

- Slots 1-4 (tone A-D): LEVEL in bits 9..13 (5 bits, LFO-able), 9 bits of fields below (read from
  the stored knobs): WAVE (5) + CUTOFF (4).
- Slots 5-8: extended tag 9 (slots 5-7 `0x7000`, slot 8 `0x2000`) | 12 bits: slot 5-8 = tone A-D
  PITCH (5) + TIME (3) + 4 bits: slot 5 patch index bits 0-3, slot 6 patch index bit 4, slots 7-8 spare.

## Screen

Dot-matrix display look (`eh_dots.c`), param rows below unchanged:

- A green LCD 20 × 2: line 1 the patch (P07 GLASS BELLS, * = edited) or the knob turned last
  (CUTOFF B +15), line 2 the page's value of the four tones in four columns (SAW  VOXA DIG1 SINE).
- Under it the page names (the current one lit), the key sounding (KEY A3) and the patch's name.
- Four palette sliders A-D to the right (antialiased, `eh_aa.c`): the page's values, a lamp per tone
  (lit while a note sounds and the tone's level > 0).
- Below: the page's four knobs (TONE A .. D), name scale 2 at y 99, 72 × 6 bar at y 112,
  "PUSH: next" at y 125.

## Bank (shift + blue turn)

P-- (the sound as it was) and 22 own patches: DIGI PAD (the preset), GLASS BELLS, STRINGS, SYNTH
BRASS, DIGI PIANO, ORGAN, SAW BASS, SQUARE LEAD, VOX CHOIR, WIND, KALIMBA, SWEEP PAD, CLAV, PLUCK,
WIRE HARP, REED, SPACE BELL, ZAP BASS, SOFT KEYS, BREATH PAD, PWM POLY, SYN BASS. Bells and kalimba
are sine / glass tones at non-integer intervals (PITCH + fine); attacks and breath are noise tones
through the TVF with short TVA envelopes.

## Tests

- `test_pl_core.c` (host): aliasing of every wave at 1 / 3 / 6 kHz; sweeps of every palette
  parameter (LEVEL, CUTOFF, PITCH, TIME, every WAVE sounds) and of the tone parameters (cutoff in
  LPF / BPF / HPF, resonance, TVF envelope depth, TVF key follow, TVA sustain, fine, pitch envelope,
  LFO rate, pitch key follow 0); release; the whole bank at C2 / C4 / C6 (sounds, peaks ≤ 28100,
  ≤ 1 % of samples limited). Mutations checked: no mip levels, reversed CUTOFF / resonance, no
  release, inverted TVA all fail it.
- `sim/run_palettetest.py` (Blackfin simulator, the op1plus features + palette): see its docstring;
  screens to `build/demo/palette_screen*.png`.

## Plan / build steps

1. Engine host v1.4 extended tags (shared with ACID 3, done).
2. `gen_tables.py` → `pl_tables.h`: wave tables (additive single cycles, mip-mapped), exp2 / log2,
   SVF coefficients, envelope time rates, pitch-step table.
3. `pl_core.c` (pure C): tone render (WG, pitch env, TVF state-variable filter (LPF = two in a
   row), TVA), envelopes, LFOs, voice render of up to 4 tones; patch structs.
4. `test_pl_core.c`: every wave band-limited at high notes (no energy above 40 kHz), sweeps (cutoff,
   resonance, level, time, pitch move one way), envelopes reach their levels, release on key up,
   level bounds for the whole bank.
5. `palette.c`: glue, poly voice handling (4 newest), key observer (release), pages, bank, screen.
6. `sim/run_palettetest.py` + `driver.c`; recipe; pack; docs.
7. Device test (user), incl. power-cycle with a PALETTE sound stored.
