# LOFI synth engine — spec

Status: **v0.7** (2026-10-13): one preset `lofi` in folder `lofi` (the preset browser entry is
back; the sounds are in the bank), shift + white push gesture removed. v0.6 (2026-10-12): the 14 presets moved into a built-in bank (shift + blue turn on a LOFI
sound; frees 14 of TE's 176 factory preset slots). v0.5: 14 LOFI presets in the preset browser (folder lofi); v0.4: LOFI in the engine list (T1). Earlier: v0.1 device-verified ("works perfectly"), but a LOFI sound left as the
current sound halted the next boot (fatal `patch.cpp:390 Invalid synth patch pulse:… error 15`);
v0.2 fixed only slots 5–8 and still halted; v0.3 covers all slots (see "Boot fix"), awaiting device test. Code: `features/lofi/`
(in recipe `op1plus`, before engine_plus). Inspired by JIANT FM's LOFI engine (ideas only, own code;
the bytebeat formulas are classic community one-liners).

## Open it on the OP-1

Part of the default `op1plus` image. Pick **LOFI** in the synth engine list, or load the preset `lofi` (browser folder lofi). Sounds: **shift + blue turn** (bank of 14); knob push flips pages.

## What it is

Chiptune waves and pitch-tracking bytebeat as a synth engine. TE's envelope, velocity, volume, FX,
LFO, glide, bend, octave and sequencers apply as for any stock engine.

| page | blue | green | white | orange |
|---|---|---|---|---|
| 1 | **WAVE** (25 positions) | **SHAPE** (per wave, below) | **CRUSH** sample-and-hold | **BITS** 16 → 1 |
| 2 | **BEND** phase distortion / byte `t ^ t >> s` | **ARP** (chip) / **LOOP** (byte) | **SWEEP** pitch drop | **TONE** lowpass |

0 = off for everything except WAVE/SHAPE (0 = PULSE, 50 %).

- **WAVE:** PULSE, TRIANGLE (16-step, NES), SAW, NOISE (15-bit LFSR, 8× pitch), METAL (93-step
  short LFSR clocked at 93× pitch: the loop plays the note), WAVE A–D (32-sample 4-bit tables, Game
  Boy style), BYTE 1–16.
- **SHAPE:** pulse DUTY 50 % → 3 %; TRI/SAW STEPS (tri 16 → 2 levels, saw smooth → 64 → 4); noise
  COLOR (noise clock ×1 → ×4); wavetable MORPH into the next table; byte VAR (t xor pattern).
- **BYTE:** t runs at 32 × the played pitch (C4 → ~8.4 kHz), so `t & 255` is the key three octaves
  down and every formula transposes with the keyboard. Notes restart t (each note sounds the same).
  LOOP repeats a window of 2^17 … 2^5 t-steps (short windows turn the pattern into a pitched tone).
  8-bit output, smoothed by a ~6 kHz one-pole.
- **ARP:** 0 / +12, 0 / +7, major, minor, 0 / +7 / +12, 0 / +12 / +24, 0 / +5 / +7 / +12, one step every
  ~17 ms (NES-style chord arpeggio from one key).
- **SWEEP:** pitch falls from note start (up to −1.6 % per 1.45 ms), stops at 1/64.

## Turning it on

- **Preset browser (v0.5):** TE has two synth browsers. The preset browser (synth key) builds its
  columns only from preset *folders* (0x1a02790 / 0x1a185f4), so v0.4's engine row never showed
  there. v0.5 adds 14 factory presets in folder `lofi` (`content_sql` into `synth_presets`):
  chip lead, nes arp, nes minor, tri bass, saw grit, chip noise, metal bell, gb wave, gb morph,
  byte one, byte loop, byte xor, byte tone, sweep drum. Each is a stock pulse preset's adsr/FX/LFO
  with type `pulse`, LOFI page 1 in slots 1-4 and the marker (~v) in slots 5-8. TE validates every
  preset at boot (non-sampler factory presets ≤ 176, fatal assert; now 174) and on selection;
  LOFI's range-check replacement lets the marker through. Names are unique (TE finds the current
  sound by type + name).

- **Engine list (v0.4):** pick **lofi** in TE's engine browser like any engine. The update's
  `content/op1_factory.db` gets a `synth_types` row `lofi` whose `default_params` are LOFI's
  defaults with the marker (`[0,0,0,0,-1,-1,-1,-1]`, added by the feature's `content_sql`). When
  a row is picked TE reads its type name (`SELECT type FROM synth_types WHERE id=%i`, call site
  `0x01940374`); LOFI rewrites "lofi" to "pulse" right there (since 2026-10-12 through the shared composer point
  `engine_name`, which WHEEL uses too), so the sound is a pulse sound with
  LOFI's marker and TE never sees an unknown type. The lofi row has no presets of its own; saved
  LOFI sounds land under pulse. Known cosmetic gap: for a current LOFI sound the browser highlights
  pulse.
- **Gesture (removed in v0.7):** shift + white knob push used to toggle LOFI on a pulse sound;
  LOFI now comes only from TE's lists (engine row, preset `lofi`).
- On a LOFI sound **any knob push** flips page 1/2, the knobs edit LOFI's values (saved in the
  preset like any knob), shift + knob does nothing. The engine screen shows LOFI's own page: wave
  name, a scope of the newest note, the four knobs of the page and the other page dimmed.
  Envelope / FX / LFO screens are TE's.

## How

- **Host: TE's pulse engine.** A pulse sound is LOFI when knob slots 5–8 are all negative; they hold
  page 2 as `~v` (v = 0…32766, −32768 never stored). Stock pulse presets never have negative slots
  5–8. The preset stays `"type":"pulse"`: it loads on stock firmware (as pulse, unverified that pulse
  ignores slots 5–8) and never hits TE's fatal "Invalid synth patch" assert. Picking another
  engine or preset resets the knobs, so the marker cannot leak.
- **Voice contract (R246 RE, docs in `lf_te246.h`):** voices render 128 samples per 64-frame audio
  block = **88.2 kHz** (2× oversampled). Pitch is not in the voice object: TE fills
  `int32 inc[128]` at `0xff801950` (phase increment per sample, 2^32 = one cycle) before each render,
  with glide, bend, octave, tune and pitch LFO applied. TE's ADSR × velocity × volume multiplies
  the rendered block afterwards and ends the voice; render just writes a raw waveform.
- **Hooks:** pointer redirects of pulse's vtable `0x019111cc`: render slot `0x019111e0` (stock
  `0xffa04b88`, encrypted L1: called, not read) → `lf_te_render`; note_on slot `0x019111e8`
  (`0x01a0dee4`) → `lf_te_note_on` (TE's note_on, then "fresh note" for render). Shared `on_key` /
  `on_knob` (filters, before engine_plus) and `frame`. Per-voice state in our own table indexed by
  `(self − 0xffb00000) / 0x60`.
- **Cost (sim, per 128-sample voice block):** PULSE 6.5k cycles, BYTE with every stage on 11.6k
  (TE's voltage render: 16.7k in the same simulator). Per-sample work is 32-bit, no divides.

## Boot fix (v0.2)

TE validates every stored synth sound on load, boot included (`0x01a037a8`): for each of the 8
knobs it calls the range check `0x01a1deb0(type, slot, &value)` (table `0x01de5890 + type·64 +
slot·8` = int16 min/max) and returns error 15 on the first value out of range, which the caller
turns into the fatal assert. Editing knobs at runtime never re-validates, which is why v0.1
worked until the next boot. Pulse's ranges (read by running TE's Ranges init `0x01a1e914` in the
simulator; columns min..max): slot 1 7808..23168, 2 512..16384, 3 0..16384, 4 −16384..16384,
5–8 0..32767. LOFI breaks both halves: page 1 uses 0..32766 in slots 1–4 (turning LOFI on writes
0 to slot 1), page 2 negatives in 5–8. v0.2 exempted only slots 5–8 and still halted. v0.3
replaces the range check (entry hook, same table and result for everything else): a pulse sound
passes with 0..32767 in slots 1–4 and −32767…−1 in slots 5–8. Turning LOFI off clamps slots 1–4
back into pulse's ranges, so pulse never renders values it does not expect.

Consequence: **a LOFI sound must not reach stock firmware** (or an image without LOFI): its
validator rejects it the same way. Before flashing stock, turn LOFI off on every pulse sound
(since v0.7: pick another engine or preset on every LOFI sound). Planned: an in-range marker encoding, so presets also load on stock.

## Tests

1. `features/lofi/test_lofi_core.c`: every WAVE position, pitch of each periodic wave, per-sample
   increments followed, level, duty, 16-step triangle, bytebeat t rate + octave, all formulas
   audible, LOOP, BITS, CRUSH, SWEEP, ARP, TONE, BEND, repeatable notes, extremes (ASan/UBSan clean).
2. `sim/run_lofitest.py`: plain pulse untouched; LOFI pitch from TE's buffer (441 Hz exact); note restart;
   gesture on/off only on pulse; page push hidden from engine_plus; knob edits + TE save; envelope
   screen untouched; cycle counts.

## Risks (device test)

- TE saves negative values in slots 5–8 (seen on the device) but its validator rejects them (fixed
  in v0.2, see above); pulse ignores slots 5–8 when LOFI is off on stock (unverified); LFO targets only slots 1–4 (page 2 then is not
  modulated); level vs stock engines (LF_LEVEL 24576).
- Naive (aliasing) waves are intended lo-fi; at 88.2 kHz TE's decimation removes only content above
  ~22 kHz.
