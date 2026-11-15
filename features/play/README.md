# PLAY

A second tape screen with one list of up to **8 tracks** in any order. Each track is one of:

- **AUDIO**: a WAV / AIF file from the set folder, looped or played once;
- **SYNTH**: NOTE ROLL patterns, played on its own instance of an SDK engine (ACID 3, MONO 3,
  STATION 1, PARTY, PALETTE, MALLET, WHEEL, WARP, TRIO) with one of the 8 synth sounds;
- **DRUM**: DRUM ROLL patterns, played from one of the 8 drum kits.

Everything runs in sync from bar 1, with level, mute, solo, master and a bar counter. TE's own
4-track tape stays as it is and is one key press away. Spec: `new_pattern_tape.md` in the SDK
root (song mode as a tracker: Song → Chain → Pattern, section 2.3; its views come next).

## The screen

```
PLAY            BPM 120     CPU 31   BAR 02.3      PLAY
1 AUD SONG.WAV   LOOP          [=====     ] -9 DB  M S
2 SYN PAT 2 SND 1 V2 ECO   12 .[===       ] -9 DB  M S
3 DRM PAT 1 KIT CUR         4 .[========  ] -9 DB  M S
+ ADD SYNTH
MASTER 0 DB        PRESS: VOICES  SHIFT: ECO  SHIFT+BLUE: EDIT
```

| Control | On a track | On the `+ ADD` row |
|---|---|---|
| blue turn | move the cursor | |
| blue press | mute | |
| shift + blue press | SYNTH / DRUM: its pattern in NOTE ROLL / DRUM ROLL, shown over PLAY (its playhead follows PLAY; shift + blue press closes it) | |
| green turn / press | level / solo | |
| white turn | AUDIO: the file; SYNTH / DRUM: the pattern (--, 1..14) | the type: AUDIO / SYNTH / DRUM |
| white press | AUDIO: LOOP / ONCE; SYNTH: voices 2 -> 4 -> 1 | add the track |
| shift + white press | SYNTH: ECO on / off | |
| orange turn | SYNTH / DRUM: the sound / kit (CUR = TE's current, 1..8) | |
| shift + orange turn | master | master |
| orange press | re-read the set folder | |
| cut, cut again | remove the track | |
| insert, insert again | a new track before this one (white turn while armed: its type) | a new track at the end |
| T1..T4, shift + T1..T4 | cursor to track 1..4 / mute it | |
| play / stop / back / forward | play-pause / pause, again bar 1 / one bar | |
| tape (on the tape screen) | switch between PLAY and TE's tape | |

**CPU.** While playing, the header shows PLAY's share of the audio interrupt's time (percent,
red above 70) and every SYNTH / DRUM row its own share (red above 30); TE's own instruments and
the tape come on top (cpuinfo's `load te`). Ways to keep it down, per SYNTH track:
- **V1 / V2 / V4:** voices at most (default 2; a new note takes the oldest voice). Each voice is a
  full engine render (ACID 3 is mono: it renders once per block whatever the setting).
- **ECO:** the engine renders at 44.1 kHz instead of 88.2 kHz (every other block, 128 samples at
  twice the pitch step = two blocks) and skips the decimator: half the engine's cost. ECO tracks
  take turns (instances 1 and 3 on one block, 2 on the next), so three ECO tracks cost at most two
  renders in any block. ACID 3 knows ECO and keeps its cutoffs, envelopes and slide (its own
  44.1 kHz tables). Other engines run as if at 88.2 kHz: their own time constants (filter
  envelopes, LFOs, glides) run at half speed and aliasing rises on bright sounds; TE's envelope
  keeps its timing.
- Muted tracks (mute, level -60, another track soloed) render nothing.
Always on: a released voice stops once its envelope is below -60 dB, the SYNTH tracks share one
decimator, and a new note renders before the voices already sounding (a mono engine such as
ACID 3 then renders once in that block instead of twice: the CPU spikes that came with legato
notes).

Limits: 3 SYNTH tracks (engine instances), 2 DRUM tracks (CPU), 8 tracks in all. A synth sound
whose engine is one of TE's own shows `NOT AN SDK ENGINE`: TE's engines exist once, as TE's six
voices, and TE renders only the sound being played; running one a second time would need each
engine's voice objects rebuilt (reverse engineering per engine). The SDK engines keep their state
per voice slot (24: TE's 6 + 3 instances x 6) and per instance (MONO 3's voice order, ACID 3's mono
voice, MALLET's motor, WHEEL's rotor, PALETTE's budget); keyboard displays stay TE's.

Patterns are edited in NOTE ROLL / DRUM ROLL. A pattern you never edited there plays TE's own
finger pattern once TE has loaded those (open TE's finger sequencer once after booting). Until
the song view exists, a track's pattern is song row 00 of that track (its chain = the track
number, one row). The track list, the choices, levels, mutes and solos are kept in `SONG.BIN` in
the set folder (written about 2 s after a change; kept when a new set without its own
`SONG.BIN` is imported).

## Audio files on the OP-1

1. Disk mode (shift + com, then disk). Create a folder **`play`** at the top of the disk (next to
   `album`, `drum`, `synth`, `tape`) and copy WAV / AIF files into it, any names (up to 16).
   Optional `set.txt` with the tempo: `bpm 120` (else TE's tempo).
2. Leave disk mode. On the way out, before TE clears the disk, PLAY copies them into its own
   folder; the `play` folder is then gone from the disk, the way TE takes `drum` and `synth` in.
3. Add an AUDIO track and pick the file with the white knob.

Files: PCM 16-bit, 44.1 kHz, mono or stereo. A file that cannot play shows the reason on its own
track (`NOT 44.1 KHZ`, `NOT 16 BIT`, `SONG.WAV: GONE`); the rest plays on.

## How it works

- **Streaming:** every track is read from its file while it plays. Each track has a ring buffer
  (16384 frames, 372 ms) in TE's sample memory. The main loop reads ahead in 16 KB chunks, at
  most 48 KB per pass. Device numbers (probe): ~3.9 MB/s at 64 KB reads, 8 stereo tracks need
  1.4 MB/s; worst single read 29 ms.
- **Sync:** one play position counts frames from bar 1. An AUDIO track maps it to its file
  (modulo its length when looped); a pattern track walks its song column (py_song.c) and asks
  NOTE ROLL's / DRUM ROLL's own scheduler for the hits of each block. Everything is sample-locked
  by construction. An audio ring that runs late is silent for that block (`!` on its row) and
  comes back in sync at the right place.
- **Synth voices:** the track's engine instance renders 128 samples at 88.2 kHz from the slot's
  stored knobs; TE's own envelope code (Adsr.cpp, on PLAY's objects) shapes it with the slot's
  envelope; the SYNTH tracks are summed with their gains at 88.2 kHz and PLAY's copy of TE's
  63-tap halfband takes the sum to 44.1 kHz once (ECO tracks join after it). TE's synth FX / LFO are not
  applied (TE has one instance of each). Notes start on the block after their step (1.45 ms).
- **Drum voices:** TE's kits stay in sample memory; PLAY reads them in place (pitch, start, end,
  playmode, reverse, volume of each key), with DRUM ROLL's VEL x lane level, TONE and LEN gate.
- **Mix:** an "after" hook on TE's metronome mix adds PLAY's sum into TE's master bus, where the
  tape and the instruments are summed too, so TE's master EQ, master FX, the mixer's drive and
  the volume act on PLAY like on the tape. PLAY scales its sum by 0.38 there (TE's drive takes
  the bus up ~x2.64), then per track gain, mute / solo, master, a soft knee, saturation.
- **Import:** a hook before TE's disk cleanup copies `play/` from the disk image
  (`/yaffs2/user/tape_c.raw`, read with the SDK's `fat` library) into the folder PLAY is not
  playing from; the main loop then switches over.

Writes: only `/yaffs2/user/play_a` and `/yaffs2/user/play_b` (the copied set, which take turns;
`ACTIVE` marks the current one) and `SONG.BIN` in the current one. PLAY never writes TE's tape,
its databases or the disk image.

## Tests

- `test_py_core.c` (host): `set.txt`, audio tracks (assign, gone / bad files), the position
  mapping, and a full stream + mix simulation against an independent per-sample reference (once
  and looped files, mono and big-endian, gains, underrun and resync, read errors), pattern inputs.
- `test_py_song.c` (host): typed tracks, playheads through song rows and chains, an empty chain
  row ends the chain, loop back to the start row, audio cells, polymeter, seek, new / clone,
  save / load.
- `test_py_drum.c` (host): pitch maths, TE's position encoding, one-shot vs gate, reverse, loop
  while held, an octave up, the start delay, the level law, voice stealing, clamping.
- `test_py_synth.c` (host): pitch increments, TE's halfband (DC, 1 kHz passes, 30 kHz gone), the
  voices against stand-ins for an engine and TE's envelope (flags, instance, release after the
  hold, -60 dB release end, voice cap and stealing, new notes first, gain and peak, ECO (parity,
  HALF flag), no engine; the 32-bit decimator bit for bit against the 64-bit sum).
- `sim/run_playload.py` (simulator, not in test_all: about 10 minutes): three SYNTH tracks on
  ACID 3, a legato note every 1/16, ECO off and on: cycles per block (peak, average), renders per
  track. 2026-11-15: ECO off peak 201k (314k before new-notes-first), ECO on peak 129k (206k
  before the parity split), averages 181k / 92k.
- `sim/run_playtest.py` (simulator, composed with WARP): import, folder switch, tracks added on
  the ADD row, exact mixed samples of once / looped files, a gone file on its own track, keys,
  a DRUM track sample by sample, a SYNTH track on a test engine as instance 1 with TE's envelope
  stand-ins, SONG.BIN saved and read back, removing a track, no yaffs buffer at an odd address,
  the SYNTH cost options (voices, ECO renders every other block, muted renders nothing) and the
  CPU numbers.

License: MIT.
