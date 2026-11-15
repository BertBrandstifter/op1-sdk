# nodsynth

Takes dsynth out of the OP-1's menus. Content only: no TE code is changed.

## What changes

- **Engine list:** dsynth's row is gone (`synth_types`).
- **Preset browser:** the folder `dsynth` with its 16 factory presets is gone.
- **Default sound of synth slot 5** (first boot, factory reset): it was the dsynth preset
  "legacy". It is now the string preset "piano of love", copied from the factory database at build
  time (no TE data in this repository).

## What stays

- **dsynth's code:** sounds already saved with dsynth still load and play. If one of your eight synth
  slots holds a dsynth sound (stock slot 5 does, unless you changed it), pick another sound for it.
  That sound is in your own `user/op1.db`, which the update does not touch.
- **Engine list highlight:** with a dsynth sound selected, opening the engine list finds no row for
  dsynth. TE then takes its other lookup path (no assert in that code).

## Why it is safe

- TE reads its engine list and preset tables whole (`SELECT id, type FROM %s`,
  `SELECT id,patch,folder FROM %s`) and picks rows by their id. A missing id is a gap, not an error,
  so the remaining rows keep their ids.
- The slot update copies the patch from the factory database. If that preset is missing, the patch
  would be NULL; the column is `NOT NULL`, so the build fails instead of writing an empty slot.

## Removing the code too (later)

That would free the space of dsynth's DSP and screen code, but nothing in CPU (an engine costs
only while it plays). It needs a proof like `nophysics/prove.py`, and a decision on what a saved
dsynth sound turns into.

## Test

`run_ndtest.py`:

1. **Synthetic databases:** the rows go, the other ids stay, the slot gets the patch byte for byte,
   and a missing preset fails the build.
2. **Stock databases** (with the firmware): no dsynth left in any table, 16 presets fewer, nothing
   else changed.

## Device test

1. Flash.
2. Open the engine list: no dsynth.
3. Open the preset browser: no dsynth folder.
4. If slot 5 still held the old dsynth sound, select it once: it plays, and the OP-1 keeps running.
