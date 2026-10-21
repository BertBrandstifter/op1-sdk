# Feature library

Every feature built on the SDK lives in `features/<name>/` with a manifest `feature.json`. Firmware
images are **composed** from features with a recipe, so features can be added to or removed from an
image independently, and the library documents itself (CLI + SQLite catalog).

```sh
python3 tools/features.py list                    # what exists, status, version
python3 tools/features.py show quantize           # summary, controls, hooks, tests, docs
python3 tools/features.py points                  # shared TE hook points and who uses them
python3 tools/features.py query "SELECT ..."      # SQL on features.db
python3 tools/compose.py recipes/op1plus.json ~/firmware/op1_246.op1    # build an image
python3 tools/compose.py --features quantize,key_probe --name test ~/firmware/op1_246.op1
python3 tools/flash.py build/apps/op1plus/build/op1plus.op1 --eject            # te-boot upload mode
```

Publishing a single feature (as a .zip via `features.py pack`, or on GitHub) and installing someone else's: **`docs/feature-package.md`**
(schema 1, SDK API 1; `features.py check | install | new`).

## Manifest (`features/<name>/feature.json`)

Full key list and rules: `docs/feature-package.md` (also `schema`, `sdk`, `prefix`, `claims`,
`license`, `authors`, `evidence`). The core keys:

| key | meaning |
|---|---|
| `name`, `title`, `version`, `status`, `status_note` | identity; status = `experimental` / `sim-verified` / `device-verified` |
| `firmware` | `{"R246": "<sha256 of the stock .op1>"}` — the firmware(s) its addresses are valid for |
| `summary`, `docs`, `controls` | what it does, where it is documented, how the user drives it |
| `sources`, `libs`, `cflags`, `init` | code (relative to the feature dir), SDK libs (`gfx`), extra flags, one-time init |
| `shared` | `[{"point": "on_key", "fn": "q_key", "mode": "filter"}]` — use of a shared hook point; optional `"order": "first"` / `"last"` overrides recipe order for that handler |
| `hooks` | exclusive hooks, same format as `build.py` (`call_sites`, `entry_hooks`, `pointers`) + `what` |
| `tests` | host unit tests and simulator benches that cover it |
| `depends`, `conflicts` | other features it needs / cannot live with |

## Shared hook points

TE places several features want are owned by the composer (`tools/compose.py`, `POINTS`), which
generates one dispatcher per point in `_compose.c`:

| point | where (R246) | modes |
|---|---|---|
| `on_key` | entry `0x019b6c74`, KeyHandler thread | observe, filter (nonzero = consumed) |
| `on_knob` | entry `0x019b6c0c` | observe, filter |
| `midi_in` | entry `0x01a85c20`, every MIDI message | observe, filter |
| `kb_note_on` / `kb_note_off` | call sites `0x019ac290` / `0x019ac3a6`, keyboard notes into VoiceCtl | observe, filter |
| `midi_note_on` / `midi_note_off` | call sites `0x01a85bc6` / `0x01a8599e`, MIDI notes into VoiceCtl | observe, filter |
| `frame` | call site `0x019ab8b2`, after TE rendered a frame | after (draw with `op1/gfx.h`) |
| `engine_name` | call site `0x01940374`, engine list pick reads the synth_types row's type name | after (rename own engine → host engine) |
| `fx_name` / `master_fx_name` | call sites `0x019ff3f4` / `0x019da9b0`, FX list pick reads the row's type name | after (rename own FX → host FX) |
| `fx_patch` | entry `0x01a01954`, synth/drum FX browser asks for the current row (patch object) | observe |
| `fx_current_name` / `master_fx_current_name` | call sites `0x01a01970` / `0x019d97f0`, the stored fx_type before the browser looks up its row | after (marked sound → own row name) |
| `seq_tick` | call site `0x01a431aa`, once per 64-frame audio block | after |
| `boot_done` | call site `0x019f09d6`, once before TE's main loop | after |
| `main_tick` | call site `0x019f0a6a`, every main-loop pass | after |

Observers run first (they see every event); filters run in recipe order, the first that returns
nonzero consumes the event. A handler's `"order": "first"` / `"last"` moves it to the front / end
of its point (e.g. CHORD+ filters keys first but draws its panel last).

## Checks before anything is built

- dependencies resolved (cycles rejected), declared conflicts rejected;
- every exclusive hook address owned by exactly one feature and never a shared point
  (collision = error naming both features);
- all features support the target firmware (SHA-256 of the stock image), source file names unique;
- `build.py` then verifies every hooked instruction / pointer against the stock image.

## Features (see `features.py list`)

Synth engines (each: one engine-list row + one preset in its own folder; the sounds are a built-in
bank on **shift + blue turn**; knob push = pages; last page = the sound's FX knobs):

- `lofi` 0.7 — LOFI on pulse: chiptune waves + 16 bytebeat formulas, bank of 14 (v0.1 device-verified;
  0.7: preset `lofi` back in the preset browser, shift + white push gesture removed)
- `enginehost` 1.0 — library: SDK engines share drwave via a tag in knob slots 5-8 (2 MONO 3, 3 STATION 1,
  4 TRIO, 5 WHEEL, 6 WARP; 1 free) + the shared FX page (device-verified)
- `wheel` 0.7 — WHEEL tonewheel organ (tag 5): 16 registrations, percussion, click, drive, rotary,
  bank of 24 (0.4 device-verified; 0.7 clean output: no clipping, drive level-compensated)
- `warp` 0.1 — WARP (tag 6): 8 waves x 16 phase / output warps, sweep, motion, unison, bank of 16
  (device-verified)
- `trio` 0.1 — TRIO (tag 4): SID-style 3 oscillators (ring, sync, AND waves) into an LP/BP/HP/notch
  SVF with envelope, drive, noise; page 1 LFO-able; bank of 16 (device-verified)
- `mono` 0.3 — MONO 3 (tag 2): classic 3-oscillator mono synth voice, 4 voices: 3 oscillators
  (range, wave, level each; osc 2 / 3 frequency), white / pink noise, 4-pole ladder (self-oscillation, bass loss,
  keyboard control), filter + loudness contours (decay switch), constant-time glide; page 1 LFO-able; 7 pages;
  bank of 16 + P-- (sim-verified)
- `m1` 0.8 — STATION 1 (tag 3): two of 100 sampled multisounds layered (B pitch / detune, A decay, B fade-in) into
  a low-pass with envelope, resonance, key tracking; page 1 LFO-able; bank of 28. MIX is a crossfade, the output has headroom for
  chords and a soft knee. Samples: a sound library (1.4 MB, committed in
  `features/m1/build/`, not MIT: see its NOTICE; built-in waves
  without it); `m1_drumkit.py`
  makes two drum sampler kits, built in as factory kits (folder station 1); station screen with LCD, LEDs and a keyboard lighting the sounding
  keys (sim-verified)
- `engine_plus` 2.0 — 4 extra parameters for fm, digital, string, cluster, phase, dna, voltage, iter
  (generic stages device-verified; engine-specific hidden parameters sim-verified)

Effects (each hosted on a TE FX with an in-range marker; own row in the FX list, own page):

- `reso` 0.3 — RESO on CWO: strings tuned to the played notes (sim-verified)
- `shimmer` 0.2 — SHIMMER on spring: reverb whose tail rises an interval per pass (sim-verified)
- `fxpack` 0.4 — "fx mix" on phone: phaser, flanger, fold, fuzz, DJ filter, transient, texture
  (sim-verified)

Performance:

- `chordplus` 0.5 — CHORD+ chord keyboard: shift + insert (insert while the panel shows: pin it); white keys = scale chords, held black
  keys = modifiers (lower octave: min/maj, 7th, sus4, 9th, inversion; upper: power, sus2, OPEN / WIDE voicing, 2nd / 3rd inversion); panel KEY / SCALE / VOICING / STRUM (device-verified)
- `drumroll` 2.1 — DRUM ROLL, the finger sequencer's step editor (drum playmode, sequencer key with the
  finger sequencer picked, shift + blue press opens and closes (note "SHIFT+BLUE: EDITOR"); handled like the synth engines: turn = value, press =
  next page, 6 pages STEP NOTE TIME LANE SOUND PAT): shows / edits the selected finger pattern (1-14)
  as 8 lanes = 8 drum samples, own length + rate each (polymetric), drawn to scale; per step VEL TONE
  LEN NUDGE PROB RATCH; per sample filter LP / BP / HP + level; swing; one pattern length for all
  lanes (page 6, and it follows TE's length). An edited pattern plays with all
  of that on TE's step events (finger keys start / stop it, TE's compressed view shows a summary, TE's
  step record still works); black keys on the drum finger screen = punch-in effects in three banks (green press on the finger screen: A PO-20 set, B arcade: 8-BIT LASER FALL COIN RND PITCH OCT UP / DOWN ECHO REVERSE SHUFFLE, C time + mix: HALF / DOUBLE TIME TRIPLET LOOP BEAT BUILD SOLOs NO KICK FADE CUT; a window while held); STEP REC (shift + white: the keyboard into the cursor step, the arrows move); undo / redo shift + BACK / FORWARD (also TE's own finger screen edits); patterns kept in TE's op1.db; `features/drumroll/README.md` (v1.0
  / v1.1 device-tested: "nice!", "really love it!"; v1.2 sim-verified)
- `noteroll` 0.9 — NOTE ROLL, the synth finger sequencer's step editor (synth playmode, sequencer
  key with the finger sequencer picked, shift + blue press opens and closes; DRUM ROLL's handling, 5
  pages NOTE SHAPE CHORD PAT CLEAR): the selected finger pattern as a piano roll, up to 6 notes a step
  (chord shapes, inversions), per note VEL + LEN (to 16 steps, TIE), per step NUDGE PROB RATCH,
  length 1-64, swing, transpose; a pitch cursor (shift + blue turn, blue press = note on / off); STEP REC (shift + white); undo / redo shift + BACK / FORWARD (also TE's finger screen edits); plays on TE's step events, TE keeps a summary; black keys = punch-in effects in three banks (green press: A PO-20 set, B arcade: CHIP ARP UP / DN, POWER UP / DOWN, COIN, TRILL, ECHO, REVERSE, OCT ARP, LASER, C time + solos; a window while held); op1.db;
  `features/noteroll/README.md` (v0.1 sim-verified)
- `requantize` 4.3 — SYNTH ROLL, requantize a tape take: shift + blue knob press (not in drum playmode); DRUM ROLL's look,
  NOTE ROLL's navigation (cursor = grid step + pitch row, blue press = a note on / off, green press = next page): pages NOTE
  (step / pitch / velocity / length), TIME (note jump, move, nudge 5 ms, length ms), QUANT (grid 1/2 .. 1/32T, mode CLEAN (default: start + length on the grid) /
  SNAP / QUEUE / STRICT, zoom, all notes AUTO), TAKE (track, commit); `features/requantize/requantize-roll.md`
  (3.1 device-tested: "very nice"; 4.0 sim-verified)
- `quantcore` 1.0 — library: the quantizer core shared by quantize and requantize
- `quantize` 1.10 — live input quantizer (shift + insert); **not in op1plus** since 2026-10-13 (user),
  kept in the library (conflicts with CHORD+'s shift + insert)

Debug / examples (not in op1plus): `fxdiag` 0.1 (FX hosting overlay, `recipes/op1diag.json`),
`key_probe` (input probe, device-verified), `midi_filter` (MIDI CC filter example).

Recipe `recipes/op1plus.json` (boot text `op1sdk +`): chordplus, lofi, wheel, warp, trio, m1, mono, reso,
shimmer, fxpack, drumroll, noteroll, requantize, engine_plus (+ enginehost, quantcore as dependencies). Factory synth
presets: 164 of TE's 176 (one per engine).

Key combinations in use (machine-readable in each manifest's `claims`; compose stops on overlaps):
shift + insert (CHORD+ on / panel / off), shift + blue knob press (requantize; not in drum playmode, not on the sequencer screen), synth engine screen
plain knob clicks (engine_plus pages on stock engines; page flips on LOFI / WHEEL / WARP / TRIO / STATION 1 / MONO 3),
shift + blue turn on an SDK engine's screen (its bank), CHORD+ panel knobs while open, requantize
SYNTH ROLL on the tape screen (all knob turns and presses, with and without shift; green press = next page),
shift + blue press on the synth finger sequencer screen (NOTE ROLL, as DRUM ROLL), shift + blue press on the drum finger sequencer screen (DRUM ROLL; while open all knob turns and presses
there: press = next page as on the engines, shift + orange press = clear the lane, shift + blue / green press = close). Left to TE: shift + metronome, shift + stop and everything else.

## Other firmware versions

Addresses are per firmware. A feature declares the firmware it was verified on; supporting another
image means re-finding its hook addresses there (the `expect` values make `build.py` refuse any
address whose instruction does not match), then adding that firmware's SHA to the manifest.
`features.db` and `build/` are generated (git-ignored); the manifests are the source of truth.
