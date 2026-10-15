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
- `enginehost` 1.0 — library: SDK engines share drwave via a tag in knob slots 5-8 (4 TRIO, 5 WHEEL,
  6 WARP; 1-3 free) + the shared FX page (device-verified)
- `wheel` 0.7 — WHEEL tonewheel organ (tag 5): 16 registrations, percussion, click, drive, rotary,
  bank of 24 (0.4 device-verified; 0.7 clean output: no clipping, drive level-compensated)
- `warp` 0.1 — WARP (tag 6): 8 waves x 16 phase / output warps, sweep, motion, unison, bank of 16
  (device-verified)
- `trio` 0.1 — TRIO (tag 4): SID-style 3 oscillators (ring, sync, AND waves) into an LP/BP/HP/notch
  SVF with envelope, drive, noise; page 1 LFO-able; bank of 16 (device-verified)
- `engine_plus` 2.0 — 4 extra parameters for fm, digital, string, cluster, phase, dna, voltage, iter
  (generic stages device-verified; engine-specific hidden parameters sim-verified)

Effects (each hosted on a TE FX with an in-range marker; own row in the FX list, own page):

- `reso` 0.3 — RESO on CWO: strings tuned to the played notes (sim-verified)
- `shimmer` 0.2 — SHIMMER on spring: reverb whose tail rises an interval per pass (sim-verified)
- `fxpack` 0.4 — "fx mix" on phone: phaser, flanger, fold, fuzz, DJ filter, transient, texture
  (sim-verified)

Performance:

- `chordplus` 0.3 — CHORD+ chord keyboard: shift + insert; white keys = scale chords, held black
  keys = modifiers; panel KEY / SCALE / VOICING / STRUM (device-verified)
- `drumroll` 0.9 — DRUM ROLL step sequencer for the drum kit ("drum roll" in the drum engine list, or
  shift + green press on the drum engine page;
  handled like the synth engines: turn = value, press = next page, 6 pages STEP NOTE TIME LANE SOUND
  CLEAR; patterns kept across restarts in TE's op1.db):
  8 lanes = 8 drum samples, own length + rate each (polymetric), drawn to scale; per step VEL TONE
  LEN NUDGE PROB RATCH; per sample filter LP / BP / HP + level; tape sync; 4 patterns, swing;
  `features/drumroll/README.md` (v0.1 device-tested: "love it")
- `requantize` 3.2 — requantize a tape take: shift + blue knob press; one screen = piano roll + take
  settings header; green press flips the knobs between NOTES (single notes / selection) and TAKE
  (grid 1/2 .. 1/32T incl. 1/4T, mode, track, zoom, commit, all notes AUTO); `features/requantize/requantize-roll.md`
  (3.1 one screen device-tested: "very nice"; 3.2 grid order + 1/4T flashed; 2.5 logs CHORD+'s chord notes)
- `quantcore` 1.0 — library: the quantizer core shared by quantize and requantize
- `quantize` 1.10 — live input quantizer (shift + insert); **not in op1plus** since 2026-10-13 (user),
  kept in the library (conflicts with CHORD+'s shift + insert)

Debug / examples (not in op1plus): `fxdiag` 0.1 (FX hosting overlay, `recipes/op1diag.json`),
`key_probe` (input probe, device-verified), `midi_filter` (MIDI CC filter example).

Recipe `recipes/op1plus.json` (boot text `op1sdk +`): chordplus, lofi, wheel, warp, trio, reso,
shimmer, fxpack, drumroll, requantize, engine_plus (+ enginehost, quantcore as dependencies). Factory synth
presets: 164 of TE's 176 (one per engine).

Key combinations in use (machine-readable in each manifest's `claims`; compose stops on overlaps):
shift + insert (CHORD+ on / panel / off), shift + blue knob press (requantize), synth engine screen
plain knob clicks (engine_plus pages on stock engines; page flips on LOFI / WHEEL / WARP / TRIO),
shift + blue turn on an SDK engine's screen (its bank), CHORD+ panel knobs while open, requantize
screen on the tape screen (all knob turns and presses, with and without shift; green press = NOTES / TAKE),
shift + white press on the tape screen in drum playmode (DRUM ROLL play / stop, unless the requantize
screen is open), shift + green press on the drum screen (DRUM ROLL; while open all knob turns and presses there except
shift + blue press: press = next page as on the engines, shift + white / orange press = play / clear). Left to TE: shift + metronome, shift + stop and everything else.

## Other firmware versions

Addresses are per firmware. A feature declares the firmware it was verified on; supporting another
image means re-finding its hook addresses there (the `expect` values make `build.py` refuse any
address whose instruction does not match), then adding that firmware's SHA to the manifest.
`features.db` and `build/` are generated (git-ignored); the manifests are the source of truth.
