# MIDI CC filter (example)

Drops incoming MIDI CC 12 before TE sees it and counts what passes. Example of a filtering MIDI feature with a one-time init.

## Open it on the OP-1

Nothing to open: not part of `op1plus` — build and flash `features/midi_filter` (boot text `op1sdk X`). It runs invisibly: MIDI CC 12 is dropped before TE sees it, everything else passes.

## Controls

- none

Status: sim-verified. License: MIT.
