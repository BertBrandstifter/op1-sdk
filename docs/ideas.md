# Feature ideas — backlog from other synth projects

Surveys of 2026-10-12. Sources are cloned next to the SDK (`../JIANT`, `../sloop-fm1`,
`../strudel`). Licenses: JIANT and sloop-fm1 GPL-3.0, strudel AGPL-3.0 → ideas and published
algorithms only, own code. Exceptions, reusable with credit: sloop's `phys_dsp.c` / `phys_symp.c`
(MIT, DaisySP + Rings, fixed point), strudel's ZZFX (MIT), sloop's `fm6_core.c` (Apache-2.0, msfa).

Routes: **a** = engine hosted on a TE voice (like LOFI on pulse), **b** = effect hosted on a TE FX
(RESO on CWO, SHIMMER on spring; free hosts: delay, grid, punch, phone, filter, nitro), **c** = no
audio DSP: key / knob / MIDI / sequencer-tick hooks (like quantize).

Done: LOFI (engine), WHEEL (engine, v0.1 2026-10-12), CHORD+ (v0.1 2026-10-13), WARP (v0.1 2026-10-13), TRIO (v0.1 2026-10-13), RESO, SHIMMER, FXPACK (phaser, flanger, fold, fuzz, DJ filter, transient,
texture; measured 1-3.5 % CPU each), engine_plus stages, quantize + requantize.

## Ratings

**CPU** (estimated share of the OP-1's DSP time; anchors measured on our own code: engine_plus
~4-5 %, RESO ~3-4 %, SHIMMER ~6 %; real code ran 2-5x the source projects' "ops" counts):
`●` < 2 %, `●●` 2-6 %, `●●●` 6-15 %, `●●●●` > 15 % (needs fewer modes/voices or optimisation).
Engines pay per voice (6 voices at 88.2 kHz), so they cost far more per op than effects (one
stream at 44.1 kHz).

**Difficulty**: low = hosting route already proven, mostly DSP; med = new UI or a new TE detail to
find; high = needs fresh reverse engineering (e.g. OP-1 sequencer data) or many modes.

## Engines (route a)

| idea | from | CPU | difficulty | why |
|---|---|---|---|---|
| *(done)* **WHEEL**: 9-drawbar organ, percussion, click, rotary without delay line | JIANT `eng_wheel.c` | `●●` measured 19k cycles/block FULL | low | no organ on OP-1 |
| **NOISE**: coloured/metallic noise into resonant filter; pink (Kellet), brown, dust | JIANT, strudel `dough.mjs` | `●●` ~5 % | low | pads, FX |
| **WARP**: single-cycle wave through ~20 phase warps (mirror, sync, fold, chaos, bit-reverse…) | strudel `worklets.mjs` 1093-1330 | `●●` ~3-5 % | med (UI for 20 modes) | new wavetable colours |
| ZZFX random SFX drum kit | strudel (MIT) | `●●` ~3 % | med | later |
| **TRIO**: SID-style 3 osc, XOR ring, hard sync, AND waves, gritty SVF | sloop `eng_trio.c` | `●●●` ~9-13 % | med | pulse lacks ring/sync |
| **VOICE**: formant vowels, TALK glide (Klatt / Hillenbrand data; strudel's vowel table) | JIANT, sloop | `●●●` ~9-13 % | med-high | no vocal engine |
| **PHYS modal / membrane**: 12 SVF modes struck or bowed (dust); bells, bars, bowls, tabla, timpani | sloop `eng_phys.c`, `phys_dsp.c` (MIT) | `●●●●` ~13-20 % (6 modes: ~7-10 %) | med (code is MIT + fixed point) | OP-1 string is plucked only |
| **SYMP**: plucked/sitar string + 3 sympathetic strings tuned to a chord | sloop `phys_symp.c` (MIT) | `●●●●` ~15-20 %, 4 KB/voice | med | drones, sitar, harp |
| supersaw (polyBLEP, 7 saws) | strudel | `●●●●` ~15 % | med | later |
| *(done)* LOFI | JIANT | `●`-`●●` | — | reference |

## Effects (route b)

| idea | from | CPU | difficulty | why |
|---|---|---|---|---|
| **Phaser / flanger / wavefolder / fuzz** inserts | sloop `fx.c` 411-630 | `●` < 1.5 % | low | none on OP-1 |
| distortion characters as tables (fold, diode, Chebyshev…) → engine_plus DRIVE | strudel `helpers.mjs` 511-590 | `●` | low | |
| **Transient shaper** (attack/sustain) | strudel `worklets.mjs` 1417 | `●` < 1 % | low | drums; host punch or nitro |
| **TEXTURE bed**: generated vinyl/tape/rain/wind/radio, breathes, pumps | sloop `fx.c` 798 | `●` ~1 % | low | lo-fi bed without a track |
| DJ filter (one knob LP ↔ HP) | strudel | `●` | low | |
| **SLICER**: 16-step tempo gate/stutter | JIANT `slicer.c` | `●` ~1 % | low-med | reuses quantize tempo |
| **DUCK** sidechain pump (+ look-ahead limiter) | sloop `fx.c` 56-120 | `●` < 0.5 % | med (must see kick hits, incl. sequencer-played) | none on OP-1 |
| **GRAIN / pitch delay**: repeats pitch ±12 | JIANT, strudel `dough.mjs` | `●●` ~2-3 %, 128 KB | med | |
| ladder filter (TB-style, tanh tables) | strudel | `●●` ~2-4 % | med | later |
| **Punch-in bank**: hold key → loop 1/4..1/32, reverse, tape stop, rewind, octave, freeze, kill EQ, gate | sloop `punch.c` | `●`-`●●` 1-3 %, 64 KB | med-high (27 modes, key mode, tempo sync) | live performance (b master + c keys) |
| CLOUD granular | JIANT | `●●●` 6-12 % | high | later |
| *(done)* RESO / SHIMMER | | `●●` 3-4 % / `●●●` ~6 % | — | measured reference |

## Performance / sequencer (route c, CPU ~0 for all)

| idea | from | difficulty | why hard / easy |
|---|---|---|---|
| **MUTATE/ORBIT**: knobs drift around a snapshot every 1-32 bars | JIANT | low-med | knob write path exists (`hidden.c`), tempo from quantize |
| **CHORD+ keys**: white key = scale chord, held black keys modify (min/maj, 7, sus4, 9, inversion), voice leading | sloop `seq.c` 250-390 | low-med | note hooks exist; needs extra notes injected into VoiceCtl |
| **Chord voicing + arp index patterns** (extends quantize) | strudel `tonal/` | med | builds on CHORD+ |
| swing / reverse / ping-pong / rotate / shuffle on the live note stream | strudel `core/pattern.mjs` | med | delay/reorder notes like quantize's QUEUE |
| **Euclid** (Bjorklund, rotation) | strudel `core/euclid.mjs` | med-high | live generator easy; writing into OP-1's pattern sequencer needs its data RE |
| **Probability / degrade, ratchet (ply), echo** per step | strudel, JIANT | med-high | per-step storage needs sequencer RE |
| onset auto-slicer for sampler | sloop `eng_slice.c` | med-high | sampler data access not mapped |
| **Fills, micro-nudge, p-locks** per step | sloop `seq.c` 663-830 | high | sequencer RE + UI + storage |

## Fixed-point tricks to reuse

- `mulq16` (sloop `dsp.c`): a·k/65536 from 16-bit halves, no 64-bit multiply.
- Simper trapezoidal SVF with tabled g, one divide per block, Q13, states clamped to ±150000.
- polyBLEP: divide only on the samples next to an edge.
- DC blocker with error feedback (sloop `fx.c` 121), same idea as RESO's loop rounding.
