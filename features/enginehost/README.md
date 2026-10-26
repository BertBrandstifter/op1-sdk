# Engine host — spec

Status: **v1.2** (2026-10-22: tag 7 = MALLET), sim-verified; v1.1 = FX MIX names by TYPE slot, all fifteen FX MIX effects; v1.0 2026-10-13. Code: `features/enginehost/` (library feature; WHEEL and
WARP depend on it).

All of TE's synth engines are taken (engine_plus: fm digital string cluster phase dna voltage iter;
LOFI: pulse), so new SDK engines share **drwave**. The host owns drwave's render (`0x0190fb40`)
and note_on (`0x0190fb48`) vtable slots and hands each voice to the engine whose **tag** the
sound carries: knob slots 5-8 all `(tag << 12) | v` with the same tag 1..7 (v = 12 bits of that
engine's page 2). Stock drwave slot-5 values 0x7d00 / 0x7fff carry tag 7, but stock presets have
slots 6-8 = 0 (all 16 factory drwave presets checked), so they never match: tag 7 is MALLET's. Page 1 stays within drwave's slot ranges (engines use
0..16383). Everything is in range: no range-check hook, presets load on stock firmware as drwave.

Tags: **1 PARTY (PSS-11)**, **2 MONO 3**, **3 STATION 1**, **4 TRIO**, **5 WHEEL**, **6 WARP**, **7 MALLET**, free: none.

- `eh_register(tag, render)` from the engine's `init`; render(vi, out, knobs, inc, flags) with
  `EH_FRESH` (first block of a new note) and `EH_BLOCK` (first voice of this audio block: shared
  per-block state such as WHEEL's rotor).
- `eh_fx_knob` / `eh_fx_draw` (`eh_ui.c`): the "page 3" every engine shows: the sound's FX knobs
  with their names (TE's OP-1 guide names; SDK effects by their own), kept inside TE's range for
  the FX type, saved through TE's FX save.
- Pointer redirects run without the SDK's `init` wrapper: until a hook has run once (frame / key,
  long before the first note) a tagged voice plays as drwave. Benches call `compose_init` first.
- Rule (user, 2026-10-13): every engine = one engine-list row + one preset in its own folder; the
  sounds live in a built-in bank (shift + blue turn).

## Open it on the OP-1

Library — nothing to open on the device. It hosts WHEEL, WARP, TRIO, STATION 1 and MONO 3 (they appear in the engine list through it) and is pulled into the `op1plus` image automatically.
