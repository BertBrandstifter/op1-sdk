# PLAY

A second tape screen with one list of up to **8 tracks** in any order. Each track is one of:

- **AUDIO**: a WAV / AIF file from the set folder, looped or played once;
- **SYNTH**: NOTE ROLL patterns, played on an instance of an SDK engine (ACID 3, MONO 3,
  STATION 1, PARTY, PALETTE, MALLET, WHEEL, WARP, TRIO) with one of the 8 synth sounds; up to 8
  SYNTH tracks share 3 instances, so at most 3 sound at once (they take turns in the song);
- **DRUM**: DRUM ROLL patterns, played from one of the 8 drum kits.

Everything runs in sync from bar 1, with level, mute, solo, master and a bar counter. PLAY is the
tape screen of an image that contains it: TE's own 4-track tape screen is not reachable there
(until v0.5 a second press of tape switched to it; TE's tape data is never touched). The tape
key always shows the track list; join opens the song view from any screen (v0.8.2). A tracker song arranges it all: the
**song view** (join, from any screen) has one column per track, the **chain view** strings patterns together,
**live mode** (shift + rec in the song view) cues rows while it plays, and **rec** renders
the song into an AIFF file that plays back as one cheap AUDIO track (below). **speech** shows a help
page for the view you are in. The views use the SDK's 5x7 font (52 characters a line). Spec: `new_pattern_tape.md` in the SDK root (section 2.3).

## The screen

```
PLAY            BPM 120     CPU 31   BAR 02.3      PLAY
1 AUD SONG.WAV   LOOP          [=====     ] -9 DB  M S
2 SYN SND 1 PAT 2 V2 ECO   12 .[===       ] -9 DB  M S
3 DRM KIT 1 PAT 1           4 .[========  ] -9 DB  M S
+ ADD SYNTH
MASTER 0 DB        PRESS: VOICES  SHIFT: ECO  SHIFT+BLUE: EDIT
```

| Control | On a track | On the `+ ADD` row |
|---|---|---|
| blue turn | move the cursor | |
| blue press | mute | |
| shift + blue press | SYNTH / DRUM: its pattern in NOTE ROLL / DRUM ROLL, shown over PLAY (its playhead follows PLAY; shift + blue press closes it) | |
| green turn / press | level / solo | |
| white turn | AUDIO: the file; SYNTH / DRUM: the sound / kit 1..8 (held at the ends) | the type: AUDIO / SYNTH / DRUM |
| white press | AUDIO: LOOP / ONCE (not with song cells: `SONG`); SYNTH: voices 2 -> 4 -> 1 | add the track |
| keys 1..8 | SYNTH / DRUM: sound / kit 1..8, at once | |
| join | the song view | the song view |
| speech | the help page (blue turn: the other pages; any key: back) | |
| shift + white press | SYNTH: ECO on / off | |
| orange turn | SYNTH / DRUM: the pattern 1..14 (held at the ends); a track with a longer song shows `SNG` and is edited in the song view | |
| shift + orange turn | master | master |
| orange press | re-read the set folder | |
| cut, cut again | remove the track | |
| insert, insert again | a new track before this one (white turn while armed: its type) | a new track at the end |
| T1..T4, shift + T1..T4 | cursor to track 1..4 / mute it | |
| play / stop / back / forward | play-pause / pause, again bar 1 / one bar | |

## The song: a tracker

Three levels: the **song** (128 rows x one column per track), **chains** (128, shared by all
tracks: 16 rows of pattern + transpose) and the **patterns** of NOTE ROLL / DRUM ROLL. Numbers are
hex (rows and chains `00..7F`, chain rows `0..F`); patterns stay `1..14`.

Every track walks down its own column: a chain plays its rows in order (each pattern once, at its
own length), then the next song row follows; an empty cell ends the column and it starts again
at the start row. Columns of different lengths drift apart on purpose (polymeter), so the rows
are not a time grid: each column shows its own playhead (the filled cell). A track with nothing
but one pattern on row 00 is the simple case the track list edits directly; a new SYNTH / DRUM
track starts that way (TE's current sound / kit, pattern 1). A column that is empty at its start
row is silent; given a chain there while playing, it joins at the next bar.

```
SONG            BPM 120     CPU 31   BAR 05.1      PLAY
     1SYN     2DRM     3AUD     4SYN
00   00       01       ON       03
01   00       01       ON       --
02   02       04       OF
03   --       04       --
CHAIN 02 ALSO ON TRACK 4        WHITE PRESS: OPEN  SHIFT: CLONE
```

| Control | Song view |
|---|---|
| blue turn / green turn | cursor row (shift: 16 rows) / column |
| white turn | the cell: empty, `OF` (a rest: one silent bar), chain `00..7F` (shift: 16 at a time); AUDIO: empty, `ON`, `OF` |
| white press | empty cell: the last chain used in this column (none yet: a new one); chain cell: open the chain view; AUDIO: `ON` <-> `OF` |
| shift + white press | empty cell: a new unused chain; chain cell: clone it (the cell gets the copy) |
| cut / insert | cut the cell into the clipboard / paste it |
| shift + insert | a new empty song row here (all columns; refused if row 7F is used) |
| shift + cut, twice | delete the song row |
| blue press / green press | mute / solo the column's track |
| T1..T4, shift + T1..T4 | cursor to column 1..4 / mute it |
| play | start at the cursor row, from bar 1 (playing: pause; resume a pause on the list) |
| shift + play | cue the cursor row on every column (see live mode) |
| rec, rec again | render the song from the cursor row to a file (see Render) |
| shift + rec | live mode on / off |
| stop / back / forward | as on the list (stop twice: bar 1, song row 00 again) |
| shift + blue press | the pattern in NOTE ROLL / DRUM ROLL |
| keys 1..8 | sound / kit 1..8 of the cursor column's track, at once |
| shift + orange turn | master |
| tape / orange press | back to the track list (join stays: join always opens the song view) |

**Gaps.** An empty cell is the column's end, not a pause. For a pause put `OF` cells in the
column: each is one bar of silence, in SYNTH / DRUM columns (white turn, one step below `00`)
as in AUDIO columns. `03 OF OF 03` = the chain, two silent bars, the chain again.

**AUDIO columns.** Without cells an AUDIO track plays free, as before (LOOP / ONCE). With cells
it follows the song: `ON` = the file once, at its own length; `OF` = one bar of silence; the
first empty cell ends the column. `ON ON OF ON` is an arrangement.

```
CHAIN 02        BPM 120     CPU 31   BAR 05.1      PLAY
TRACK 1 SYN   SND 2                  ALSO ON TRACK 4
ROW  PAT  TSP                        ROW  PAT  TSP
0    01   +0                         8
1    01   +5                         9
2    03   -12                        A
3    --                              B
ROW 1: PATTERN 1                 PLAY: AUDITION  SHIFT+BLUE: EDIT
```

| Control | Chain view |
|---|---|
| blue turn / green turn | cursor row 0..F / column PAT, TSP, SND |
| white turn | PAT: `--`, 1..14; TSP: -48..+48 semitones (shift: octaves; DRUM: the hits' TONE); SND: `-` (the track's sound) or 1..8 |
| keys 1..8 | the track's sound / kit, at once |
| white press | an empty row: the last pattern used here |
| cut / insert | cut the row into the clipboard / paste it |
| shift + insert / shift + cut, twice | insert an empty row (refused if row F is used) / delete the row |
| play / stop | audition: this chain alone on its track, looping, the other tracks silent / stop |
| blue press / green press | mute / solo the track |
| shift + blue press | the row's pattern in NOTE ROLL / DRUM ROLL |
| join / orange press | back to the song view (ends an audition) |

**Sounds in the song.** A SYNTH track plays one of TE's 8 synth sounds, a DRUM track one of the
8 kits (the track's SND / KIT). A chain row can name another slot in its **SND** column: the
track plays that sound while the row plays, then the next row's (or its own again). So one track
can change its preset per chain: a verse chain on sound 2, a chorus chain on sound 5. **Keys
1..8** set the cursor track's own sound in every view; pressed while a row with its own SND
plays, the key's sound wins until that row ends (live mode: try a sound, the song takes over
again). The list shows the slot playing now (amber when it is a chain row's). A change acts on
the next block: notes still sounding go on with the new sound's parameters. The slot should hold
an SDK engine (TE's own engines cannot run in PLAY); only the 8 slots in memory can be used, no
presets are loaded from flash.

The first empty row ends a chain. Chains are shared: the same chain in a SYNTH column plays NOTE
ROLL's patterns, in a DRUM column DRUM ROLL's. Clipboards, cursors and the start row are kept for
the session; the song itself is in `SONG.BIN`.

## Live mode

**shift + rec** in the song view turns it into **LIVE** (red title): cells cannot be edited, and the keys
cue what plays next.

| Control | Live mode |
|---|---|
| play | cue the cursor row on every column (stopped: start there) |
| white press | cue the cursor cell: its column only |
| T1..T4, shift + T1..T4 | mute / solo track 1..4 |
| blue press / green press | mute / solo the cursor column's track |
| blue turn / green turn | cursor row / column |
| stop | pause; again: bar 1, song row 00, all cues gone |
| shift + rec | back to editing |
| rec, rec again | render from the cursor row (the song as written: cues are not part of it) |

A cued column finishes the chain it is playing, then jumps to the cued row (the cell blinks in a
frame until then) and loops back *there* from then on. An AUDIO column changes at the end of the
`ON` / `OF` segment it plays (a cue less than 93 ms before that end waits for the next segment).
Cueing an empty cell stops the column at that point; a stopped column cued to a chain joins at
the next bar. Shift + play cues a row in the plain song view too. Back / forward, editing AUDIO
cells or song rows, and stop twice put every column back on the start row.

## Render: the song as a file

The OP-1 cannot play many SYNTH tracks at once (see CPU below). A render plays the song
"offline" into a file instead of the output, as slowly as it has to, with every SYNTH track at
full quality: **no ECO and all voices**, whatever the tracks are set to. The file then plays
as an ordinary AUDIO track, which costs almost nothing, and the SYNTH tracks it replaces can be
muted or removed.

1. In the song view (or live mode) put the cursor on the row to start from. Mute / solo what
   should (not) be in the file: the render is what you would hear.
2. **rec**: the footer asks `RENDER 8 BARS FROM ROW 00`. The length is the longest column played
   once; **white turn** changes it (shift: 8 bars). **rec** again starts, any other key cancels.
3. The footer shows the file's name and the progress; the song's playheads move. **stop** (or
   rec) cancels and deletes the file. Other keys do nothing until it is done. TE's own
   instruments stay playable; the render takes about 40 % of the audio time.
4. Done: `REND01.AIF READY`. On the track list add an AUDIO track and pick the file with the
   white knob (or put it in an AUDIO column as `ON` cells).

- The file: `RENDnn.AIF` (01..99), AIFF 16 bit 44.1 kHz, exactly the bars asked for (so it loops
  cleanly; a note ringing past the last bar is cut). Stereo if an AUDIO track is in the mix,
  else mono (half the size, half the stream). About 10 MB a minute in stereo.
- In the file: the tracks at their levels, mute / solo, PLAY's soft clip. Not in it: the master
  fader (rendered at 0 dB), TE's master EQ / FX / drive (they act again when the file plays).
  An AUDIO track's level is -9 dB when added: set a rendered track to **0 dB** to hear it as
  loud as the tracks it replaces.
- Renders live in their own folder (`/yaffs2/user/play_r`), so importing a new set from disk
  mode keeps them. The file pool holds 16 files in all, the set's first. There is no way yet to
  delete a render on the device or to copy it to disk mode.
- How: the audio interrupt renders in steps (one track of the next block per step) within a
  cycle budget instead of mixing into TE's bus; the main loop writes the captured blocks to
  flash. The engines run exactly as when playing, only not in real time.

**CPU.** While playing, the header shows PLAY's share of the audio interrupt's time (percent,
red above 70) and every SYNTH / DRUM row its own share (red above 30); TE's own instruments and
the tape come on top (cpuinfo's `load te`). Ways to keep it down, per SYNTH track:
- **V1 / V2 / V4:** voices at most (default 2; a new note takes the oldest voice). Each voice is a
  full engine render (ACID 3 is mono: it renders once per block whatever the setting).
- **ECO:** the engine renders at 44.1 kHz instead of 88.2 kHz (every other block, 128 samples at
  twice the pitch step = two blocks) and skips the decimator: half the engine's cost. ECO tracks
  take turns (instances 1 and 3 on one block, 2 on the next), so three ECO tracks cost at most two
  renders in any block. ACID 3 knows ECO and keeps its cutoffs, envelopes and slide (its own
  44.1 kHz tables, resonance matched per resonance). **ECO boost** (v0.8.1): an ECO render
  carries `EH_BOOST` while PLAY's work in the last block of the same parity, the boosts already
  granted in this block and the track's own last render stay under 200k cycles (~35 % of a
  block); ACID 3 then runs its filter at 88.2 kHz where its cutoff is above 6 kHz (the top of
  an accent / env-mod sweep: up to twice the render's work in those blocks, nothing extra
  otherwise). Other engines run as if at 88.2 kHz: their own time constants (filter
  envelopes, LFOs, glides) run at half speed and aliasing rises on bright sounds; TE's envelope
  keeps its timing.
- Muted tracks (mute, level -60, another track soloed) render nothing.
Always on: a released voice stops once its envelope is below -60 dB, the SYNTH tracks share one
decimator, and a new note renders before the voices already sounding (a mono engine such as
ACID 3 then renders once in that block instead of twice: the CPU spikes that came with legato
notes).

Limits: up to 8 SYNTH tracks, at most 3 of them sounding at once (v0.8): the 3 engine
instances are a pool. A track takes one for its first note and gives it back once its voices
are silent, so SYNTH tracks that take turns in the song (rest cells, later columns, live cues)
can use 8 different sounds. A note that finds no free instance takes one whose track only has
a release tail left (the tail is cut), else it is dropped and the track shows `BUSY` for ~1 s
(red `SYN` in the song view). Muted tracks take none. 2 DRUM tracks (CPU), 8 tracks in all. A synth sound
whose engine is one of TE's own shows `NOT AN SDK ENGINE`: TE's engines exist once, as TE's six
voices, and TE renders only the sound being played; running one a second time would need each
engine's voice objects rebuilt (reverse engineering per engine). The SDK engines keep their state
per voice slot (24: TE's 6 + 3 instances x 6) and per instance (MONO 3's voice order, ACID 3's mono
voice, MALLET's motor, WHEEL's rotor, PALETTE's budget); keyboard displays stay TE's. That state
passes on with the instance to the next track that takes it.

Patterns are edited in NOTE ROLL / DRUM ROLL. A pattern you never edited there plays TE's own
finger pattern: TE loads those only when its finger sequencer is first opened, so PLAY asks TE to
load them (TE's own loader, synth and drum at once) as soon as op1.db is open after boot and the
song has a SYNTH or DRUM track (v0.3.3; before, such tracks stayed silent until the finger
sequencer had been opened). On the list, a track's pattern is song row 00 of that track (a
chain of one row); longer songs are made in the song view. The track list, the choices, levels, mutes and solos are kept in `SONG.BIN` in
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
  (modulo its length when looped; with song cells through the column's ON / OF segments,
  `py_set_song`); a pattern track walks its song column (py_song.c) and asks
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

Writes: `/yaffs2/user/play_r` (rendered files), `/yaffs2/user/play_a` and `/yaffs2/user/play_b` (the copied set, which take turns;
`ACTIVE` marks the current one) and `SONG.BIN` in the current one. PLAY never writes TE's tape,
its databases or the disk image.

## Tests

- `test_py_core.c` (host): `set.txt`, audio tracks (assign, gone / bad files), the position
  mapping, and a full stream + mix simulation against an independent per-sample reference (once
  and looped files, mono and big-endian, gains, underrun and resync, read errors), pattern inputs,
  song-driven tracks (ON / OF segments: the mapping, and through the ring sample by sample),
  live cues (the switch at the segment's end, the ring taken back, a replaced cue, silence).
- `test_py_song.c` (host): typed tracks, playheads through song rows and chains, an empty chain
  row ends the chain, loop back to the start row, audio cells, polymeter, seek, new / clone,
  save / load; for the views: a chain played alone, chain users, the list's quick-edit rule,
  song / chain row insert and delete, AUDIO segments from a start row, a cued row at the
  chain's end, rest cells, a chain row's sound, an older `SONG.BIN` (PSN2) read.
- `test_py_drum.c` (host): pitch maths, TE's position encoding, one-shot vs gate, reverse, loop
  while held, an octave up, the start delay, the level law, voice stealing, clamping.
- `test_py_synth.c` (host): pitch increments, TE's halfband (DC, 1 kHz passes, 30 kHz gone), the
  voices against stand-ins for an engine and TE's envelope (flags, instance, release after the
  hold, -60 dB release end, release tails (`py_syn_tail`), voice cap and stealing, new notes first, gain and peak, ECO (parity,
  HALF flag, BOOST only with ECO), no engine; the 32-bit decimator bit for bit against the 64-bit sum).
- `sim/load_play.py` (simulator, a measurement, not in test_all: about 10 minutes): three SYNTH tracks on
  ACID 3, a legato note every 1/16, ECO off and on: cycles per block (peak, average), renders per
  track. 2026-11-15: ECO off peak 201k (314k before new-notes-first), ECO on peak 129k (206k
  before the parity split), averages 181k / 92k.
- `sim/run_playtest.py` (simulator, composed with WARP): import, folder switch, tracks added on
  the ADD row, exact mixed samples of once / looped files, a gone file on its own track, keys,
  a DRUM track sample by sample, a SYNTH track on a test engine as instance 1 with TE's envelope
  stand-ins, SONG.BIN saved and read back, removing a track, no yaffs buffer at an odd address,
  TE's finger patterns loaded once op1.db is open (a DRUM track on TE's own pattern plays),
  the SYNTH cost options (voices, ECO renders every other block with HALF and, under light load,
  BOOST, muted renders nothing) and the
  CPU numbers; the song view (cells, clone, clipboard, song rows, play from the cursor row), the
  chain view (rows, audition), AUDIO cells sample by sample over two circles; the help page
  (speech), live mode (a cued row at the chain's end, live keys, an AUDIO cue sample by sample),
  render (an AUDIO track into a stereo AIFF sample by sample, the master not in it, the file
  in the pool; cancel deletes it; a DRUM track into a mono file; keys held off meanwhile),
  rest cells, sounds (keys 1-8, a chain row's SND, a key over it until the row ends), five
  SYNTH tracks on the 3 pooled instances (in turn: all play on instance 1; at once: three play,
  two show BUSY).

License: MIT.
