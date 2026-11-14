# Default slot sounds without a second copy in the update

TE's update carries the default sounds of the 8 drum and 8 synth slots in
`content/audio/drum/` and `content/audio/synth/`. Six of them are byte-identical copies of factory
sounds that the update already has:

| Slot file | Same bytes as |
|---|---|
| `drum_0.raw` | `factory_drum/peek.raw` |
| `drum_2.raw` | `factory_drum/awa beef.raw` |
| `drum_4.raw` | `factory_drum/neoprene.raw` |
| `drum_5.raw` | `factory_drum/acetate.raw` |
| `drum_6.raw` | `factory_drum/red maple.raw` |
| `synth_7.raw` | `factory_synth/suitcase.raw` |

The update is one LZMA stream with an 8 MB window, too small to find copies this far apart. So the
copies cost **2.1 MB of the update** (op1plus: 15.1 → 13.0 MB) and 4.6 MB of content. `drum_1`,
`drum_3`, `drum_7` are their own sounds; `synth_0..6` are 1-byte placeholders.

## When TE uses these files

Only when `user/op1.db` is missing: first boot, or after a factory reset. db2.cpp's init
(`0x0196f6a0`) copies `content/op1.db` to `user/` (`0x0196ea42`: copy only if the user file does
not exist). If that copy happened, it creates `user/audio/{synth,preset_synth,drum,preset_drum}` and
copies those directories from `content/` (`0x0196eb7e(dir, root)`). On an OP-1 in use these files
are never read. A slot then gets its audio from the kit you pick (`user/audio/drum/drum_<n>.raw`).

## What slotlink does

- **Build** (`content_links` in `feature.json`, `tools/build.py`): each slot file is left out of
  the update after a byte-for-byte check against the factory file **in stock firmware** (another
  firmware: the build stops). It runs after every other feature's content changes:
  - factory file unchanged → the slot file is left out;
  - factory file changed (e.g. `drumtrim` cut the kit) and the slot's row in `content/op1.db`
    equals the stock factory preset → the row gets the **final** preset (trimmed positions to the
    trimmed audio) and the slot file is left out;
  - otherwise the slot file is kept. With `drumtrim` this is `drum_4` (neoprene): its slot has its
    own patch.
- **OP-1** (`slotlink.c`): call-site hooks after TE's directory copies of `audio/synth`
  (`0x0196f8b6`) and `audio/drum` (`0x0196f8ce`). Each linked slot file that is still missing is
  copied from its factory file (TE's file calls, 2 KB blocks). A file that exists is never touched,
  so this only fills what TE would have copied.

Nothing to operate.

## Not covered

- Files already on the OP-1 stay. te-boot writes the files of an update. Whether it deletes old
  `content/` files that a new update lacks is not known, so the flash is not freed by this alone.
- `slotlink.c`'s table and `content_links` must list the same pairs.

## Test

- `sim/run_sltest.py`: through the generated hooks (TE's copy first, then ours), against an
  in-memory file system: missing slot files made byte for byte (several blocks, exactly one block,
  one byte); a TE-copied file and the user's file untouched; a missing factory file and a failing
  write counted, descriptors closed; a second run changes nothing; other directories do nothing.
- Build: compose `recipes/op1plus.json` and read the `left out` / `kept` lines.
