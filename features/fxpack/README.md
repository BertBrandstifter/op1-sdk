# FXPACK — spec

Status: **v0.4** (2026-10-13: TYPE first on page 1, MIX on page 2), sim-verified; v0.2 = one list row. Code: `features/fxpack/`
(in recipe `op1plus`). Ideas from sloop-fm1 (phaser, flanger, wavefolder, fuzz, texture bed),
strudel (DJ filter, distortion curves, transient shaper) — own code; see `docs/ideas.md`.

## What it is

Seven effects behind **one** entry in TE's FX list, **fx mix** (synth, drum or master FX). Page 1's
blue knob picks the effect; each pick loads that effect's own defaults:

| effect | page 1 (blue = TYPE) | page 2 (knob push) |
|---|---|---|
| **phaser** | TYPE · RATE · DEPTH · FEEDBACK | MIX · CENTER · STAGES 4/6/8 · SPREAD |
| **flanger** | TYPE · RATE · DEPTH · FEEDBACK (− ← centre → +) | MIX · DELAY 0.1–10 ms · DAMP · SPREAD |
| **fold** | TYPE · DRIVE · BIAS · TONE | MIX · SHAPE triangle → sine · LEVEL · – |
| **fuzz** | TYPE · DRIVE · TONE · CHAR (SOFT HARD DIODE FUZZ CHEBY) | MIX · LEVEL · BIAS · GATE |
| **djfilter** | TYPE · FILTER (LP ← open → HP) · RESO · DRIVE | MIX · SLOPE 12/24 dB · LFO rate · WOBBLE |
| **transient** | TYPE · ATTACK (− ← 0 → +) · SUSTAIN (− ← 0 → +) · OUTPUT | MIX · SPEED · CLIP · – |
| **texture** | TYPE · KIND (VINYL TAPE RAIN WIND RADIO ROOM) · LEVEL · TONE | BREATH · RATE · DENSITY · DUCK |

The effect's first four values are phone's own knobs 1–4 (stored as phone's params 1–4: saved,
LFO-modulated by TE; FXPACK rescales phone's ranges); MIX (BREATH) is phone's knob 4, so TE's LFO
on knob 4 moves MIX. Since v0.4 FXPACK edits all eight knobs itself (TE's knob order no longer
matches the page), clamped to phone's ranges and saved through TE. The synth FX screen shows
FXPACK's page (effect name, a scope of the last output block in one colour that follows its peak:
blue quiet → green → orange full scale; both pages; stepped values as
text). The blue knob switches the effect, one per click, without going back to the list. As master FX (mixer, FX page) the same page and page 2 (incl.
TYPE) work too; as drum FX page 2 keeps the stored values (no drum page yet). SPREAD (phaser, flanger) offsets the right channel's LFO on the master FX.

## How

- **Host: TE's phone effect** (fx id 3). Ranges: p1 204..20480, p2 3072..17408, p3 1536..16896,
  p4 0..32767, p5..p8 0..32767. Simulator sweep: phone's output (mono + stereo) is bit-identical
  for any p5..p8. An FX is FXPACK when fx_params 5–8 are all `0x6000 | v` (24576..32767): param
  5's v is the TYPE, 6–8 page 2. All values in range → boot check passes, stock firmware loads
  the sound as phone. (Markers: RESO `0x2000` on CWO, SHIMMER `0x4000` on spring, FXPACK
  `0x6000` on phone.)
- **FX list:** `content_sql` adds one `fx_types` row `fx mix` (PHASER's defaults + TYPE 0);
  renamed to "phone" in the shared `fx_name` / `master_fx_name` points. When a browser asks which
  row is current, a marked phone sound answers "fx mix" (`fx_current_name` /
  `master_fx_current_name`), so the cursor stays and a confirm keeps the effect. Per-effect
  defaults live in `fxpack.c` (`defaults[]`), loaded on a TYPE change.
- **Process:** phone's slots (mono `0x01911308` → `fp_te_mono`, stereo `0x01911328` →
  `fp_te_stereo`); synth vs drum by `TE_PLAYMODE` as SHIMMER; master = one state per channel. A
  state resets when its effect changes.
- **DSP** (`fxpack_core.c`, tables from `gen_tables.py` → `fp_tables.h`): phaser = 4/6/8
  first-order allpasses (tabled coefficient 80 Hz–10 kHz, ramped per block) with feedback ≤ 0.9;
  flanger = 2048-sample line, interpolated, delay ramped per block, damped ±0.9 feedback; fold =
  triangle fold (period 4× full scale) blended to a sine fold, DC blocker, tone; fuzz = 5
  257-point curves (tanh, hard, asymmetric diode, fuzz, Chebyshev T3), bias, DC blocker, tone,
  envelope gate; DJ = Simper trapezoidal SVF, g tabled 20 Hz–19 kHz (Q16), one divide per block,
  states clamped so Q14 products fit 32 bit, optional second stage; transient = fast/slow
  followers (shift-based), per-block gain 2^(±2) from their ratio, ramped; texture = LCG noise
  shaped per kind (crackle + pops, hiss + flutter, droplets, brown noise in a wandering band-pass,
  narrow band + drifting whistle, rumble), breath LFO, input-follower duck.
- **Cost (sim, worst page-2 settings):** cycles per 64-sample block PHASER 10.8k, FLANGER 6.4k,
  FOLD 7.7k, FUZZ 8.2k, DJ 20.4k (24 dB + drive), TRANSIENT 11.9k, TEXTURE 13.1k → ~1–3.5 % CPU;
  master stereo ~2×. RAM: 3 states × ~4.3 KB.

## Tests

1. `features/fxpack/test_fxpack_core.c` (36 checks, ASan/UBSan): names/choices; MIX 0 (TEXTURE
   LEVEL 0) bit-exact for all seven; stability (full-scale noise, four knob sets, then silence);
   phaser notch depth; right channel differs with SPREAD; flanger notch at 1/(2D); fold 3rd
   harmonic; every fuzz curve distorts, gate silences hiss; DJ LP/open/HP and 24 dB; resonance
   peak; transient attack/sustain up; every texture kind sounds; duck.
2. `sim/run_fptest.py`: stock phone untouched; all seven run instead of phone; drum play mode; stock
   while switching; master per channel; all seven names → "phone" (shimmer/reso still renamed);
   page-1 TYPE one per click, clamps, saves, marker kept; page-2 MIX and page-1 knobs in phone's
   ranges (clamped); engine screen / spring sound not
   claimed; per-effect cycles.

Listening demo (host render, same core): `build/demo/fxpack_demo.wav` — a 120 bpm loop dry, then
through each effect (5 s each).

## Risks (device test)

- Synth vs drum by `TE_PLAYMODE` is inferred (as SHIMMER).
- 16-row FX list (code-checked, not seen on device).
- Power-cycle with an FXPACK effect selected as synth FX and as master FX.

**2026-10-14: drum FX screen.** The effect's page now also shows and edits on the drum sound's FX
screen (edit mode 1, drum sub-screen `0x01aab9f4` == 2, type `0xff801754[slot]`, on `0x01aaac30[slot]`,
params `0xff8015c4 + slot * 8`, saved into the drum patch `0x01ddd9a8 + slot * 0x38` through
`0x019ff664`); before, the drum FX screen showed the host effect's own screen.
