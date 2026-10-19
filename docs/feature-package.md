# OP-1 feature packages — publishing standard (schema 1, SDK API 1)

A **feature package** is one OP-1 feature (a synth engine, an effect, a sequencer, a tool) published
by itself, as a **.zip** or a **git repository**: source code plus a manifest. It extends one or more exact stock TE firmware builds (by SHA-256).
Whoever installs it builds the firmware image locally with op1-sdk, from **their own** copy of the
stock firmware. No firmware image or TE-derived file is ever published.

```sh
python3 tools/features.py new my_feature --prefix mf_ --author "Your Name"   # scaffold
python3 tools/features.py check my_feature                                   # validate
python3 tools/features.py pack my_feature                                    # -> build/packages/my_feature-1.0.0.zip
python3 tools/features.py install my_feature-1.0.0.zip                       # or a .zip URL, or:
python3 tools/features.py install https://github.com/you/op1-feature-my_feature --ref v1.0.0
python3 tools/compose.py --features quantize,my_feature --name mine ~/firmware/op1_246.op1
python3 tools/flash.py build/apps/mine/build/mine.op1 --eject                 # te-boot upload mode
```

## 1. Repository layout

The repository root (or the zip's top directory) is the feature directory. Installation copies it to
`features/<name>/`, next to the other features.

```
op1-feature-<name>/
  feature.json        manifest (section 2), required
  README.md           what it does for the player, controls, firmware, status, required
  LICENSE             required; SPDX id repeated in feature.json "license"
  <prefix>*.c / .h    sources (subdirectories allowed; paths in the manifest are package-relative)
  test_<unit>.c       host unit test, next to <unit>.c (built with the system cc + <unit>.c)
  sim/run_<bench>.py  simulator bench (optional; gets OP1_SDK = SDK root in the environment;
                      writes build output to $OP1_SDK/build/bench/, never into the package)
  docs/               longer spec (optional)
```

**Never in a package:** `.op1`, `.ldr`, `.bin`, `.elf`, `.o`, `.dis`, `.dfu`, memory dumps, TE
disassembly or strings, `build/` output, or any file over 1 MB. `check` and `install` reject them.
The only firmware-derived facts a package carries are the ones needed to hook into it: addresses
and the expected instruction/pointer words at those addresses (`expect`). The build checks those
against the user's stock image.

## 2. Manifest `feature.json`

| key | req. | meaning |
|---|---|---|
| `schema` | ✓ | `1` (this document) |
| `name` | ✓ | `[a-z][a-z0-9_]{1,31}`, unique within an image; repository name `op1-feature-<name>` |
| `version` | ✓ | `MAJOR.MINOR[.PATCH]`; git tag `v<version>` on the release commit |
| `title`, `summary` | ✓ | short name; one paragraph of what the player gets |
| `license` | ✓ | SPDX id (`MIT`, `GPL-3.0-only`, …) matching `LICENSE` |
| `authors` | ✓ | list of names |
| `repository` |  | URL people install from (needed by dependents) |
| `status` | ✓ | `experimental` → `sim-verified` → `device-verified` |
| `status_note`, `evidence` |  | evidence: `[{"firmware": "R246", "date": "2026-10-12", "what": "device: preview + commit on 4 tracks"}]`; `device-verified` should have at least one |
| `sdk` | ✓ | SDK API level the feature is written for (section 4) |
| `firmware` | ✓ | `{"R246": "<sha256 of the stock .op1>"}`; every firmware the addresses are valid for |
| `prefix` | ✓ | symbol prefix, e.g. `"rq_"` (or a list `["q_", "qz_"]`), section 3 |
| `sources` | ✓ | `.c` / `.S` files, package-relative |
| `libs`, `cflags`, `init` |  | SDK libs (`gfx`), extra compiler flags, one-time init function |
| `shared` |  | shared hook points used: `[{"point": "on_key", "fn": "rq_key", "mode": "filter"}]`; optional `"order": "first"` / `"last"` overrides recipe order for that handler |
| `hooks` |  | exclusive hooks (`call_sites` / `entry_hooks` / `pointers`); each with `expect` + `what` |
| `claims` | ✓ | physical inputs the feature takes over (section 5); `[]` if none |
| `controls` |  | human-readable control list (README / catalog) |
| `content_files` |  | `[{"file": "content/audio/factory_drum/<name>.raw", "from": "build/x.raw", "optional": true, "sql": {"file": "content/op1_factory.db", "from": "build/x.sql"}}]`: files added to the update (paths in the feature; e.g. a factory drum kit's audio), each with an optional SQL file (statements separated by `;` + newline) run like `content_sql`; an existing file of the update is replaced. A feature may be content only (no `sources`). `optional`: a missing file (local data, e.g. made from your own samples) leaves out the file and its SQL together |
| `content_sql` |  | `[{"file": "content/op1_factory.db", "sql": ["INSERT ..."]}]`: SQL run at build time on a content database of the update (e.g. a row in TE's engine list `synth_types`). Write order-independent statements (`SELECT MAX(id) + 1 ...`); every value must pass TE's checks |
| `tests` |  | package-relative host tests and benches |
| `depends` |  | `["quantize"]` or `[{"name": "quantize", "version": ">=1.8 <2", "repository": "https://…", "ref": "v1.8.0"}]` |
| `conflicts` |  | feature names it cannot share an image with |
| `docs` |  | package-relative spec |

## 3. Linking rules (everything links into one image)

- Every **global** symbol a package defines starts with its `prefix`. Everything else is `static`.
  `check` cross-compiles the sources and inspects them with `nm`. A plain `grid` in two features
  would otherwise be a link error.
- Source basenames should start with the prefix too, because the composer refuses two features
  with the same object name.
- A dependency's headers are included relative to the install layout:
  `#include "../quantcore/quant_core.h"`. List those headers in the dependency's README as its API.
  Changing them incompatibly is a MAJOR version bump.
- TE addresses come from the SDK headers (`<op1/te246.h>`) whenever they are there. Addresses that
  only this feature uses go in its own header named after the firmware (`<prefix>te246.h`), so a
  port to another firmware has one file to redo.

## 4. Extending a TE build

- **Firmware pinning:** a feature is valid only for the stock images listed in `firmware`. The
  composer intersects all features' lists and refuses a stock image whose SHA-256 is not in it.
- **Prefer shared hook points** (`features.py points`: `on_key`, `on_knob`, `midi_in`,
  `kb_note_on/off`, `midi_note_on/off`, `frame`, `seq_tick`, `boot_done`, `main_tick`). The SDK owns
  them and chains every feature's handler:
  - observers run first;
  - filters run in recipe order, and the first nonzero return consumes the event;
  - `after` handlers run after TE.
- **Exclusive hooks** (an address only this feature patches) need `expect`. `build.py` refuses an
  address whose stock instruction does not match. Two features on the same address are an error
  that names both.
- **SDK API level** (`sdk`): the contract of the shared points (names, signatures, modes), the
  dispatch semantics, `op1/te246.h` and the SDK libs. This SDK is **API 1**. A package declaring a
  higher level is refused. The level is bumped only for incompatible changes, and the changes are
  listed in section 9.
- **Supporting another firmware:** re-find every address in that image, add a firmware entry, ship
  the per-firmware header. Until the SDK has per-firmware point tables, one SDK checkout targets
  one firmware (R246).

## 5. Input claims

Features grab keys and knobs. Two features reacting to the same input in the same place produce
behaviour that depends on recipe order, so packages declare what they take:

```json
"claims": [
  {"input": "blue_push", "shift": "yes", "screen": "any"},
  {"input": "white_turn", "shift": "no", "screen": "tape", "modal": true}
]
```

- `input`: `<key>_press` for `speech metronome synth drum tape mixer t1..t4 cut insert join sound1..sound8 auto rec play stop back forward mic com keyboard`,
  or `<knob>_turn` / `<knob>_push` for `blue green white orange`.
- `shift`: `no` (default), `yes`, `any` (the handler does not look at shift).
- `screen`: `any` (default), `synth`, `drum`, `tape`, `mixer`, `tempo`, `sequencer`, `album`, or a
  list of them (e.g. every screen but `sequencer`).
- `playmode`: `any` (default), `synth`, `drum` (TE's playmode; the handler must leave the input to
  TE in the other one).
- `modal: true`: only while the feature's own panel/mode is open (the user opened it on purpose).

The composer stops on two non-modal claims that overlap: same input, compatible shift, and
overlapping screen and playmode (`any` overlaps everything). It only notes overlaps involving a modal claim.
The code must match the claims. For example, if `shift` is `no`, the handler must return 0 while
shift is held.

## 6. Behaviour rules

- **Opt-in:** without its trigger, a feature leaves TE's behaviour unchanged, apart from what
  `summary` says (e.g. an overlay).
- Do not write to flash, te-boot, or the user's disk. Do not touch the boot path beyond the SDK's
  hooks.
- Drive TE like a user would (key injection with `TE_KEY_PUSH` + `TE_KEY_WAKE`) instead of writing
  TE state directly, where possible.
- Hooks run on TE's threads (KeyHandler, GUI, Seq, audio). Keep them short and never block.

## 7. Publishing and installing

A package can be published two ways. Both carry the same files and pass the same checks.

**As a .zip (author):**
1. `features.py check <name>` passes: no errors. Also run `test_all.py`.
2. Set `status` honestly. Add an `evidence` entry for every device test.
3. `features.py pack <name>` runs the full publishing checks and writes
   `build/packages/<name>-<version>.zip` with one top directory `<name>-<version>/`. The zip is
   reproducible (sorted entries, fixed timestamps), so the printed sha256 identifies the release.
   Share the zip and its sha256 (forum, release page, mail).

**As a git repository (author):** steps 1–2, then commit, tag `v<version>`, push, and add the GitHub
topic `op1-feature`. A GitHub release can attach the `pack` zip as well.

**User:**
1. `features.py install <source>`, where source is a `.zip` file, a `.zip` URL (http/https), a git
   URL with `--ref v<version>`, or a local directory. Zips are extracted safely (no absolute paths,
   no `..`, no links, at most 16 MB / 2000 entries; `__MACOSX`, `.DS_Store` skipped); the package
   may sit at the top of the zip or in one top directory (GitHub "Download ZIP" works). Then install
   runs `check` (static checks plus a cross-compile; nothing from the package is executed), copies
   it to `features/<name>/`, and records the source, ref, commit (git) or sha256 (zip) in
   `features.lock.json`. Compare that sha256 with the one the author published.
2. Dependencies that declare a `repository` (git URL or .zip URL) are installed the same way.
3. A recipe may name packages as `{"name": "x", "repository": "…", "ref": "v1.0.0"}`. If the package
   is missing, compose prints the install command.

**Trust:** installing a feature means running its author's code on your OP-1 with full access to
the firmware. A package's benches and host tests are Python and native code that `test_all.py`
runs with your user rights. Read a package before building, flashing or testing it. te-boot
(upload mode) is never touched by the SDK, so a bad image can always be replaced.

## 8. Contributing a feature to op1-sdk itself

Features can also live in the SDK repository, under `features/<name>/`, so every SDK user gets
them. That route is a pull request; see [`CONTRIBUTING.md`](../CONTRIBUTING.md). A contributed
feature follows this same standard (it must `pack` cleanly), so it can be published as a package too.

## 9. Versions of this standard

- schema 1 / SDK API 1 (2026-10-12): first version. Shared points as listed in section 4; claims;
  prefix rule; lock file.
- 2026-10-15 (no schema change): `.zip` packages (`features.py pack`, install from .zip / .zip URL,
  sha256 in the lock file); contributing to the SDK repository (section 8).
- 2026-10-17 (no schema change): optional claim field `playmode` (`any` / `synth` / `drum`); `screen`
  may be a list.
