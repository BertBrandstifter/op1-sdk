# FXPACK — spec

Status: **v0.7** (2026-10-22: every effect but FUZZ 1.4× louder), sim-verified; v0.6 = COMB, RING MOD, TREMOLO, LADDER, AUTO-WAH, VOWEL, TYPE 32 slots; v0.5 = CHORUS + BITCRUSH, TYPE slots, FOLD/FUZZ drive halved, stepped values as text + bar; v0.4 = TYPE first on page 1, MIX on page 2; v0.2 = one list row. Code: `features/fxpack/`
(in recipe `op1plus`). Ideas from sloop-fm1 (phaser, flanger, wavefolder, fuzz, texture bed),
strudel (DJ filter, distortion curves, transient shaper) — own code; see `docs/ideas.md`.

## Open it on the OP-1

Part of the default `op1plus` image. Pick **fx mix** in the FX list (synth FX, drum FX, or master FX on the mixer's FX page). Blue knob = TYPE (phaser, flanger, chorus, comb, ring mod, tremolo, fold, fuzz, bitcrush, DJ filter, ladder, auto-wah, vowel, transient, texture); knob push flips to page 2 (MIX + the effect's values).

## What it is

Fifteen effects behind **one** entry in TE's FX list, **fx mix** (synth, drum or master FX). Page 1's
blue knob picks the effect; each pick loads that effect's own defaults:

| effect | page 1 (blue = TYPE) | page 2 (knob push) |
|---|---|---|
| **phaser** | TYPE · RATE · DEPTH · FEEDBACK | MIX · CENTER · STAGES 4/6/8 · SPREAD |
| **flanger** | TYPE · RATE · DEPTH · FEEDBACK (− ← centre → +) | MIX · DELAY 0.1–10 ms · DAMP · SPREAD |
| **chorus** | TYPE · RATE · DEPTH (± 0–3 ms) · VOICES 1/2/3 | MIX · DELAY 1.5–20 ms · TONE · SPREAD |
| **comb** | TYPE · FREQ 20 Hz–5 kHz · FEEDBACK (− ← 0 → +, ±0.92) · DAMP | MIX · DEPTH (LFO on FREQ) · RATE · SPREAD (right up to ½ octave higher) |
| **ring mod** | TYPE · FREQ 10 Hz–3.9 kHz · DEPTH (LFO on FREQ) · RATE | MIX · SHAPE sine → square · TONE · SPREAD |
| **tremolo** | TYPE · RATE 0.1–16 Hz · DEPTH · SHAPE sine → square | MIX · SPREAD (0–180°: master = auto-pan) · – · – |
| **fold** | TYPE · DRIVE (1–8×) · BIAS · TONE | MIX · SHAPE triangle → sine · LEVEL · – |
| **fuzz** | TYPE · DRIVE (1–16×) · TONE · CHAR (SOFT HARD DIODE FUZZ CHEBY) | MIX · LEVEL · BIAS · GATE |
| **bitcrush** | TYPE · BITS 1–16 · RATE ~490 Hz–44.1 kHz · JITTER | MIX · TONE · DRIVE (1–4×) · – |
| **djfilter** | TYPE · FILTER (LP ← open → HP) · RESO · DRIVE | MIX · SLOPE 12/24 dB · LFO rate · WOBBLE |
| **ladder** | TYPE · CUTOFF ~60 Hz–16 kHz · RESO · DRIVE (1–4×, starts at unity) | MIX · ENV (input level opens it) · LFO rate · WOBBLE |
| **auto-wah** | TYPE · SENS · RESO · FREQ (start, 40 Hz–2.3 kHz) | MIX · SPEED · MODE LP/BP/HP · DIR UP/DOWN (5 octaves) |
| **vowel** | TYPE · VOWEL A → E → I → O → U · RESO · SHIFT (±1 octave) | MIX · LFO rate · DEPTH (± one vowel) · SPREAD |
| **transient** | TYPE · ATTACK (− ← 0 → +) · SUSTAIN (− ← 0 → +) · OUTPUT | MIX · SPEED · CLIP · – |
| **texture** | TYPE · KIND (VINYL TAPE RAIN WIND RADIO ROOM) · LEVEL · TONE | BREATH · RATE · DENSITY · DUCK |

The effect's first four values are phone's own knobs 1–4 (stored as phone's params 1–4: saved,
LFO-modulated by TE; FXPACK rescales phone's ranges); MIX (BREATH) is phone's knob 4, so TE's LFO
on knob 4 moves MIX. Since v0.4 FXPACK edits all eight knobs itself (TE's knob order no longer
matches the page), clamped to phone's ranges and saved through TE. The synth FX screen shows
FXPACK's page (effect name, a scope of the last output block in one colour that follows its peak:
blue quiet → green → orange full scale; both pages; stepped values — TYPE, VOICES, STAGES, CHAR,
SLOPE, KIND — show their text right of the name and a bar at step / (n − 1), and move one step
per detent, as STATION 1). The blue knob switches the effect, one per click, without going back to the list. As master FX (mixer, FX page) the same page and page 2 (incl.
TYPE) work too; as drum FX page 2 keeps the stored values (no drum page yet). SPREAD (phaser, flanger, chorus, comb, ring mod, tremolo, vowel) offsets the right channel's LFO on the master FX.

## How

- **Host: TE's phone effect** (fx id 3). Ranges: p1 204..20480, p2 3072..17408, p3 1536..16896,
  p4 0..32767, p5..p8 0..32767. Simulator sweep: phone's output (mono + stereo) is bit-identical
  for any p5..p8. An FX is FXPACK when fx_params 5–8 are all `0x6000 | v` (24576..32767): param
  5's v is the TYPE, 6–8 page 2. TYPE = 32 fixed slots (`v >> 10`, table in `fxpack_core.c`,
  copied in enginehost's `eh_ui.c`): every TYPE value ever stored keeps its effect (v0.4's seven
  in slots 2 6 11 16 20 25 29, v0.5's CHORUS 9 and BITCRUSH 19); the new six sit in slot centres
  (COMB 4, RING MOD 13, TREMOLO 14, LADDER 22, AUTO-WAH 23, VOWEL 27); the remaining 17 slots
  fall to a neighbour and can take later effects. All values in range → boot check passes, stock firmware loads
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
- **Output gain:** every effect but FUZZ (loud already) ends in ×1.4, faded in over the first
  quarter of MIX (MIX 0 stays a bit-exact bypass) and on dry and wet alike (notches keep their
  depth), through a soft knee: linear to half scale, cubic up to full scale. TEXTURE's fourth
  knob is BREATH: its bed itself is ×1.4.
- **DSP** (`fxpack_core.c`, tables from `gen_tables.py` → `fp_tables.h`): phaser = 4/6/8
  first-order allpasses (tabled coefficient 80 Hz–10 kHz, ramped per block) with feedback ≤ 0.9;
  flanger = 2048-sample line, interpolated, delay ramped per block, damped ±0.9 feedback; chorus = 1–3
  taps on the same line, triangle LFOs 120° apart, delay ramped per block, no feedback, one-pole
  tone on the wet, MIX dry 1 − m² / wet 2m − m²; comb = the same line at 1/FREQ (16.16, ramped),
  damped feedback, line input soft-limited (x − 4/27 x³); ring = sine carrier (exp pitch, clipped
  toward a square by SHAPE) × input, toned; tremolo = per-sample LFO gain; fold =
  triangle fold (period 4× full scale) blended to a sine fold, DC blocker, tone; fuzz = 5
  257-point curves (tanh, hard, asymmetric diode, fuzz, Chebyshev T3), bias, DC blocker, tone,
  envelope gate; bitcrush = soft-clip drive, sample and hold clocked by a 16.16 phase (JITTER
  clocks early by up to half a period), BITS = rounding to a 2^(15…0) step toward zero (silence
  stays 0), tone; ladder = four TPT one-poles (states 8 bits finer, 32-bit steps from whole-LSB differences) with
  unit-delay feedback ≤ 3.5 scaled by (1 − 1.4 G) so it never rings on, soft limit in the loop,
  half the passband loss made up; auto-wah = Chamberlin SVF (states 8 bits finer, 40 Hz–6.4 kHz)
  swept by a follower with knob attack/release; vowel = three Chamberlin band-passes (input
  pre-scaled by q: peak 1) on interpolated formant tables (A E I O U), ×3 made up (auto-wah BP ×2); DJ = Simper trapezoidal SVF, g tabled 20 Hz–19 kHz (Q16), one divide per block,
  states clamped so Q14 products fit 32 bit, optional second stage; transient = fast/slow
  followers (shift-based), per-block gain 2^(±2) from their ratio, ramped; texture = LCG noise
  shaped per kind (crackle + pops, hiss + flutter, droplets, brown noise in a wandering band-pass,
  narrow band + drifting whistle, rumble), breath LFO, input-follower duck.
- **Cost (sim, worst page-2 settings):** cycles per 64-sample block (output gain included) PHASER 13.1k, FLANGER 9.2k, CHORUS 14.1k,
  COMB 9.3k, RING MOD 7.4k, TREMOLO 7.0k, FOLD 10.2k, FUZZ 8.7k, BITCRUSH 9.8k, DJ 23.4k (24 dB + drive),
  LADDER 15.7k, AUTO-WAH 19.2k, VOWEL 14.9k, TRANSIENT 13.8k, TEXTURE 14.1k → ~1–4 % CPU;
  master stereo ~2×. RAM: 3 states × ~4.3 KB.

## Tests

1. `features/fxpack/test_fxpack_core.c` (65 checks, ASan/UBSan): names/choices; TYPE slots (v0.4
   values keep their effect); MIX 0 (TEXTURE LEVEL 0) bit-exact for all fifteen; stability (full-scale noise, four knob sets, then silence);
   phaser notch depth; right channel differs with SPREAD; flanger notch at 1/(2D); chorus comb notch, 3 voices
   L ≠ R with SPREAD; fold 3rd harmonic; every fuzz curve distorts, gate silences hiss; bitcrush
   1 bit = 3 levels, RATE holds, open ≈ input; comb notch / resonance; ring sidebands; tremolo
   depth and L/R anti-phase; ladder pass / 4-pole stop / resonance; auto-wah opens on loud input;
   vowel A vs I formants; every stepped value's steps map back; DJ LP/open/HP and 24 dB; resonance
   peak; transient attack/sustain up; every texture kind sounds; duck.
2. `sim/run_fptest.py`: stock phone untouched; all fifteen run instead of phone; drum play mode; stock
   while switching; master per channel; all seven names → "phone" (shimmer/reso still renamed);
   page-1 TYPE one per click (FLANGER → CHORUS), clamps, saves, marker kept; stepped values one
   step per detent (STAGES on page 2, FUZZ CHAR in phone's range); screens of CHORUS page 1
   and AUTO-WAH page 2 → `build/demo/fxpack_screen.png`, `fxpack_screen_wah.png`; page-2 MIX and page-1 knobs in phone's
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
