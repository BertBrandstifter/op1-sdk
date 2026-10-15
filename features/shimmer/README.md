# SHIMMER effect — spec

Status: **v0.1 built, sim-verified, awaiting device test** (2026-10-12). Code: `features/shimmer/`
(in recipe `op1plus`). Idea from JIANT FM's SHIMMER (own code: FDN reverb + delay-line shifter).

## Open it on the OP-1

Part of the default `op1plus` image. Pick **SHIMMER** in the FX list (synth FX, drum FX, or master FX on the mixer's FX page). Knob push = page 2.

## What it is

A reverb whose tail rises an interval on every pass through the tank: play a chord, and the tail
blooms into the octave above, then the one above that, fading as it climbs. Pick **shimmer** in
TE's FX list (synth, drum or master FX; master gets a wide stereo tail).

| page | blue | green | white | orange |
|---|---|---|---|---|
| 1 (TE's FX knobs) | **DECAY** ~0.3 → ~20 s | **TONE** dark → bright | **SHIMMER** none → a lot | **MIX** dry → wet |
| 2 (knob push) | **INTERVAL** +12 / +7 / +19 / +24 / −12 | **SIZE** room → hall | **MOD** still → chorused | **LOWCUT** 40 Hz → 1 kHz |

Page 1 is TE's own spring knobs: saved, LFO-modulated and shown like any FX knob (SHIMMER rescales
spring's ranges). The synth FX screen shows SHIMMER's page (interval, rising sparks as loud as the
tail and as fast as SHIMMER, both pages' values); a knob push flips to page 2. As master FX (mixer, FX page) SHIMMER shows the same page and page 2 works there too; as drum FX
page 2 keeps the stored values (no drum page yet).

## How

- **Host: TE's spring effect** (fx id 4). Spring's Ranges (table `0x01de5c50` + 4 × 0x30):
  p1 1344..16448, p2 7744..16448, p3 0..16384, p4 0..32767, p5..p8 0..32767. A simulator sweep
  (spring's mono + stereo process run with four different p5..p8 sets) gives bit-identical output,
  so spring ignores p5..p8. An FX is SHIMMER when fx_params 5–8 are all `0x4000 | v`
  (16384..24575), v = page-2 value (13 bit). Every stored value is inside TE's ranges: presets pass
  the boot check and load on stock firmware as spring. Plain spring (stock default p5..p8 = 8000,
  no marker) runs TE's process untouched. Spring (not CWO, RESO's host) so both effects can be in
  one image and a SHIMMER preset falls back to a reverb on stock.
- **FX list:** `content_sql` adds `fx_types` row `shimmer` with default_params
  `[9651, 12966, 8192, 13000, 16384, 22118, 18841, 18432]`. The rename "shimmer" → "spring" runs
  in the new shared composer points `fx_name` (call site `0x019ff3f4`) and `master_fx_name`
  (`0x019da9b0`); RESO 0.2 moved its "reso" → "cwo" rename there too (both used to own the sites).
- **Process:** spring's process slots (mono `0x019113d0` → `sh_te_mono`, stereo `0x019113f0` →
  `sh_te_stereo`). Only one mono FX object exists (`0xffb00600`); which sound's params apply comes
  from `TE_PLAYMODE` (0 synth, 1 drum); while TE switches (`TE_PLAYMODE_TARGET` differs) spring
  runs stock. The tank resets when the play mode changes.
- **DSP** (`shimmer_core.c`, 32-bit integer per sample): input DC blocker, −6 dB, two allpass
  diffusers (241, 593 samples, g 0.625) → 4-line FDN (int16 lines of 4096, lengths 2203 / 2591 /
  3011 / 3469 × SIZE 0.25..1, slewed 2 samples per block with per-sample ramps so nothing steps),
  one-pole damping per line (TONE), Householder mix, line gains from RT60 = 0.3 s × 2^(6.06 · DECAY)
  (capped at 0.998), soft knee 4:1 above 20000 on every write. **Shimmer:** line 0's feedback
  (after the mix) goes through a two-tap delay-line shifter (window 2048 samples, triangle
  crossfade, linear interpolation; 2-pole lowpass first at 3.5–8 kHz per interval so the shifted
  top end never folds back, LOWCUT high-pass before it) and is crossfaded power-preservingly with
  the unshifted feedback (cos/sin of SHIMMER × 70°, +1.2 dB for the grain crossfade). Pitch
  shifting moves energy out of the band rather than adding it, so the loop stays at the tank's
  decay: no runaway at any setting (tested). MOD: 0.4 Hz triangle, ±14 samples, lines 90° apart.
  Master: tank on mid, L = line 0 − line 3, R = line 1 − line 2 (correlation −0.4).
- **RAM:** 42 KB per tank, two tanks (mono, master), in the SDK image's .bss.
- **Cost (sim):** 37k cycles per 64-sample block mono, 39k stereo (~25 M cycles/s, ~6 % CPU; RESO
  is 22k). First block after a start also clears the tank (~25k extra once).

## Tests

1. `features/shimmer/test_shimmer_core.c` (23 checks, ASan/UBSan): interval mapping, MIX 0
   bit-exact (mono + stereo), tail short vs long DECAY, wet level, RT60 within ×1.6 of the formula,
   shimmer partials (+12: 880 Hz from a 440 Hz note ×460 in energy; −12: 220 Hz; +7: 659 Hz),
   stability (3 s full-scale noise, every knob full, each interval: 20 s later peak < 2500, then
   silent), DC in, stereo width, SIZE moved during a tail without steps.
2. `sim/run_shtest.py`: plain spring untouched; SHIMMER tail as synth FX; drum play mode reads drum
   params, stock while switching; master both channels, MIX 0 dry, no marker = stock stereo;
   shared rename (`compose_fx_name`: "shimmer" → "spring", "reso" → "cwo", others untouched; master
   point too); page-2 push/edit keeps the marker in range and saves; engine screen / CWO sound not
   claimed; cycles.

Listening demo (host render, same core): `build/demo/shimmer_demo.wav` — chords with SHIMMER 0,
+12, +7, −12, +19 (9 s each).

## Risks (device test)

- FX list now has 9 rows (7 stock + reso + shimmer). Code check (2026-10-12): the list is a
  `std::vector` filled from `SELECT id, type FROM fx_types` (`0x1970a84` via `0x19707d8`), no
  maximum; drawn by the shared PresetBrowser (6 visible text rows, scrolls, name cut to 13 chars,
  no icons); no FX-name lookup before the rename sites. On opening, the browser highlights the
  stored type (spring for a SHIMMER sound; unmatched id → row 0, no assert): cosmetic, like LOFI.
- `TE_PLAYMODE` as the synth/drum selector for the mono FX is inferred (RESO uses CWO's own pos
  field, which spring does not have).
- CPU: 6 % on top of TE; with LOFI + engine_plus + a SHIMMER master FX at once, listen for
  dropouts.
- Power-cycle test with SHIMMER selected (synth FX and master FX) is part of the device test.

**2026-10-14: drum FX screen.** The effect's page now also shows and edits on the drum sound's FX
screen (edit mode 1, drum sub-screen `0x01aab9f4` == 2, type `0xff801754[slot]`, on `0x01aaac30[slot]`,
params `0xff8015c4 + slot * 8`, saved into the drum patch `0x01ddd9a8 + slot * 0x38` through
`0x019ff664`); before, the drum FX screen showed the host effect's own screen.
