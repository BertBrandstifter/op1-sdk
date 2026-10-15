# STATUS — session handoff (2026-10-11, night)

## Latest (2026-10-14): drum FX screen for the SDK effects + "drum roll" in the drum engine list (op1plus `eca94ada`, flashed)

User: on the drum page our effects did not use their own screen; DRUM ROLL should be in the drum
engine list (drum / dbox) and use the kit presets for its sounds. RE (two agents, 2026-10-14): drum
edit mode 1 (DrumController 0x01aabaa8, ctor 0x01977774), drum sub-screen 0x01aab9f4 (2 = FX), drum FX
type 0xff801754[8], on 0x01aaac30[8], drum patch 0x01ddd9a8 + s*0x38, 0x019ff664 saves drum FX too
(now in te246.h). Drum engine pick 0x0193f86c (msg +4 slot, +8 drum_types id or -kit index);
drum_types has only dbox; kits come from drum_presets folder "drum"; an unknown saved drum type halts
boot. Changes: RESO / SHIMMER / FX MIX page + editing on the drum FX screen (rstest check 12);
DRUM ROLL 0.8: drum_types row 64 "drum roll", entry hook skips TE's pick and turns DRUM ROLL on for
the sound (kits keep it, dbox ends it), DRUM ROLL only on the drum engine page (T1) so T2-T4 stay
TE's / the effects' (drtest check 11, mutation-tested). 32/32 suite.

## (2026-10-14): DRUM ROLL 0.7 play / stop from the tape screen (`5f29dcdd`, not flashed)

User: to record DRUM ROLL, the familiar shift + white press should work on the tape screen. 0.7:
tape screen + drum playmode: shift + white press = DRUM ROLL play / stop (requantize keeps it while
its screen is open: weak reference to `rq_state` >= PANEL). Record with TE's rec + play; DRUM ROLL
follows the tape position. Bench check 7 extended. Includes 0.6 (back-to-back hits / ratchet fix).
32/32 suite.

## (2026-10-14): DRUM ROLL 0.6 back-to-back hits + ratchet fixed (`6defd144`, not flashed)

User: the same drum on consecutive steps lost hits / collided, RATCH had no effect. Cause (RE, drum
note-on 0x01a84ba0, first loop): TE ignores a drum note-on while the same note is still in its
sounding list. DRUM ROLL's note-off came one step later = the very block of the next hit, and the
tick sent new note-ons before due note-offs; ratchet hits all fell inside the first hit's hold.
Fix: a hit first releases our still-held copy of that drum (as a fast finger would; one-shot tails
keep ringing, release only changes the voice state per the key's play mode, 0x01978898), and due
note-offs go out before new hits. `features/drumroll/sim` now emulates TE's held-note rule; new check 10
(16 x 1/16 on one drum + 3-hit ratchet, all heard), mutation-tested. Also in 0.5: blue press =
step on / off, green press = page. 32/32 suite.

## (2026-10-14): DRUM ROLL 0.5 blue press = step on / off, green press = page (`07c48952`, not flashed)

User decided (tried it, "more ergonomic"): blue press sets / clears the cursor step on any page,
green press turns the page. 0.4 (green = step, blue = page) was never flashed.

## (2026-10-14): DRUM ROLL 0.4 green press = step on / off on every page (`08593391`, not flashed)

0.3 device-tested, user: "nice". 0.4: green press toggles the cursor step on any page (plays it
when stopped, as the ON knob).

## (2026-10-14): DRUM ROLL 0.3 controls (op1plus `b30de819`, flashed)

User after 0.2 device test: shift + blue press on the drum screen started a requantize take (user
saw it as "chord+ mode"); pages only on blue press; white press = hear the current step. 0.3: while
DRUM ROLL is open every knob press is drumroll's (shift + blue swallowed), blue = page, white = hear,
green / orange plain presses unused.

## (2026-10-14): DRUM ROLL 0.2 synth-style pages (op1plus `c6c213cf`, flashed)

0.1 flashed + device-tested, user: "love it", but wanted the handling consistent with the SDK synth
engines. 0.2: turn = value, any knob press = next page, shift + blue turn = lanes (the engines' bank
gesture); 5 pages P1 STEP (step ON VEL PROB), P2 NOTE (step TONE LEN RATCH), P3 TIME (step NUDGE
SWING ZOOM), P4 LANE (SAMPLE LENGTH RATE MUTE), P5 SOUND (FILTER CUTOFF RESO LEVEL); shift + green
turn pattern, shift + white press play/stop, shift + orange press clear (again = undo), shift +
green press close. Editing a step while stopped plays it. `features/drumroll/sim` updated, 32/32 suite.

## (2026-10-14): DRUM ROLL step sequencer (op1plus `ba877d1c`, flashed, user: "love it")

User: FL-style drum step sequencer in a TE look, drawn to scale like the piano roll, own beat per
drum sample, per-note tone / length etc., filters per drum sample. `features/drumroll` v0.1, spec
`features/drumroll/README.md`. Shift + green press on the drum screen. 8 lanes (one drum key each, own length
1-64 + rate 1/4..1/32 incl. triplets), steps VEL TONE LEN NUDGE PROB RATCH, per-sample LP/BP/HP +
level (also for keys played by hand), tape sync, 4 patterns (RAM only), swing. New RE (drum note
path, `te246.h`-level facts in drumroll.c): drum note-on 0x01a84ba0 is synchronous, `extra` =
semitones (pitch 2^(extra/12)), asserts unless playmode + target are drum, 128-entry sounding list
(every note-on needs its note-off); drum sampler voice vtable 0x0190fac4 (render slot 0x0190fad8,
note_on 0x0190fae0, 12 voices x 0x14 at 0xffb00000, key params 0x01aaadb4 + slot*0x180 + key*16).
`lib/gfx.c`: clipped column fill + direct glyph stores (~3x faster overlays). 32/32 suite.
Device test: drum screen edit mode (assumed 1), timing vs metronome + tape, tone / gate / filter
sound on a few kits, CPU with many filtered voices, frame rate with the screen open, badge position.

## (2026-10-13): REQUANTIZE 3.2 grids (op1plus `bb84d267`, flashed)

3.1 flashed, user: "very nice". 3.2: grid list longest first 1/2 1/4 1/4T 1/8 1/8T 1/16 1/16T 1/32
1/32T, 1/4T new, default still 1/16. 29/29 suite.

## (2026-10-13): REQUANTIZE 3.1 one screen (op1plus `5ece8082`, flashed)

User: panel + piano roll as one default screen, keep whole-take and per-note quantize. Ending the
take opens the roll directly, grid / mode / track (REPLACE) in its header. Green press flips the
knobs (user picked this layout): NOTES (default, edits as 3.0) <-> TAKE (header lit: blue grid,
green mode, white track, orange zoom; orange press commit, shift + orange press = every note back to
AUTO = whole take quantized). White press = preview in both. New `RE_AUTO_ALL` (41 host checks),
`features/requantize/sim` updated. 29/29 suite. Spec `features/requantize/requantize-roll.md`. Device test: header readable,
TAKE focus visible enough, frame cost (roll now always drawn).

## (2026-10-13): FXPACK 0.4 page layout + one-colour scope (op1plus `14548a68`, not device-tested)

Scope: one colour per frame from the block's peak (blue quiet -> green -> orange full scale),
instead of the four knob colours (user: "too colorful").

User: TYPE first on page 1, MIX to page 2, four per page. Page 1 = TYPE · knobs 1-3, page 2 =
MIX (phone's knob 4) · page-2 values. FXPACK now edits all knobs itself (phone ranges, clamped,
saved through TE; TE's LFO still drives knobs 1-4). `features/fxpack/sim` updated. 29/29 suite.

## (2026-10-13): REQUANTIZE 3.0 piano roll (op1plus `d1b00474`, not device-tested)

Spec `features/requantize/requantize-roll.md`. Green press in the requantize panel opens a piano roll of the take
(tape screen): cursor/select notes, move (5 ms / grid step), length, pitch (semitone / octave),
delete, quantize single notes or a selection, "as played" <-> AUTO. AUTO notes still follow the
panel's grid + mode (FIXED/deleted notes are left out of the pass); unedited takes render exactly
as 2.5 (host test over 57 random takes x 3 modes). New `rq_roll.c` + `test_rq_roll.c` (38 checks),
`features/requantize/sim` roll scenario (mutation-tested). 29/29 suite. Device test: readability of the scale-1
hint line, frame cost of the full-screen overlay, preview + commit of an edited take.

## (2026-10-13): WHEEL 0.7 clean output (user: crackle in chords)

TRIO device-verified ("all really nice"). WHEEL: drive presets hard-clipped and were 2-3x louder;
fixed (level 18000, soft knee, drive compensation, quieter click); see features/wheel/README.md.

## (2026-10-13): TRIO engine built

`features/trio` v0.1 (spec `features/trio/README.md`, engine host tag 4): SID-style 3 oscillators (16 sets incl.
ring / sync / AND waves), intervals, detune, trapezoidal SVF LP/BP/HP/notch with envelope, drive,
noise; page 1 LFO-able (cutoff, PW); 16-sound bank; page 4 FX. 28/28 suite. Cost 12.8k .. 25k
cycles per voice block. Demo + screenshot in build/demo.

## (2026-10-13): WARP engine + engine host (device-verified: "works great", image 335770d7)

`features/enginehost` (drwave shared by SDK engines via tag in slots 5-8; FX page helper) - WHEEL
0.6 moved onto it (tag 5). `features/warp` v0.1 (spec `features/warp/README.md`, tag 6): 8 waves x 16 warps,
sweep / motion / unison / colour, 16-sound bank, page 3 FX. 26/26 suite. Cost <= 18.1k cycles per
voice block. Demo + screenshot in build/demo. Next: TRIO (user picked WARP + TRIO).

## (2026-10-13): one preset per engine

User rule: every new engine shows exactly one preset in TE's preset browser (own folder, preset named
after the engine); the sounds live in its built-in bank (shift + blue turn). LOFI 0.7: preset `lofi`
back (folder lofi), shift + white push gesture removed. WHEEL 0.5: one preset `wheel`. CHORD+ 0.3:
bigger panel; SDK font gained '#' (drawn as a double cross) and '*'. Factory presets 162/176.

## (2026-10-13): live quantizer out, CHORD+ on shift + insert

User: remove the live quantize mode (shift + insert) from the image, keep requantize; CHORD+ moves to
shift + insert. quant_core moved into library feature `features/quantcore` (requantize + quantize
depend on it), so op1plus no longer pulls in `quantize`. Recipe: chordplus, lofi, wheel, reso,
shimmer, fxpack, requantize, engine_plus.

## (2026-10-13): CHORD+ built

`features/chordplus` v0.1 (spec `features/chordplus/README.md`): shift + metronome = chord keyboard (white =
scale chords, held black = modifiers), panel KEY/SCALE/VOICING/STRUM. First in recipe; chord notes
go through the rest of the kb chain (quantizer). Composer: new per-handler `"order"`; requantize
2.5 logs as filter after CHORD+. 24/24 suite. Next after device test: PHYS (6 modes).

## (2026-10-12, late): WHEEL organ engine

Plan agreed with the user: build + device-test one at a time: **WHEEL** (now) → **CHORD+** → **PHYS**
(6 modes). `features/wheel` v0.1 (spec `features/wheel/README.md`): tonewheel organ hosted on **drwave** with
in-range marker `0x5000|v` in slots 5-8 (no range-check hook; presets load on stock as drwave).
16 registrations + BASS/BODY/TOP trims, percussion, click, drive, rotary (horn/drum, spin-up).
Engine-list row `wheel` + presets jazz organ / rock organ (folder wheel). New shared composer point
`engine_name` (call site 0x01940374): LOFI migrated to it. 32 host checks + `features/wheel/sim`;
22/22 suite. Cost FULL 19.1k cycles/voice block (voltage 16.7k). Demo `build/demo/wheel_demo.wav`.
v0.1 flashed as op1plus `beece8f2` and liked by the user ("really nice"). **v0.2**: knob click
cycles 3 pages, page 3 = the sound's FX knobs (reverb/echo), clamped to TE's FX ranges.
Preset limit decision: keep TE's 176 factory cap; extra sounds ship as user presets (TE caps
factory 176 + samplers 42 + user 100 = one 318-entry array at 0x01dddb68; relocating it = later,
with the editor). **Factory preset list now full (176/176, TE asserts `<=176`).**

**v0.4 + LOFI 0.6 (image `4ab1ee32`, flashed, WHEEL device-verified: "really good"):** page 3 shows the FX knobs' real names (TE
guide: spring TONE TURNS DAMPING SEND, delay SIZE SPEED FEEDBACK MIX, ...). Built-in preset banks
(shift + blue turn): WHEEL 24 organ sounds, LOFI its 14 former factory presets (rows removed, factory
list 162/176). FX sweep in the simulator: TE's wet paths stay silent without boot state (dead end).

## TL;DR

- **Install mystery solved** (from te-boot code): updates only install from te-boot's own Firmware
  Upload RAM disk. The last three builds were copied to the normal user disk, which te-boot never
  reads. `tools/flash.py` now refuses the wrong disk. Name rule: any `*.op1`.
- **C-only hooks**: build.py generates trampolines (`"c"`, `"filter"`, `"after"`, `"init"`),
  replays overwritten entry bytes (refuses PC-relative ones), automates pointer redirects.
- **New RE** (in `te246.h` + README Findings): display double-buffer + frame hook point, key/knob
  input path, lifecycle hook sites, heap/string/libc, audio callback + FX/engine vtables.
  **Assert printer is fatal after boot** (earlier docs said otherwise).
- 6/6 simulator benches PASS (`python3 tools/test_all.py`), incl. 2 new ones.
- **Device-verified (2026-10-11):** `features/key_probe` installed via te-boot upload mode
  (`tools/flash.py`, upload disk = 15 MB `OP-1` volume) and runs: boot text `op1sdk K`, overlay top-left
  on every screen, swatches red/green/blue (RGB565 confirmed), key + knob values update live.
  Confirms: install procedure, generated trampolines (entry + "after" call-site), gfx mapping.

## Device state

- OP-1 runs `key_probe` (boot text `op1sdk K`, SHA `5a7aeeaf…`), stable. Stock restore image
  `~/firmware/op1_246.op1` (SHA `c5315218…13a9`); te-boot is never modified.

## How to install (verified from te-boot 2.30 code, not yet re-tested on device)

1. OP-1 off, hold **COM** while switching on, choose firmware upload ("=== Firmware Upload ===").
2. `python3 tools/flash.py features/key_probe/build/key_probe.op1 --eject`
3. Screen: "Found firmware package..." → "Done!" (or "No firmware written. Try again."). Restart.

te-boot (firmware.cpp, loop at te-boot SDRAM `0x6d4`): RAM disk formatted by te-boot (label `OP-1`,
buffer `0x01000000`); every name ending `.OP1` is installed; `FRC` in the name = force flag, `RBT` =
reboot flag. The old crash screen (Queue.h + yaffs2 frames, no SDK frames) fits the main firmware
choking on unknown files on the user disk — don't put `.op1` files there.

## Device test plan (next session)

1. Flash `features/key_probe` (`op1sdk K`, SHA `5a7aeeaf…`). Expect a black box top-left of the UI
   on every screen, with `K -1 UP N 0`, `KNOB -1 …`, `MIDI 00 00 00`, and 3 colour swatches.
   - Swatches red/green/blue → RGB565 confirmed. Red/blue swapped → BGR: fix `GFX_RGB` in gfx.h.
   - Box at a different corner / mirrored → fix the mapping in `lib/gfx.c` (`gfx_pixel`).
   - Box not visible at all → frame hook not reached; check boot text first.
2. Press every key once and write down the ids → complete `TE_KEY_*` in te246.h
   (only T1–T4 = 6–9, shift = 38 and the note keys are known).
3. Turn each knob one detent → confirm knob order (0 = blue?) and diff per detent (±1024?).
4. `swift tools/midi_send.swift OP-1 note 60 100` → MIDI line shows `90 3C 64`.
5. Optional: `features/midi_filter` (`op1sdk X`): CC 12 from the Mac should have no effect.

## What changed this session

- Ownership: the previous session left 81 files root-owned (benches could not write); fixed.
- `tools/build.py`: generated trampolines, `"libs"`, `"pointers"`, `-u` keeps hook targets alive under
  `--gc-sections`, variable-length entry replay. Old projects still build byte-identical
  (hello `784625d4…`, fb_test `7b6a6040…`).
- `tools/flash.py`, `tools/test_all.py`, `lib/gfx.c` + `include/op1/gfx.h` (UI-coordinate drawing,
  3x5 font, decimal/hex), `.gitignore` (keeps toolchain, builds and all TE-derived files out).
- Examples: `midi_filter` (C only), `key_probe` (overlay probe). `fb_test` is superseded: TE clears
  and flips buffers every frame, so direct drawing into `0x02000000` vanishes/flickers.
- Benches: `sim/gentest` (filter path, init-once, replay), `sim/overlaytest` (render-then-overlay
  order proven by mutation test, return value kept, pixel mapping). Sim now maps L1 data A/B.

## Input quantizer (in progress)

`features/quantize` (boot text `op1sdk Q`): spec `features/quantize/README.md`. v1 (`fb138902…`) device-verified
2026-10-11. v1.1 adds 1/2. Shift+insert cycles off → 1/16 → 1/8 → 1/4 → 1/2 → 1/32 → 1/8T → 1/16T → 1/32T (v1.9) → off; synth mode; keyboard + MIDI notes held
to the next grid point (tape grid while the tape runs, metronome phase otherwise). Host tests +
`features/quantize/sim` pass.

## Feature packages (2026-10-12)

Standard for publishing single features (GitHub repo per feature): `docs/feature-package.md`
(schema 1, SDK API 1). `features.py check|install|new`, `tools/fpkg.py`, lock file
`features.lock.json`; compose checks SDK level, versioned depends and input `claims` (overlap =
error). All in-repo manifests migrated (schema/sdk/prefix/claims); globals renamed to their prefix
(q_grid, rq_rec_start, filter_*; benches updated); engine_plus knob clicks now ignore shift+push
(matches its claim). Open: `license` + `authors` not set in the in-repo manifests (user's choice),
benches/docs still live outside the feature dirs (check warns), no README per feature yet.

## Feature library (new)

Features live in `features/<name>/` with a `feature.json` manifest; images are composed from
recipes (`tools/compose.py recipes/op1plus.json <stock>` -> `build/apps/op1plus/build/op1plus.op1`).
Catalog: `tools/features.py list|show|points|query` (SQLite `features.db`). Spec: `docs/features.md`.
All benches now build through the composer.

## Device state (2026-10-11, latest)

OP-1 runs **op1plus `802ea760`** (lofi 0.4 + reso 0.1 + quantize 1.9 + requantize 2.4 + engine_plus 2.0, flashed 2026-10-12; LOFI device-verified; awaiting feedback on STRICT, 1/32T, commit pre-roll), boot text
`op1sdk +`. Device-verified so far: quantize v1.6, engine layer generic stages, key_probe.
Waiting for device feedback: chord window (1.7), drums (1.8), requantize, fm/digital/voltage/iter
hidden parameters.

## LOFI synth engine (v0.1 device-verified 2026-10-12; v0.2 boot fix)
v0.1 left as current sound -> next boot fatal `Invalid synth patch pulse:… error 15` (TE's patch
validator range-checks knobs; pulse slots 5-8 reject negatives). v0.2 (slots 5-8 only) still halted:
pulse's slots 1-4 have narrow ranges too (slot 1 7808..23168). v0.3 accepts LOFI values in all
pulse slots (range check 0x01a1deb0 replaced). A LOFI sound must not reach stock firmware.
v0.4: LOFI in TE's engine list (SDK: `content_sql` patches content databases at build time;
`synth_types` row + type name rewrite lofi -> pulse at call site 0x01940374). Built `ff56ebf2`.

`features/lofi` v0.1 (spec `features/lofi/README.md`), in op1plus before the other features: chiptune waves +
16 pitch-tracking bytebeat formulas hosted on TE's pulse voice (marker: knob slots 5-8 negative).
shift + white knob push on a pulse sound's engine screen toggles it. RE findings: voices render
128 samples at 88.2 kHz, pitch = per-sample increments at 0xff801950, ADSR applied by TE after
render. Flashed as `8ddd45ad`. Next: device test, then engine-list entry (patch
`content/op1_factory.db`), then shimmer/reso effects.

## Latest build (2026-10-12): op1plus `f896c705`, flashed, awaiting device test

- Master FX pages: RESO / SHIMMER / FX MIX draw their own page on the mixer's FX page, knob push
  = page 2 (FX MIX: TYPE too), saved via TE_MASTER_CHANGED.
- LOFI 0.5: 14 presets in folder "lofi" → LOFI column in the preset browser (it lists folders,
  not synth_types). Device test must include a power-cycle (presets are validated at boot).

## Master FX bug (2026-10-12, investigating)

Diagnostics removed again: device runs **op1plus `080f64b8`** (no fxdiag; fxpack 0.2, current-row fix). fxdiag + recipes/op1diag.json kept for later debugging.

User: reso/shimmer appear in the master (mixer) FX list but don't show/work. Verified from code:
master pick (0x19da958) writes the row's default_params (with our marker) to 0xff8016c4; the
stereo FX objects' vtable +0x14 slots are exactly the ones we patch (sim: phone 0x01911328,
spring 0x019113f0, cwo 0x0190f5a8). The caller of the master FX process is in encrypted L1, so
unknown. Flashed **op1diag `f16cf574`** (op1plus + `features/fxdiag`, boot text `op1sdk D`):
bottom overlay with edit mode, stored master params, master FX object/vtable/slot, and
stereo/marked/mono call counters of RESO, SHIMMER, FXPACK. Remove fxdiag when solved.
**Found (device, 2026-10-12):** browsing the master FX list previews a row (works, our defaults),
but confirming makes TE look up the stored type ("cwo"/"spring") → cursor + defaults of the stock
row → marker gone. Fix: shared points `fx_patch` (entry 0x01a01954), `fx_current_name` (call site
0x01a01970) and `master_fx_current_name` (0x019d97f0): a marked sound answers with its own row
name, so the browsers highlight and confirm our row (synth, drum, master). Flashed op1diag
`b2871f4d` (2026-10-12; incl. fxpack 0.2 "fx mix"). Master FX page done (static RE, mixer agent): edit mode 3 + TE_MIXER_PAGE 0x01aad5f0 == 2 +
master type 0xff801090 + on 0x01aaac40, no trick edit; page-2 edits write 0xff8016c4 and call
TE_MASTER_CHANGED(0x01aad610). RESO/SHIMMER/FXPACK draw their page there too (constants in
te246.h). No master
overlay exists yet (v0.1 overlays are synth FX screen only).

## FXPACK (v0.2: one list row "fx mix"; 2026-10-12)

`features/fxpack` v0.1 (spec `features/fxpack/README.md`): seven effects behind one FX-list row "fx mix" (page 2 blue = TYPE, loads per-effect defaults), hosted on
**phone** (marker `0x6000|v` in fx_params 5-8; param 5 = TYPE): phaser, flanger, fold, fuzz,
djfilter, transient, texture. Page 2 blue = TYPE. 36 host checks + `features/fxpack/sim`; 20/20 suite;
~1-3.5 % CPU per effect. Demo `build/demo/fxpack_demo.wav`. Image `0e75678e` = op1plus with
lofi, reso, shimmer, fxpack, quantize, requantize, engine_plus. Next from the plan
(docs/ideas.md): WHEEL and NOISE engines, then MUTATE and CHORD+.

## SHIMMER effect (flashed `ea09d478` 2026-10-12, awaiting device test)

`features/shimmer` v0.1 (spec `features/shimmer/README.md`): pick "shimmer" in TE's FX list; reverb (4-line
FDN) with a pitch shifter inside the loop, tail rises +12/+7/+19/+24/-12 per pass. Page 1 = TE's
spring knobs (DECAY TONE SHIMMER MIX), page 2 by knob push (INTERVAL SIZE MOD LOWCUT). Hosted on
**spring** (fx id 4; sim sweep: spring ignores p5-8) with marker `0x4000|v` in fx_params 5-8, so
presets pass the boot check and load on stock as spring. Synth vs drum params chosen by
TE_PLAYMODE (inferred). New shared composer points `fx_name` / `master_fx_name` (FX-name rename);
RESO 0.2 migrated to them. 23 host checks + `features/shimmer/sim` pass; 18/18 suite. ~6 % CPU (37k
cycles/block). Demo: `build/demo/shimmer_demo.wav`. FX list 9 rows: safe per code check (dynamic
vector, scrolling browser, see features/shimmer/README.md). Open: device test incl. power-cycle with SHIMMER as synth and master FX.

## Idea backlog

`docs/ideas.md`: ranked engines / effects / performance features from JIANT, sloop-fm1 and strudel
(cloned next to the SDK), with licenses (GPL/AGPL = ideas only; sloop PHYS DSP + ZZFX are MIT).

## RESO effect (flashed `802ea760`, awaiting device test)

`features/reso` v0.1 (spec `features/reso/README.md`): pick "reso" in TE's FX list; four strings tuned to the
synth's sounding notes. Hosted on CWO (all fx_params 0..32767) with an in-range marker in
fx_params 5-8 (0x2000|v), so presets pass TE's boot check and load on stock as CWO. FX list row via
content_sql + rename "reso" -> "cwo" at the two pick call sites.

## op1plus = quantizer + requantize + engine layer

`build/apps/op1plus` (boot text `op1sdk +`): both features in one firmware (`plus.c` chains the
shared on_key / on_knob / frame hooks). Engine layer spec: `features/engine_plus/README.md` — click a knob
on the synth screen to flip it to DRIVE / FILTER / RESO / CRUSH (preset slots 5–8) for fm,
digital, string, cluster, phase, dna, voltage, iter. 10/10 tests pass. Quantizer v1.6 (QUEUE default,
phrase start, held lengths) **device-verified**; engine layer works on device ("work great"),
next: engine-specific hidden parameters instead of the generic stages (RE running per engine).

Simulator note: GDB's bfin sim executes some parallel instructions wrongly (a store in
`R0 = R7 + R1 || [P5++] = R0` took the new R0); when running TE code in the sim, check results.

## Open / next (priority order)

1. ~~Device test~~ done (overlay, colours, hooks). ~~Key-id table~~ done: `TE_KEY_*` in te246.h,
   all device-verified (sheet in `op-1 key mapping/`); metronome = 1 (code-verified; this unit's contact is worn, needs a hard press). Knobs 0-3 =
   blue/green/white/orange (push = keys 59-62). Still open: knob step size per detent, MIDI line check.
2. **Editor foundation**: the hook model is now data-driven (`project.json` + C), so a host-side
   editor can generate projects. Decide: what the editor edits first (overlay UI apps? FX
   parameters? key remaps / MIDI processing?) — this picks the next RE target.
3. **Audio**: wrap the block callback `0xff80036c` (unverified on device; the IVG11 handler is in L1,
   so a crash here kills audio) or replace FX process via vtable pointers like op1r. Needs a bench
   with a fake FX object first. Open: why voices render 128 samples and FX 64.
4. Input: consume keys (filter on `on_key`) for SDK apps with their own screen; needs a mode
   concept (e.g. shift+key combo toggles the app).
5. Naming passes (vtables, RTTI), xref for data pointers; regenerate-`analysis/` script (it is now
   git-ignored as TE-derived).
6. `git init` + first commit (not done yet; `.gitignore` is ready).

## Quick commands

```sh
cd op1-sdk
python3 tools/test_all.py                                              # all benches
python3 tools/build.py features/key_probe ~/firmware/op1_246.op1   # build an update
python3 tools/flash.py features/key_probe/build/key_probe.op1 --eject    # te-boot upload mode only
swift tools/midi_send.swift OP-1 note 60 100 2000                        # MIDI to the device
```
