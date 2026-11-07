# Engine host — spec

Status: **v1.4** (2026-11-07: extended tags 8..13 for ACID 3 and PALETTE, sim-verified); v1.3 (2026-10-26: the dot-matrix displays of the engine screens, `eh_dots.c`; 2026-10-27: antialiased shapes, `eh_aa.c`), sim-verified; v1.2 = tag 7 MALLET; v1.1 = FX MIX names by TYPE slot, all fifteen FX MIX effects; v1.0 2026-10-13. Code: `features/enginehost/` (library feature; WHEEL and
WARP depend on it).

All of TE's synth engines are taken (engine_plus: fm digital string cluster phase dna voltage iter;
LOFI: pulse), so new SDK engines share **drwave**. The host owns drwave's render (`0x0190fb40`)
and note_on (`0x0190fb48`) vtable slots and hands each voice to the engine whose **tag** the
sound carries: knob slots 5-8 all `(tag << 12) | v` with the same tag 1..7 (v = 12 bits of that
engine's page 2). Stock drwave slot-5 values 0x7d00 / 0x7fff carry tag 7, but stock presets have
slots 6-8 = 0 (all 16 factory drwave presets checked), so they never match: tag 7 is MALLET's. Page 1 stays within drwave's slot ranges (engines use
0..16383). Everything is in range: no range-check hook, presets load on stock firmware as drwave.

Tags: **1 PARTY (PSS-11)**, **2 MONO 3**, **3 STATION 1**, **4 TRIO**, **5 WHEEL**, **6 WARP**, **7 MALLET**;
extended (v1.4): **8 ACID 3**, **9 PALETTE**, free: 10..13.

**Extended tags** (v1.4, tags 1..7 were full): slots 5-7 carry tag 7 and slot 8 carries tag − 7
(1..6), i.e. `7 7 7 x` with x ≠ 7. MALLET's sounds are `7 7 7 7` (unchanged), stock drwave presets
`7 0 0 0` / `0 0 0 0` (no match). `eh_tag` returns 8..13 for them, so every older engine (which
compares `eh_tag(k) == its tag`) leaves them alone; an image without v1.4 plays them as drwave.
Engines with an extended tag build slots 5-8 with `eh_p2s(tag, slot - 4, v)`.

- `eh_register(tag, render)` from the engine's `init`; render(vi, out, knobs, inc, flags) with
  `EH_FRESH` (first block of a new note) and `EH_BLOCK` (first voice of this audio block: shared
  per-block state such as WHEEL's rotor).
- `eh_fx_knob` / `eh_fx_draw` (`eh_ui.c`): the "page 3" every engine shows: the sound's FX knobs
  with their names (TE's OP-1 guide names; SDK effects by their own), kept inside TE's range for
  the FX type, saved through TE's FX save.
- `eh_lcd_blank` / `eh_lcd_text` / `eh_lcd_w`, `eh_rrect` / `eh_frame` (`eh_dots.c`): the
  engines' dot-matrix displays (STATION 1's LCD 40×2, MONO 3's LED 12×2, PARTY's LCD 16×2, WHEEL's
  lamps 8×2): cells of 5 × 7 dots with the unlit dots drawn, dot pitch / size / cell advance per
  display (a 3-pixel dot = a round lamp), the font in `eh_font57.h` (ASCII 0x20 .. 0x7e, drawn in
  `gen_font57.py`, MIT: run it to regenerate); rounded frames.
- `eh_aa_line` / `eh_aa_ellipse` / `eh_aa_frame` / `eh_aa_box` / `eh_aa_plot` (`eh_aa.c`, after
  `eh_aa_begin(buf)`): antialiased shapes (MALLET's instruments, PARTY's knob, lamps and SELECT
  buttons): each pixel along an edge blended into the frame by its coverage (from its distance to
  the shape, 1/16 px). Lines with round ends; ellipses, arcs and dashes in two scan passes (rows /
  columns, each pixel once; no divide or root per pixel); rounded boxes from corner tables. Lines
  and ellipses are kept (up to 16384 pixels with their coverage, 64 KB) and played back while drawn
  the same again; `eh_aa_forget` drops them, `eh_aa_dy` moves everything up / down.
- Pointer redirects run without the SDK's `init` wrapper: until a hook has run once (frame / key,
  long before the first note) a tagged voice plays as drwave. Benches call `compose_init` first.
- Rule (user, 2026-10-13): every engine = one engine-list row + one preset in its own folder; the
  sounds live in a built-in bank (shift + blue turn).

## Open it on the OP-1

Library — nothing to open on the device. It hosts WHEEL, WARP, TRIO, STATION 1 and MONO 3 (they appear in the engine list through it) and is pulled into the `op1plus` image automatically.
