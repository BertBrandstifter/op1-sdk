# OP-1 key ids (R246, `on_key` 0x019b6c74)

Recorded on a real OP-1 with `examples/key_probe` (2026-10-11). The sheet (`op-1 keys.csv`)
has back/forward/shift off by one; the re-checked values are **back 36, forward 37, shift 38**
(also what the firmware's key-up switch at `0x018ddeac` says). Metronome = 1 (firmware key-down table: id 1 opens the tempo screen); this unit's metronome
contact is worn and only registers when pressed hard.
The volume knob sends no key event. Authoritative list: `TE_KEY_*` in `include/op1/te246.h`.
