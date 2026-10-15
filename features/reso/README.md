# RESO effect — spec

Status: **v0.1 built, sim-verified, awaiting device test** (2026-10-12). Code: `features/reso/`
(in recipe `op1plus`). Idea from JIANT FM's RESO reverb (own code).

## Open it on the OP-1

Part of the default `op1plus` image. Pick **RESO** in the FX list (synth FX, drum FX, or master FX on the mixer's FX page). Knob push = page 2.

## What it is

Four resonating "strings" (tuned feedback combs) that ring at the notes the synth is playing — any
source: keyboard, MIDI, sequencers, arpeggiator. When nothing plays, they keep their last chord and
ring out. Pick **reso** in TE's FX list (synth, drum or master FX).

| page | blue | green | white | orange |
|---|---|---|---|---|
| 1 (TE's FX knobs) | **DECAY** short → ~20 s | **TONE** dark → bright | **CHORD** voicing | **MIX** dry → wet |
| 2 (knob push) | **GLIDE** instant → ~1.5 s | **OCTAVE** −2 … +2 | **DETUNE** ±25 ct | **DRIVE** |

CHORD: PLAYED (the strings take the sounding notes, lowest first; fewer than four notes → octaves
up), ROOT (0, 12, 19, 24), FIFTH, MAJ, MIN, SUS4, MAJ7, MIN7 (on the lowest sounding note).

Page 1 is TE's own CWO knobs: saved, LFO-modulated and shown like any FX knob. The synth FX screen
shows RESO's page (voicing, the four string notes with their level, both pages' values); a knob
push flips to page 2. As master FX (mixer, FX page) RESO shows the same page and page 2 works there too; as drum FX page 2
keeps the stored values (no drum page yet).

## How

- **Host: TE's CWO effect** (fx id 7). Its 8 fx_params all have range 0..32767 (Ranges table
  `0x01de5c50`, read by running TE's Ranges init `0x01a1e914` in the simulator), and no stock FX
  process reads params 5–8. An FX is RESO when fx_params 5–8 are all `0x2000 | v` (8192..16383),
  v = page-2 value (13 bit). Every stored value is inside TE's ranges, so RESO sounds pass the patch
  validator (boot included, see `features/lofi/README.md` "Boot fix" for that trap) and load on stock firmware
  as plain CWO. Plain CWO (no marker) runs TE's process untouched.
- **FX list:** `content_sql` adds `fx_types` row `reso` with default_params
  `[20000, 22000, 0, 16384, 8192, 12288, 8192, 8192]` (marker + defaults). Picking it, TE reads the
  type name (call sites `0x019ff3f4` synth/drum, `0x019da9b0` master); RESO rewrites "reso" →
  "cwo" there, so TE never meets an unknown FX name (fatal `fx_types.cpp:42`). Since v0.2 through
  the shared composer points `fx_name` / `master_fx_name` (SHIMMER renames at the same sites).
- **Process:** CWO's process slots (mono `0x0190f588` → `rs_te_mono`, stereo `0x0190f5a8` →
  `rs_te_stereo`). Mono serves synth (object pos 0, params of the current synth sound) and drum
  (pos 1); stereo runs the strings on the mid signal and mixes the wet into both channels. Strings
  tuned from the synth voices (`0xff80105c` sounding flags, `0xff8012d4` target increments at
  88.2 kHz: period at 44.1 kHz = 2^31 / inc), retuned only when the voices or voicing change.
- **DSP** (`reso_core.c`): per string a 2048-sample int16 line (lowest note ~21.6 Hz), linear
  interpolated read, one-pole damping in the loop, error-feedback rounding (no DC build-up, quiet
  ringing does not die), soft knee 4:1 above the knee (strings self-limit; DRIVE lowers the knee
  and boosts the input, loop gain never above 1), DC blockers on input and wet output.
- **Cost (sim):** 22k cycles per 64-sample block (~15 M cycles/s, ~3–4 % CPU), 29k on a block that
  retunes.

## Tests

1. `features/reso/test_reso_core.c` (19 checks, ASan/UBSan): voicings, PLAYED octaves, OCTAVE,
   folding of extreme notes, DETUNE, MIX 0 bit-exact, ringing at the note's period after the note,
   DECAY short vs long, 4 s full-scale noise with every knob maxed returns to silence, DC in →
   no build-up, GLIDE.
2. `sim/run_rstest.py`: plain CWO untouched; RESO rings at the voices' note as synth FX, drum FX, master
   FX (both channels); MIX 0 dry; "reso" → "cwo"; page-2 push/edit keeps the marker in range and
   saves; page 1 stays TE's; cycle counts.

## Risks (device test)

- Inferred, not seen: FX order in TE's chain (after the voice sum), `0xff8012d4` vs glide.
- Level of the strings vs dry (tuned on the host demo only).
- Power-cycle test with RESO selected is part of the device test.

**2026-10-14: drum FX screen.** The effect's page now also shows and edits on the drum sound's FX
screen (edit mode 1, drum sub-screen `0x01aab9f4` == 2, type `0xff801754[slot]`, on `0x01aaac30[slot]`,
params `0xff8015c4 + slot * 8`, saved into the drum patch `0x01ddd9a8 + slot * 0x38` through
`0x019ff664`); before, the drum FX screen showed the host effect's own screen.
