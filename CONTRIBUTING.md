# Contributing to op1-sdk

There are two ways to share an OP-1 synth engine, effect or other feature with other SDK users:

| route | what you do | who gets it |
|---|---|---|
| **package** | `features.py pack <name>` and share the zip (or a git repo) | anyone who runs `features.py install <zip>` |
| **contribution** | a pull request adding `features/<name>/` to this repository | every SDK user |

Both use the same standard, [`docs/feature-package.md`](docs/feature-package.md). Most features should
start as packages. A feature moves into the SDK once people have used it on real devices.

## Adding a feature by pull request

1. Fork, then scaffold: `python3 tools/features.py new <name> --prefix <p>_ --author "Your Name"`.
2. Keep everything inside `features/<name>/`: sources, `README.md`, `LICENSE`, host tests
   (`test_<unit>.c`), simulator benches (`sim/run_<bench>.py`, build output to `build/bench/`) and
   longer docs. New code is MIT (`LICENSE` at the root), like the rest of the SDK.
3. Check before opening the PR:
   ```sh
   python3 tools/features.py check <name>     # no errors
   python3 tools/features.py pack <name>      # must succeed: the feature is publishable on its own
   python3 tools/test_all.py                  # whole suite still passes
   ```
4. Build it in an image together with the default recipe (`recipes/op1plus.json`). The composer
   must not report claim overlaps or hook-address conflicts with the features already there.
5. Set `status` honestly. `device-verified` needs an `evidence` entry (firmware, date, what you
   tested). Power-cycle the device after saving presets with your feature: an out-of-range stored
   value halts the next boot.

### What the PR must not contain

- Any TE firmware, image or TE-derived file: `.op1`, `.ldr`, `.bin`, `.elf`, `.dfu`, memory dumps,
  disassembly, strings. `check` and `pack` reject them. Addresses and their `expect` words are the
  only firmware facts allowed.
- Build output (`build/`) or files over 1 MB.
- Changes to another feature, unless the PR is about that feature too (say so in the description).

### New synth engines and effects

- New engine: one preset in the browser plus one row in TE's engine list (`content_sql`). Sounds go
  in the shift + blue bank. No gesture controls.
- Every value the feature stores in a preset must pass TE's range checks.
- Effects that also run on the drum page should get their own drum FX screen (see `reso`, `shimmer`).

### Changing the SDK itself

Shared hook points, `include/op1/te246.h` and `lib/` are the **SDK API**. A change that breaks
existing features bumps `SDK_API` in `tools/fpkg.py`, must be listed in section 9 of
`docs/feature-package.md`, and should update every feature in `features/` in the same PR. New
TE addresses need the RE evidence (function, what it does, how it was confirmed) in the PR.

## Review

A reviewer reads all the code before building or flashing it. Installing a feature runs its author's
code on the OP-1 with full firmware access, and its benches run on the reviewer's machine.
