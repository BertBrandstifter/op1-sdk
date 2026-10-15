# WHEEL synth engine — spec

Status: **v0.7** (2026-10-13): cleaner output. The user heard crackle when notes overlapped; measured:
every drive preset hard-clipped (up to 40 % of the samples) and played 2-3x louder than the clean
ones, overloading TE's voice mix in chords, and the percussion attack reached full scale. Now: one
drawbar peaks at 18000 (was 24576), percussion 14400 / 7200, a soft knee instead of the hard clip
(exactly linear up to 3/4 of full scale), drive output x 1 / (1 + 4 d) (same loudness at every
drive setting), key click half as loud. All 24 bank sounds: 0 clipped samples, 5-10k RMS. v0.6:
engine host. v0.5 (2026-10-13): one preset `wheel` in folder `wheel` (was jazz organ + rock organ);
the sounds are in the bank. v0.4 (2026-10-12): page 3 names the FX knobs. v0.3: built-in bank of 24 organ sounds
(shift + blue turn). v0.2: page 3 =
the sound's FX knobs. v0.1 device-tested by the user
("really nice"; asked for reverb/echo control on the engine screen → v0.2). Code: `features/wheel/`
(in recipe `op1plus`, after lofi and before engine_plus). Inspired by JIANT FM's WHEEL engine
(ideas only, own code; registrations are classic drawbar settings).

## Open it on the OP-1

Part of the default `op1plus` image. Pick **WHEEL** in the synth engine list, or load the preset `wheel` (browser folder wheel). Sounds: **shift + blue turn** (bank of 24 organ sounds); knob push cycles pages.

## What it is

A tonewheel organ as a synth engine. TE's envelope, velocity, volume, FX, LFO, glide, bend, octave
and sequencers apply as for any stock engine (use a full-sustain envelope for organ playing).

| page | blue | green | white | orange |
|---|---|---|---|---|
| 1 | **REG** registration (16) | **PERC** percussion (7) | **DRIVE** overdrive | **ROTARY** OFF / SLOW / FAST |
| 2 | **BASS** 16' + 5⅓' −8..+8 | **BODY** 8' + 4' −8..+8 | **TOP** 2⅔' .. 1' −8..+8 | **CLICK** key click |
| 3 | FX knob 1 | FX knob 2 | FX knob 3 | FX knob 4 |

- **Page 3 names** (v0.4), from TE's OP-1 guide, effects reference: delay SIZE SPEED FEEDBACK MIX,
  grid X SIZE / Y SIZE / Z FEEDBACK / MIX, punch FREQ PUNCH ROUNDS POWER, phone TONE PHONIC BAUD
  TELEMATIC, spring TONE TURNS DAMPING SEND, nitro FREQ FOLLOW RES FREQ 2 (guide text garbled
  there; knob 2's bipolar range fits FOLLOW), cwo FREQ DELAY FEEDBACK SIDEBAND; RESO, SHIMMER and
  the seven FX MIX types by their own page 1. A simulator sweep of TE's FX could not confirm them
  (the stock image's wet paths stay silent without boot state; only "4th knob = MIX" on grid,
  punch and phone showed up).
- **Page 3** edits the sound's FX knobs, the same four values as TE's FX screen (T3): spring
  reverb, delay echo or whatever FX is loaded (SHIMMER / RESO / FX MIX by name). Each value stays
  inside TE's range for that FX type (table `0x01de5c50` + type·0x30, {min, max, ?} per knob, read
  live), so the boot check stays happy; saved through TE's FX save (`0x019ff664`). FX type and
  on/off stay on T3 (page 3 says so when the FX is off). FX ranges (TE's Ranges init in the
  simulator): delay 1024..11264 / 3276..32767 / 0..16384 / 0..32767; spring 1344..16448 /
  7744..16448 / 0..16384 / 0..32767; grid, punch, phone, filter, nitro, cwo see `wh_te246.h`
  comment and the bench.

- **REG** (one digit per bar, 16' 5⅓' 8' 4' 2⅔' 2' 1⅗' 1⅓' 1'): FLUTE 008000000, SOFT 006600000,
  BALLAD 808000000, JAZZ 888000000, BLUES 888800000, SMOOTH 868500000, CLARINET 008080800,
  GOSPEL 888808008, ROCK 888400008, SHRILL 800000888, REED 008866444, THEATRE 878600046, SPARKLE
  006008008, BRIGHT 888888000, HIGH 000888888, FULL 888888888. Bars are 3 dB a step.
- **BASS / BODY / TOP**: centre = the registration as is; left pushes the group's bars in, right
  pulls them out (clamped to 0..8). The screen shows the resulting nine drawbars.
- **PERC**: OFF, 2ND, 3RD, 2ND SOFT, 3RD SOFT, 2ND SLOW, 3RD SLOW. As on the organ: percussion uses
  the 1' bar's contact (1' goes silent), normal volume drops the other bars ~3 dB, fast decay τ
  0.12 s, slow 0.5 s. Unlike the organ it retriggers on every note (TE's release tails would make
  single-trigger unpredictable).
- **DRIVE**: gain 1..8 into a cubic soft clip (after the rotor; the organ drives before it).
- **ROTARY**: partials below 800 Hz go to the drum, above to the horn (exact: each bar is one sine).
  Horn 0.83 / 6.75 Hz, level swing 0.45, pitch ±0.35 %; drum 0.67 / 5.9 Hz, 0.25, ±0.15 %. Speeds
  ease (horn τ ~0.6 s, drum ~3 s), so SLOW ↔ FAST spins up and down; OFF brakes to a stop and
  bypasses the rotor.
- **CLICK**: high-passed noise, τ 3 ms, at note on.

## Turning it on

- **Engine list**: pick **wheel** in TE's engine browser. `content_sql` adds a `synth_types` row
  `wheel` with WHEEL's defaults (JAZZ, 3RD, SLOW, a little click); when the row is picked TE
  reads its type name (call site `0x01940374`, now the shared composer point `engine_name`, also
  used by LOFI) and WHEEL rewrites "wheel" to "drwave".
- **Preset browser**: folder `wheel` with one preset **wheel** (v0.5; JAZZ, 3RD, SLOW, spring).
  Rule for every new engine: one preset in its own folder, the sounds in a built-in bank. TE asserts at most 176 non-sampler factory presets (`patch.cpp:616`, count of
  `synth_presets` rows, compare `<= 0xb0` at `0x01a0582a`; user presets are in `synth_user` and do
  not count). Stock has 160, LOFI 14, WHEEL 2 = **176: the list is now full**. Further engines need
  room (e.g. fewer LOFI presets).
- **Built-in bank (v0.3)**: on a WHEEL sound, **shift + blue turn** steps through 24 sounds (JAZZ
  TRIO, SOUL JAZZ, BLUES BAR, GOSPEL, CATHEDRAL, BALLAD, FLUTE, SOFT PAD, ROCK, OVERDRIVE, SHRILL,
  REED, CLARINET, THEATRE, SPARKLE, ORGAN BASS, SMOOTH, BRIGHT, HIGH, SKA, GRIT, DREAMY, TOY,
  FULL ON), one per knob event, wrapping. Loading writes all 8 values (saved like a knob edit);
  envelope and FX stay as they are. The header shows the entry's name (`*` once edited). No TE
  preset rows: TE's factory list is capped at 176. LOFI got the same (its 14 factory presets).
- No on/off gesture: shift + white push is LOFI's (non-modal claim) and claims cannot be scoped to
  an engine. Turn WHEEL off by picking another engine or preset.
- On a WHEEL sound **any knob push** cycles page 1 → 2 → 3; the knobs edit WHEEL's values (saved in the
  preset), shift + knob does nothing. The engine screen shows WHEEL's page (drawbars, settings,
  knobs) above y 140; engine_plus' knob row (if any of its knobs is flipped) stays visible below.

## How

- **Host: TE's drwave engine.** A drwave sound is WHEEL when knob slots 5–8 all hold
  `0x5000 | v` (v = 0..4095, page 2); page 1 lives in slots 1–4 as 0..16383. Drwave's ranges
  (TE's Ranges init `0x01a1e914` run in the simulator): slot 1 0..24575, 2 0..16383, 3 0..16383,
  4 0..32767, 5–8 0..32767, so **every WHEEL value is in range**: presets pass TE's boot-time patch
  validator without a range-check hook and load on stock firmware as drwave. Stock drwave presets
  have slot 5 = 32000/32767 and slots 6–8 = 0, so none looks marked (checked: 0 of 16).
- **Voice contract**: as LOFI (`features/lofi/README.md`): 128 samples at 88.2 kHz, pitch from TE's
  per-sample increments at `0xff801950`, TE's ADSR after render. Drwave vtable `0x0190fb2c`:
  render slot `0x0190fb40` (stock `0x0197c870`) → `wh_te_render`, note_on slot `0x0190fb48`
  (`0x0197a826`) → `wh_te_note_on`. 6 voices of 0x24 bytes from `0xffb00000` (ctor `0x0197ada8`).
- **DSP** (`wheel_core.c`): additive, one sine per bar. All bars are integer multiples of the 16'
  phase (×1 3 2 4 6 8 10 12 16), one accumulator per rotor group. Bars above ~6 kHz fold down an
  octave (as the top tonewheels do). Sine = parabola + one correction step in fract16 (error
  ~0.1 %, h2 −88 dB). The voice is computed at 44.1 kHz and interpolated to 88.2 kHz (TE's 2×
  decimation removes the images). Mix gain = 1 / (0.75 S + 0.25) for S full bars. The rotor
  advances once per audio block (a new block = the voice index did not rise).
- **Cost** (sim, per 128-sample voice block): FLUTE 11.9k cycles, FULL 19.1k, FULL + percussion +
  click + rotary + drive 24.2k (TE's voltage: 16.7k in the same simulator). Per-block set-up is
  32-bit only: 64-bit divides were library calls costing more than the voice itself.

## Tests

1. `features/wheel/test_wheel_core.c` (32 checks, ASan/UBSan clean): zones/offsets, clean sine at
   pitch and level, all nine bar ratios at equal level, no 7th harmonic, CLARINET odd harmonics,
   foldback, glide, percussion 2nd/3rd/soft/slow + 1' cut + −3 dB, click only in the first ms,
   rotary swing (fast) vs steady (off), drive harmonics, repeatable notes, extremes.
2. `sim/run_wheeltest.py`: plain drwave untouched (render, knob, push), note_on, WHEEL pitch from TE's
   buffer (441 Hz), restart, paging hidden from engine_plus/LOFI, page-2 edits keep the marker,
   page-1 clamps at drwave's ranges, shift + knob, other screens, every stored value inside
   drwave's ranges, rename chained with LOFI's, cycle counts.
3. Demo: `build/demo/wheel_demo.wav` (jazz chords, rotary slow → fast → slow, rock organ).

## Device test

1. Engine list → **wheel**: the WHEEL screen (drawbars + JAZZ / 3RD / SLOW). Play: organ with
   percussion and a slow rotor.
2. Orange knob to FAST: the rotor spins up over ~1–3 s; back to SLOW spins down.
3. Knob push → page 2: BASS/BODY/TOP move the drawbars on screen; CLICK adds the tick.
4. Preset browser → folder **wheel** → jazz organ, rock organ.
5. **Power-cycle with a WHEEL sound as the current sound** (TE validates stored sounds at boot).
6. Listen for crackles with 6-note chords + rotary + drive + a heavy FX (CPU headroom).

## Risks

- The new-block detection assumes TE renders voices in index order once per block (rotor speed
  is wrong otherwise, sound still fine).
- Drwave's own knob screen behaviour under shift is untested on a WHEEL sound (we consume shift +
  knob; shift + push passes through to TE/drwave).
- TE's LFO adds to slots 1–4 (REG/PERC/DRIVE/ROTARY), clamped to 0..16383: an LFO on ROTARY
  switches the rotor speed.
