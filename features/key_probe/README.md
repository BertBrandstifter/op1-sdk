# On-screen input probe (debug)

Black box top-left over every TE frame: last key id + up/down + count, last knob + diff + sum, last MIDI bytes, RGB565 colour swatches. Used to map every key id; observes only, consumes nothing.

## Open it on the OP-1

Nothing to open: not part of `op1plus` — build and flash `features/key_probe` (boot text `op1sdk K`); the probe box then sits top-left over every screen.

## Controls

- none (display only)

Status: device-verified. License: MIT.
