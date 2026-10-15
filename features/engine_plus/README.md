# Engine layer ("engine plus") — spec

Status: **v1 built, sim-verified, awaiting device test** (2026-10-11). Code: `features/engine_plus/`,
shipped inside `build/apps/op1plus` together with the quantizer.

## Open it on the OP-1

Part of the default `op1plus` image. Nothing to open: on any TE synth engine screen (fm, digital, string, cluster, phase, dna, voltage, iter), **click a knob** to flip it to its extra parameter — click again to flip back.

## What it does

Every OP-1 synth preset stores 8 knob values, but only dsynth and sampler use slots 5–8; the
other engines ignore them. The engine layer gives those engines four extra per-voice stages,
driven by slots 5–8:

| Knob (click to flip) | Slot | Stage | 0 = |
|---|---|---|---|
| blue | 5 | **DRIVE** — cubic soft clip, gain 1–16× | off |
| green | 6 | **FILTER** — 4-pole ladder lowpass, 18 kHz → 70 Hz ("darken") | open |
| white | 7 | **RESO** — ladder feedback 0–3.6 (tamed at very high cutoffs) | none |
| orange | 8 | **CRUSH** — drops 0–13 low bits + holds samples 1–16× | off |

Engines with the layer: fm, digital, string, cluster, phase, dna, voltage, iter. Not: dsynth and
sampler (they use slots 5–8 themselves: the click flips the knob to TE's own extra parameter),
drwave (a preset stores 32767 in slot 5), pulse (vtable not located yet), drum engines, ls.

Every stage is skipped while its value is 0, so presets that never touched slots 5–8 sound
bit-exact as on stock. Values are saved in the preset (`"knobs"` 5–8); on stock firmware the same
presets load fine and the values are ignored.

## Engine-specific hidden parameters (v2, `hidden.c`)

Engines with an entry in `hidden.c` get their own four parameters instead of the generic stages:
they adjust the voice's internals (or the knob copy TE's render gets) right before TE renders,
and restore what must not persist right after. Mapped from the disassembly and checked by running
TE's render in the simulator (0 = bit-exact stock, on = different spectrum).

| engine | slot 5 | slot 6 | slot 7 | slot 8 |
|---|---|---|---|---|
| fm | RATIO4: op4 ratio ×1–4 (table A) | RATIO3 (table B) | RATIO2 (table C) | WOBRT: LFO rates ×1–64 |
| digital | SUBOCT: osc B/C down 0–8 octaves | FREE: unsynced, detuned down to −6 % | XDRIFT: osc C beats 0–10 Hz | XPHASE: repeatable osc C phase per note |
| voltage | SHAPE: osc2 phase vs osc1 (slewed) | SPREAD: osc3 up / osc4 down ~0–20 Hz | DRIFT: release drift while held | FBPOL: feedback +k → −k |
| iter | POLAR: chain feedback / FM sign (note-on) | SUST: slow/freeze level decay | ITER: slow/freeze chain decay | FEED: loop gain ×1–4 |

fm's ratio/LFO tables are shared by all voices: rebuilt from TE's pristine copy (`0x01d05880`)
before each voice block and restored after. Still generic: string, cluster (RE run interrupted),
phase (internals are code constants: plan = own C port of its small render), dna (needs a pass).

## Controls

- Synth engine screen (edit mode 0, subscreen 0): **click a knob** (keys 59–62) to flip it to its
  extra parameter; click again to flip back. Clicks are consumed only there.
- A flipped knob edits slot 5–8 of the current sound: same step size as TE's dsynth
  (~100 clicks full range), saved through TE's own path (`TE_PATCH_SET_KNOBS` → JSON + save queue).
- Overlay along the bottom: label + bar in a light knob colour for each flipped knob.

## How

- Voice render (`vtable + 0x14`, `render(this, int16 *out, int16 *knobs)`, 128 samples) is
  redirected per engine ("pointers"); `ep_render` calls TE's render, then processes the voice's
  buffer in place. Render **writes** its buffer (verified by running dsynth's render in the
  simulator: a pre-filled buffer is overwritten), so in-place processing does not touch other voices.
- DSP in Blackfin fractional arithmetic (single-cycle 16×16 MAC, saturating add/sub via GCC's
  `__builtin_bfin_*`; portable equivalents for the host tests): per 128-sample voice block
  drive ~2.0k, filter ~3.7k, crush ~2.1k cycles (sim). All on: ~9k incl. wrapper → ~19 M
  cycles/s for 6 voices (~4–5 % CPU). First float/int64 version was 53k (too heavy).
- Tests: `test_ep_dsp.c` (bypass bit-exact, drive, filter response, resonance boost + bounds,
  crush, block-size independence), `sim/run_eptest.py` (wrapper, paging, slot write + save, dsynth
  routing, cycle count).

## Open

- Device test: sound, CPU headroom (watch for crackles with 6 voices + all stages + heavy FX).
- Overlay position vs each engine screen's artwork.
- Pulse engine vtable; optional per-engine stage sets.
