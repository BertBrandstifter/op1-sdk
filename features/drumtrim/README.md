# Factory drum kits without silent tails

Content only, no TE code. TE's 14 factory drum kits (neoprene … peek) end most hits in long
stretches of silence: up to 4.2 s of electret's 11.5 s. `drumtrim.py` cuts every hit 64 frames
after its last sample above −60 dB of **its own** peak (those 64 frames fade out, they are below
−60 dB already), drops silent gaps between hits and moves each key's start / end to match.

**11.18 MB → 9.76 MB of flash (−1.43 MB, 13 %), the update ~1 MB smaller.** Every key plays
bit-identical audio up to the cut; start, pitch, playmode, volume, FX and LFO stay as they are.

## Open it on the OP-1

Nothing to open: the factory drum kits (preset browser, folder drum) play as before. Add `drumtrim` to a recipe after running:

```sh
python3 features/drumtrim/drumtrim.py <path>/op1_246.op1
python3 tools/compose.py recipes/op1plus.json <path>/op1_246.op1
```

The trimmed files are made from your stock firmware into `build/` (TE's data: never committed or
packed); without them the feature adds nothing (`content_files`, optional).

## How

- `content/audio/factory_drum/<kit>.raw` replaced, the kit's `drum_presets` row updated
  (`UPDATE … WHERE id = N AND folder = 'drum' AND` the kit's name, so another firmware's rows stay
  untouched).
- Positions are frames × 4058 (TE's unit); a hit moves by whole frames, so sub-frame offsets stay.
- Keys with a non-default playmode or reverse are kept whole (their end can be a loop point or where
  reverse playback starts): one key (neoprene, key 1).
- Picking a kit copies its audio into your drum slot (`user/audio/drum/drum_<n>.raw`); the copies
  already on the OP-1 stay as they are until you pick the kit again.
- Checks (in `drumtrim.py`): each key's audio identical to stock up to the cut, ends only earlier,
  sub-frame offsets kept, every key inside the new file.

Status: sim-verified (content checks; no device run yet). License: MIT (the code; the audio is TE's).
