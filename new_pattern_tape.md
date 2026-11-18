# NEW PATTERN TAPE — the PLAY page

**Status (2026-11-15):** PLAY v0.7 built: one track list of up to 8 AUDIO / SYNTH / DRUM tracks,
master-bus mix, CPU options, and the tracker song (song view, chain view, AUDIO cells: section
2.3). `features/play/README.md` is the current manual. Sections 0-2.2 and 4-7 below describe the
v0.1 design (fixed `A1..A8` / `P1..P4` slots, pages) and are kept as history where the README says
otherwise. Open: device test of live mode and of the render.
**Target image:** `op1plus` (`recipes/op1plus.json`), firmware R246.
**Feature folder, when built:** `features/play/`.

## 0. Decisions

1. PLAY is an **alternative tape screen**. In an image that contains `play`, the tape key opens
   PLAY instead of TE's 4-track tape. TE's tape screen stays **unmodified**: no patches to TE's
   tape code, no writes to `tape.db` or the tape track files.
2. PLAY is **optional per image**. A recipe without `play` gives the stock tape screen. Dropping
   `play` from a recipe must always be safe: PLAY stores nothing, so no stored value can halt a
   later boot (see the LOFI lesson in `docs/lessons.md`).
3. In an image with `play`, both screens stay reachable. **Not `shift + tape`:** on the device
   it opens TE's tape-erase screen (edit mode 10, T1-T4 there erase a track; probe 2026-11-15).
   Proposal: `tape` pressed again while PLAY is shown switches to TE's tape and back (to be checked:
   what TE does on a second `tape` press).
4. A **set** = audio tracks + pattern tracks. The audio group is `A1` (the arrangement) plus up to
   seven optional loops `A2..A8`. Patterns are 0 or 4 (`P4` = drum).
5. A set is **all-or-nothing per file**: one bad file present on disk invalidates the whole set,
   so the page never plays half a song.
6. PLAY's transport is its own. TE's tape transport stays stopped while PLAY plays.

## 1. What the player gets

The tape key opens PLAY: four rows at a time, three pages.

| Page | Rows | Contents |
|---|---|---|
| `AUDIO 1-4` | 4 | `A1` arrangement, `A2..A4` loops |
| `AUDIO 5-8` | 4 | `A5..A8` loops |
| `PATTERN` | 4 | `P1..P3` synth patterns, `P4` drum pattern |

Up to 8 audio + 4 pattern = 12 tracks. The default is no set: three empty pages, silent.

Playback keeps running when the player leaves for synth / drum / mixer, the way TE's tape does;
only `stop` stops it.

## 2. Track model

### 2.1 Audio tracks

| Slot | Role | Required |
|---|---|---|
| `A1` | arrangement: plays once, defines the set length | yes, for any audio set |
| `A2..A8` | loops, each a whole number of bars | no, each optional |

Normative rules:

- Valid audio groups: empty, or `A1` plus any subset of `A2..A8` (gaps allowed, e.g. A1 + A3 + A7).
  Loops without `A1` are invalid.
- Each slot is `Ax.wav` **or** `Ax.aif`; both formats are accepted for every slot. Two files for
  one slot (`A3.wav` + `A3.aif`) is invalid.
- Accepted formats: RIFF/WAVE or AIFF, **PCM 16-bit, 44.1 kHz, mono or stereo**. Anything else
  (24-bit, 48 kHz, compressed AIFC, MP3, FLAC, OGG) invalidates the set in v1.
- **Tempo lock:** the set tempo comes from `set.txt` (`bpm`, section 3); loading the set sets TE's
  tempo to it. Every loop must last a whole number of 4/4 bars at that tempo (tolerance ±64
  samples) or the set is invalid (`A4: NOT WHOLE BARS`). Without a `bpm` line the set tempo is
  TE's tempo at load time, checked the same way.
- All tracks start together at bar 1, sample-locked. `A1` plays through once; loops repeat at
  their own bar length until `A1` ends. With LOOP on, the whole set repeats from bar 1.
- Changing TE's tempo while a set is loaded only changes pattern speed; audio does not stretch in
  v1, so the header shows `BPM 128 ≠ SET 120` as a warning.
- The set is read-only. The page never writes, moves, trims or retags a file.

### 2.2 Pattern tracks (v0.2: decided 2026-11-15, design in progress)

| Slot | Role | Pattern edited in | Plays on |
|---|---|---|---|
| `P1..P3` | synth pattern | NOTE ROLL (synth finger patterns 1..14) | PLAY's own instance of an SDK engine, sound = one of the 8 synth sound slots |
| `P4` | drum pattern | DRUM ROLL (drum finger patterns 1..14) | PLAY's own drum sample player, kit = one of the 8 drum sound slots |

Decisions (user, 2026-11-15):

- Patterns are **edited in NOTE ROLL / DRUM ROLL**, the editors the user already knows; PLAY has no
  step editor. A PLAY track only references a pattern number and a sound slot.
- **All four tracks in v0.2.** PLAY plays them itself on its own clock (bar 1 = the audio set's
  bar 1, the set's tempo), independent of TE's finger sequencer and of the sound TE is playing:
  P1..P3 each run their own engine instance, so three different synth sounds play at once.
- Only **SDK engines** (ACID 3, MONO 3, WARP, MALLET, TRIO, PALETTE, STATION 1, ...) can be pattern
  sounds: TE's own engines exist once, as TE's voices. CPU: ~15 % per ACID-class track (device),
  so the three synth tracks plus drums, TE's base and the audio mix stay below ~85 % only with
  monophonic or light engines; the page shows the load.
- Patterns loop at their own length (1..64 steps of 1/16) from bar 1; the audio set's end (LOOP
  off) stops them too. With no audio set, the patterns play alone (the bar clock runs at the
  set.txt / TE tempo).

Open until the research is in: per-voice engine state for extra instances (enginehost), the
envelope / decimation TE applies after an engine's render, the drum kit in sample memory, how a
track's choice (sound slot + pattern) is stored (proposal: `PATTERNS.TXT` in PLAY's own folder,
written when changed; initial values from `set.txt` `pattern` lines).

### 2.3 Song mode: a tracker (reviewed 2026-11-15, built as PLAY v0.4)

Concepts from the M8 tracker's public manual (structure and workflow only; own design, own look).
PLAY has **one list of up to 8 typed tracks** (AUDIO / SYNTH / DRUM, `features/play`), and the
song grid has one column per track. **Built:** the model and the playheads (`py_song.c`,
host-tested, `SONG.BIN` "PSN2"), the song view, the chain view, AUDIO cells (`py_view.c`,
v0.4), live mode, the 5x7 font and the help pages (v0.5).

Review 2026-11-15 (what changed against the proposal; the former **[decide]** points are decided
as proposed unless listed here):

1. **Knobs.** Since v0.3.4 the list's pattern is on *orange* turn, not white: the list rule below
   says so. Orange turn has no job in the song / chain views; shift + orange turn stays the master.
2. **The play clock.** Song rows are not a time grid, so "play from row R" cannot keep the bar
   counter: play in the song view restarts the clock (bar 1) with every column on the cursor row.
   Play while playing pauses, as on the list; a pause is resumed from the list (play there
   continues), the song view's play always starts at its cursor row. Stop twice = bar 1 and start
   row 00 again.
3. **The list's quick pattern edit** also needs the chain to be *unshared*: a one-row chain that
   another cell uses too is no longer edited from the list (it would change the other track).
4. **AUDIO cells:** white press toggles `ON` / `OF` (an empty cell: `ON`); the proposal left white
   press undefined there. The column's position is a pure function of the play position (no head
   state), so the main loop's read-ahead and the mix agree by construction. Editing an AUDIO
   cell while playing re-seeks that set's rings (as changing a file does).
5. **Clipboard:** two of them (a song cell, a chain row). A chain number cannot be pasted into an
   AUDIO column or `ON` / `OF` into a SYNTH / DRUM column (footer note).
6. **Row insert / delete** act on the whole song row (all columns): columns of equal length stay
   aligned.
7. **Numbers** are hex in both views (song rows `00..7F`, chains `00..7F`, chain rows `0..F`);
   patterns `1..14` and transpose stay decimal, as NOTE ROLL / DRUM ROLL show them.
8. **Chain view:** all 16 rows on one screen (two blocks of 8), no scrolling. Transpose on a DRUM
   track shifts the hits' TONE. The audition ends on stop and when the view is left.
9. **Cursor and playhead:** the playing cell is filled in the track's colour, the cursor is a
   white frame, so both stay readable on the same cell.

#### Levels

| Level | What | Edited in |
|---|---|---|
| **SONG** | 128 rows x 8 columns, column k = track k of the list (same order). SYNTH / DRUM cell: a chain `00..7F` or empty; AUDIO cell: `ON` / `OF` or empty | PLAY, song view |
| **CHAIN** | 16 rows of *pattern* (`1..14` or empty) + *transpose* (-48..+48 semitones) + *sound* (the track's or slot 1..8) | PLAY, chain view |
| **PATTERN** | NOTE ROLL (SYNTH) / DRUM ROLL (DRUM) pattern 1..14, 1..64 steps | NOTE ROLL / DRUM ROLL |

The 128 chains are shared: any column can use any chain. A chain in a SYNTH column plays NOTE
ROLL's patterns, the same chain in a DRUM column DRUM ROLL's (the numbers mean the editor of the
column's type). The chain view names the track it was opened from and the other tracks using it.

#### Playback

- Every track has its own playhead going down its column. A chain row plays its pattern once (the
  pattern's own length), then the next chain row; after row F or an empty row, the next song row.
  An empty song cell ends the column: back to the start row. A cell whose chain has no rows is
  skipped. Columns with different lengths drift apart on purpose (polymeter); equal lengths stay
  aligned. Song rows are therefore **not** a shared time grid: the views show each column's own
  playhead.
- **Start row:** play on the track list at bar 1 starts every column at row 00; play on the song
  view starts the clock again at bar 1 with every column on the cursor row, which is also where
  every column loops back to. A column whose start-row cell is empty is silent.
- **Chain view play:** that chain alone on its track, looping, from bar 1; every other track is
  silent (the track sounds even when it is muted). Stop, or leaving the chain view, ends the
  audition: stopped at bar 1.
- Transport, mute / solo / level, CPU and the master stay the track list's (section 4 / README).

#### Sounds in the song (v0.7)

- A chain row has a third value **SND** (`-` = the track's own slot, `1..8`): the sound / kit
  slot the track plays while that row plays. Per row, not per song cell: a cell is one byte, and
  a chain is the unit that is reused.
- **Keys 1..8** set the cursor track's own sound in the list, the song view, live mode and the
  chain view. While a row with its own SND plays, a key's sound wins until that row ends.
- The slot is read every block (`slot_of` in py_pattern.c), so a change needs no restart; notes
  still sounding continue with the new slot's knobs and envelope.
- `SONG.BIN` is "PSN3" now (chain rows 3 bytes); a "PSN2" file is read and converted in memory
  (rows without a sound), and written as PSN3 at the next save.
- Limits: TE's 8 synth slots / 8 kits as they are in memory. No preset loading from flash.

#### Up to 8 SYNTH tracks, 3 at once (v0.8), 5 at once (v0.9)

SYNTH tracks no longer own an engine instance: the three instances (1..3, enginehost `EH_INST`)
are a pool. A track takes one for its first note and gives it back once its voices are silent
(release below -60 dB), so up to 8 SYNTH tracks can share a song when they take turns (rest
cells, columns that start later, live cues): the song switches between up to 8 sounds and
engines, not 8 in parallel.

- At most 3 SYNTH tracks sound at once. A note that finds no free instance takes one whose
  track only has release tails left (that tail is cut); if every instance holds a note, the new
  note is dropped and the track shows `BUSY` (list, red `SYN` in the song view) for ~1 s.
- A muted track (mute, level -60, another one soloed) takes no instance.
- Memory and CPU stay as in v0.7 (no extra instances): the per-instance engine state (MONO 3's
  voice order, ACID 3's mono voice, MALLET's motor, WHEEL's rotor) passes from one track to the
  next, as it already did when a chain row changed a track's sound.
- DRUM tracks stay at 2 (CPU).
- v0.9: 5 instances (enginehost `EH_INST` 6, ~7 KB of RAM per instance over all engines). The 4th
  and 5th are an overload guard's: taken only while PLAY's peak block (the last of each parity,
  at least 50k cycles per sounding instance while nothing is measured, e.g. every track's first
  note at the song's start) plus one more instance's share stays under 250k cycles. Otherwise
  the note is dropped (`BUSY`), as when all instances are taken.

#### ECO boost (v0.8.1)

ECO renders an engine at 44.1 kHz, every other block. Where that is audible (ACID 3: the top of
an accent / env-mod sweep above 6 kHz, measured 2026-11-17) the engine may run at the full rate
for those parts only: PLAY marks an ECO render with `EH_BOOST` while its CPU budget allows it
(PLAY's work in the last block of the same parity + the boosts granted in this block + the
track's own last render under 200k cycles, ~35 % of a block). Engines that ignore the flag are
unchanged. No setting: ECO stays one switch per track.

#### Rest cells (v0.6.1)

An empty cell ends a column, so a pause inside a column needs a cell of its own: `OF` in a
SYNTH / DRUM column (stored `PS_REST` 0xFE, white turn one step below `00`) is one bar of
silence without a pattern, the same meaning `OF` has in an AUDIO column. Live cues, the start
row and the render treat it as any other cell. A column with a rest is no longer the list's
one-pattern case (`SNG`).

#### AUDIO tracks in the song

- An AUDIO column with **no cells** plays as before: free, from bar 1, LOOP or ONCE on the list.
  Nothing changes for sets without a song.
- As soon as the column has a cell, it follows the song: `ON` = the file from its start, once,
  its own length (a 4-bar loop = 4 bars); `OF` = one bar of silence; empty = the column's end
  (back to the start row). An arrangement is then `ON ON OF ON ...` down the column. The list
  shows `SONG` instead of LOOP / ONCE for such a track.
- Implementation: the cells from the start row to the first empty one are the track's segment
  table (`py_set_song`); `py_map` maps a global frame through it (position modulo the table's
  length), so the read-ahead simply reads the right file frames into the ring and the mix needs
  no change.

#### Getting there and back

- **join** opens the **song view** (v0.8.2: from the list, from the chain view, from any other
  screen; in the song view, plain or live, it stays). **tape** opens the **track list** (from
  another screen and from the song / chain view). Before v0.8.2 join toggled list <-> song view
  and the tape key brought back whichever view was open last.
- **orange press** goes one level up: chain view -> song view -> track list. (On the list it stays
  "re-read the set folder".)
- **white press on a chain cell** opens the chain view. **shift + blue press** opens the pattern in
  NOTE ROLL / DRUM ROLL, as on the list (song view: the pattern the column plays now, else the
  cell's chain's first row; chain view: the cursor row's pattern); shift + blue press closes it.
- The list stays the home screen; PLAY remembers the view and cursors for the session.

#### Song view

```
SONG            BPM 120     CPU 31   BAR 05.1      PLAY
     1SYN     2DRM     3AUD     4SYN     5DRM
00   00       01       ON       03       --
01   00       01       ON       03
02   02       04       OF       --
03   --       04       ON
04            --
CHAIN 02 ON TRACK 1     WHITE PRESS: OPEN  SHIFT: CLONE
```

- Columns: the list's tracks (number + type in the type's colour), 36 px each; rows 11 px, 8
  visible, scrolling with the cursor. Empty cells blank, the column's end `--`.
- The cell each column plays now is filled in the track's colour (no glyph: the 3x5 font has no
  `>`); the cursor is a white frame around one cell.
- Footer: what the cursor cell is, and the keys that apply to it. Without tracks:
  `NO TRACKS: ADD THEM ON THE LIST - TAPE`.

| Control | Song view |
|---|---|
| blue turn | cursor row (shift: 16 rows) |
| green turn | cursor column (track) |
| white turn | the cell's value: empty / `OF` (a rest) / chain `00..7F` (shift: 16 at a time); AUDIO: empty / `ON` / `OF` |
| white press | empty cell: the last chain used in this column (none yet: a new unused chain); chain cell: open the chain view; AUDIO: `ON` <-> `OF` (empty: `ON`) |
| shift + white press | empty cell: a new unused chain; chain cell: clone it (a copy under a new number, the cell points to the copy) |
| blue press / green press | mute / solo the cursor column's track (as on the list) |
| cut | cut the cell into the clipboard (one press: the clipboard keeps it) |
| insert | paste the clipboard into the cell |
| shift + insert | a new empty row at the cursor (rows below move down; refused if row 7F is used) |
| shift + cut, twice | delete the cursor row (rows below move up) |
| T1..T4 / shift + T1..T4 | cursor to column 1..4 / mute it |
| play | start at the cursor row, bar 1 (playing: pause) |
| shift + play | cue the cursor row on every column (live mode below) |
| rec, rec again | render the song from the cursor row (below) |
| shift + rec | live mode on / off |
| speech | the help page |
| stop / back / forward | as on the list |
| shift + orange turn | master |
| shift + blue press | the pattern in NOTE ROLL / DRUM ROLL |
| tape / orange press | back to the track list |

#### Chain view

```
CHAIN 02    BPM 120     CPU 31   BAR 05.1      PLAY
TRACK 1 SYN   SND 2          ALSO ON TRACK 4
ROW  PAT  TSP              ROW  PAT  TSP
0    01   +0               8    --
1    01   +5               9
2    03   -12              A
3    --                    B
...
ROW 1: PATTERN 1 +5         PLAY: AUDITION  SHIFT+BLUE: EDIT
```

| Control | Chain view |
|---|---|
| blue turn | cursor row 0..F |
| green turn | column PAT / TSP |
| white turn | PAT: `--`, 1..14; TSP: -48..+48 (shift: octaves) |
| white press | an empty PAT: the last pattern used here (none yet: pattern 1) |
| cut / insert | cut the row into the clipboard / paste it |
| shift + insert / shift + cut twice | insert an empty row (refused if row F is used) / delete the row (the rest moves) |
| blue press / green press | mute / solo the track |
| shift + blue press | the row's pattern in NOTE ROLL / DRUM ROLL |
| play / stop | audition this chain on its track / stop |
| shift + orange turn | master |
| join / orange press | back to the song view |

The first empty row ends the chain; rows after it are shown dim (they do not play).

#### The track list with a song

- The list's pattern number shows the pattern the track plays now (while stopped: song row 00's).
- Orange turn on a SYNTH / DRUM row keeps editing song row 00's chain, first row (the quick way
  for a one-pattern track) **only while** the column is just row 00 (or empty), its chain one row
  long and used by no other cell. Otherwise the label reads `SNG` instead of `PAT` and orange turn
  does nothing (before v0.4 it cut the chain to one row).
- On the list the pattern is 1..14 and the sound / kit 1..8, both held at the ends (no `--`, no
  CUR); a new SYNTH / DRUM track starts on TE's current slot and pattern 1. An empty cell is made
  in the song view only. A column that is empty at its start row is silent; given a chain there
  while playing, it joins at the next bar.
- An AUDIO track with cells shows `SONG` instead of LOOP / ONCE; white press does nothing there.

#### Storage

Unchanged: `SONG.BIN` ("PSN2", CRC) holds the grid, the chains and the track settings; the
clipboards, the cursors, the view, the "last value" per column and the start row are session-only.

#### Live mode (built, v0.5)

- **shift + rec** in the song view = live mode on / off (title `LIVE`, red; rec alone was the
  key in v0.5, it is the render now). In live mode the cells are
  not edited: **play** cues the cursor row on every column (stopped: play starts there), **white
  press** cues the cursor cell on its column only, T1..T4 mute (shift: solo). **Shift + play**
  cues the cursor row in the plain song view too.
- A cued SYNTH / DRUM column finishes its current chain, then jumps to the cued row, which is its
  start row from then on (it loops back there). A cued empty cell stops the column; a stopped
  column cued to a chain joins at the next bar. A second cue before the jump replaces the first.
- A cued AUDIO column (one with cells) changes at the end of the `ON` / `OF` segment it plays:
  the track gets a second segment table that takes over at that frame (`py_set_cue`), the ring
  is taken back to it and read again. A cue less than 4096 frames (93 ms) before the segment's
  end waits for the next segment. A free AUDIO track (no cells) ignores cues.
- The cued cell blinks in a frame of the track's colour until the jump.
- Back / forward, editing AUDIO cells or song rows, and stop twice put every column back on the
  global start row (cues and per-column start rows are session state of the running play).

#### Render to a file (built, v0.6)

Why: the CPU limits the SYNTH tracks (5 instances with a CPU guard, ECO, voice caps). A render plays the song
into a file, not in real time, at full quality; the file is then one AUDIO track.

- **rec** in the song view or live mode asks `RENDER n BARS FROM ROW rr` (n = the longest
  column played once from the cursor row; white turn changes it, shift: 8 bars); **rec** again
  starts; any other key cancels. While it renders, stop or rec cancels (the file is deleted) and
  every other key and knob is held off.
- Rendered: the tracks as they sound (levels, mute, solo), SYNTH tracks **without ECO and with all
  voices**, PLAY's soft clip, the master at 0 dB. Not TE's master EQ / FX / drive. Exactly n bars.
- File: `/yaffs2/user/play_r/RENDnn.AIF`, AIFF 16 bit 44.1 kHz, stereo if an AUDIO track is in
  the mix, else mono. The render folder is read into the file pool after the set folder (16 files
  in all) and survives imports (the set folders `play_a|b` take turns and are cleared).
- Implementation (`py_render.c`): the audio interrupt, instead of mixing into TE's bus, renders
  in steps (`py_pat_step`: one track of the next block per step, then the buses and `py_mix`)
  until ~40 % of a block's cycles are used; a block starts only when every AUDIO ring holds it
  and the capture ring has room. The main loop writes the capture ring to the file. The engines
  are only called from the interrupt, as when playing.
- Open: deleting renders on the device, exporting them to disk mode, a tail past the last bar.

#### The tape key (v0.6)

Decision 3 of section 0 is withdrawn for this image: PLAY is the only tape screen. Tape again and
shift + tape do nothing on PLAY (TE's tape screen and its erase screen are not reachable); TE's
tape data is still never touched. v0.8.2: the tape key always shows PLAY's track list; join on
another screen injects a tape key press (TE_KEY_PUSH) and PLAY opens in the song view.

#### Views: font and help (v0.5)

The list, the song view and the chain view use the SDK's 5x7 font (`gfx_text7`, 52 characters a
line) instead of the 3x5 one; a two-line footer (what the cursor is on; the keys). **speech**
(without shift) shows a help page for the current view (track list, song, chain, live), blue
turn pages, any key closes it.

#### Build order

1. Done (v0.4): song view: columns, cells, playheads, cursor; join / orange press in and out;
   play from the cursor row.
2. Done (v0.4): song view editing: values, last value, new / clone, clipboard, row insert /
   delete; the list's orange turn rule above.
3. Done (v0.4): chain view: values, rows, audition play.
4. Done (v0.4): AUDIO cells (`ON` / `OF`, song-driven streaming).
5. Done (v0.5): live mode; the larger font; the help pages.

## 3. Disk layout and validation

The set lives in a folder **`play/` at the root of the OP-1 disk** (disk mode, next to
`album/ drum/ synth/ tape/`):

```
play/
  A1.wav          arrangement (required for audio)
  A2.aif          optional loops, any of A2..A8, .wav or .aif
  A5.wav
  set.txt         optional
```

**Device finding (probe 2026-11-15): TE deletes `play/` from the image.** After disk mode, the
image's root keeps only `TAPE` and `ALBUM`; `DRUM`, `SYNTH`, `PLAY` and macOS's folders are deleted
entries (`E5`). TE's disk sync (`disk/fat_manager.cpp` 0x01996530 "Sync after reboot. %d files
removed", `filesync_common.cpp`, `filesync_yaffs.cpp`, `Defragmenter.cpp`) imports synth/ and drum/
into yaffs and clears the rest. So the set cannot live in the image. Plan: **import on disk-mode
exit**, like TE does with synth/drum: hook TE's sync before it deletes, copy `play/*.wav|aif` +
`set.txt` (validated with `pp_fat.c`) to `/yaffs2/user/play/`, and PLAY reads those yaffs files
directly (`TE_FILE_OPEN` + pread). Optionally export them back into `play/` on disk-mode entry, so
the set is visible and editable there, as TE does with synth/drum. Hook point: to be found in the
sync code (RE).

Disk mode's volume is a FAT image inside `user/tape_c.raw`, not yaffs files, so `TE_FILE_OPEN`
cannot open `play/A1.wav` directly. v1 reads it with a small **read-only FAT reader over
`tape_c.raw`** (or TE's own import path, if the probe finds one). PLAY never writes the image.

`set.txt` (optional, one setting per line):

```
bpm 120
audio A1 "song" A2 "drums" A3 "bass" A5 "pad"
pattern P1 "lead" P2 "bass" P3 "chords" P4 "beat"
loop on
```

- Without `set.txt`: file names are shown, the tempo is TE's, LOOP is off.
- A malformed line is ignored and shown as a warning (`SET.TXT LINE 3`); names fall back to file
  names. A malformed audio file still invalidates the set.
- Validation order: folder exists -> `A1` present if any audio file is -> one file per slot ->
  every header opens and matches the format -> loops are whole bars -> memory budget (3.1) ->
  pattern group absent or complete. The first failure is shown on screen.
- Files other than `A1..A8.{wav,aif}` and `set.txt` are ignored.

### 3.1 Memory budget

**Device numbers (probe 2026-11-15), reading `tape_c.raw` on yaffs from the main loop, one read
per pass (~125 passes/s):**

| Read size | Throughput | Average read | Worst read (5 s / overall) |
|---|---|---|---|
| 4 KB | 506 KB/s (limited by the loop rate) | 0.6 ms | 0.9 / 29.2 ms |
| 16 KB | 1636 KB/s | 2.3 ms | 2.8 / 29.2 ms |
| 64 KB | 3855 KB/s | 9.3 ms | 9.7 / 12.8 ms |
| 128 KB | 5021 KB/s | 18.6 ms | 19.0 / 19.2 ms |

One stereo stream needs 176 KB/s; eight need 1.4 MB/s. Raw speed is ~7 MB/s (128 KB in 18.6 ms),
so **all eight tracks can stream from yaffs**: about 32 KB per read (~4.6 ms main-loop stall)
gives ~2.7 MB/s. Free sample heap is only ~2.1 MB (one block), too little to preload 7 loops, but
plenty for ring buffers (8 x ~44 KB = 250 ms each, covers the 30 ms worst read + loop jitter).
So v1 streams every track, loops included, and preloads nothing. The rules below are kept for a
later preload option.


- Eight stereo streams from NAND = 1.4 MB/s, and a yaffs read can take milliseconds (thread only).
  There is no free RAM outside TE's heaps (`docs/performance.md`, RAM investigation).
- Plan: **loops are preloaded** into TE's sample heap (`TE_HEAP_MALLOC(TE_HEAP_SAMPLES, …)`);
  **only `A1` streams**, through a ring buffer (~0.5 s, 88 KB stereo) filled from `main_tick`.
- Total loop size must fit in the sample heap's measured free space minus a margin, otherwise
  `SET INVALID - LOOPS TOO BIG (9.1 / 6.0 MB)`. Memory is freed when the set is unloaded.
- Underrun (A1 read falls behind): output that track silent, flag `UNDERRUN` in the header, and
  keep the clock running; never repeat stale buffer data.

## 4. Transport and mix

- PLAY's own transport: `play` starts from the current bar, `stop` stops (second `stop` resets
  to bar 1). Bar.beat is shown in the header.
- TE's tape transport stays stopped (`TE_TAPE_STATE == 0`) while PLAY plays. PLAY never seeks or
  arms TE's tape.
- Audio enters the output **after TE renders each block** (wrapping `TE_AUDIO_BLOCK_CB` the way
  `cpuinfo` does, or a found mix point). Before the master is applied or after it: decided by the
  probe.
- Mix: per-track level (default **-9 dB**), mute, solo, master level, and a soft clip on PLAY's
  sum so eight stems cannot hard-clip TE's output.
- **Volume:** audio added after TE's render is not affected by the OP-1's volume knob (probe
  2026-11-15): TE applies the volume digitally inside its render. PLAY must read TE's volume and
  apply it to its own sum (variable still to be found), or find a mix point before the volume.
- Device facts (probe 2026-11-15): one ACID 3 voice costs ~15 % (`SDK ENG` 15.3 one note, 15.5
  chord: ACID is monophonic), so three pattern synths of that class plus drums and TE's ~10 % base
  reach ~65-70 %: pattern engines need a per-engine budget.
- Device facts (probe 2026-11-15): output word 0 = left, word 1 = right; the 8-stream mix costs
  ~3.5 % of the audio block; TE's sample heap had ~2.2 MB free (one block, free list) with a synth
  and a drum kit loaded: about 12 s of stereo loops, so the loop budget is tight.
- Recording or exporting the mix is out of scope for v1.

## 5. Screen

Full panel over TE's frame (320 x 156 UI coordinates, `gfx`):

```
PLAY        SET            BPM 120   BAR 01.1        PLAY
--------------------------------------------------------------
A1  WAV   song                 -09 dB   M  S
A2  AIF   drums  4 BAR         -09 dB   M  S
A3  AIF   bass   8 BAR         -12 dB   M  S
A4  ---                           --
--------------------------------------------------------------
<  AUDIO 1-4  >                          SHIFT+TAPE: TE TAPE
```

- Header: set state (`SET` / `NO SET` / `INVALID`), tempo (or tempo warning), bar.beat,
  transport, `UNDERRUN` when it happened.
- Rows: slot, format badge, name, loop length in bars, level/meter, mute, solo.
- Colours: arrangement amber, loops green, synth patterns blue, drum red, empty rows dim.
- Empty state: `NO SET - PUT A1.WAV (+ LOOPS) IN PLAY/`.
- Invalid state: `SET INVALID` plus the first reason (`A5.AIF: 48 KHZ`, `A3: TWO FILES`,
  `LOOPS WITHOUT A1`, `A4: NOT WHOLE BARS`, `LOOPS TOO BIG`).
- Meter: one bar per row, peak-hold tick; silent tracks show `--`.

## 6. Controls

| Control | Action |
|---|---|
| `tape` | open PLAY (instead of TE's tape screen) |
| `tape` again | switch to TE's tape screen / back to PLAY (session only; `shift + tape` is TE's erase screen) |

On the PLAY screen:

| Control | Action |
|---|---|
| `play` | start / pause |
| `stop` | stop; again: reset to bar 1 |
| `t1..t4` | select row 1..4 on the current page |
| `shift + t1..t4` | mute row 1..4 |
| blue turn | move the row cursor |
| blue press | mute / unmute the cursor row |
| green turn | level of the cursor row |
| green press | solo / unsolo the cursor row |
| white turn / press | previous / next page |
| orange turn | master level |
| orange press | re-read `play/` (stops first; no partial load) |
| back / forward | one bar back / forward |
| `rec` | reserved, does nothing in v1 |

Claims (checked by `compose.py`): `tape_press` (shift any), and on screen `tape`: `play`, `stop`,
`back`, `forward`, `rec`, `t1..t4`. While TE's tape screen is shown through `shift + tape`, PLAY
releases them and TE's keys work as stock. `noalbum` claims `play_press` on the album screen only:
no overlap.

## 7. Persistence

- v1 stores nothing: no flash writes, no `op1.db` / `tape.db` rows, no settings. Dropping `play`
  from a recipe is therefore always safe.
- The `play/` folder is the only state. Page, cursor, levels, mutes and the PLAY / TE-tape choice
  are session-only (reset to defaults at boot).

## 8. Implementation plan (later; not part of this spec)

1. **Probe** — built: `features/playprobe` (recipe `recipes/op1play.json`; device procedure in its
   README). Its `pp_fat.c` (read-only FAT16/32 + WAV/AIFF headers) is meant to be reused by
   `play`. To be measured on the device:
   - mix point: add a test tone after TE's render; channel order; does the volume knob act on it;
   - disk: read `play/A1.wav` from the FAT volume in `tape_c.raw`; does TE ignore an unknown root
     folder when leaving disk mode?
   - read speed: sustained yaffs read rate from `main_tick`, worst-case latency;
   - sample heap: free bytes with typical synth/drum sounds loaded;
   - CPU: the 8-stream mix cost, and the SDK engines' cost per voice (one note vs. full chords).
     A second engine instance cannot be probed without the pattern player: engines keep their
     state per TE voice (0..5), so P1..P3 need engines that take a voice table of their own;
   - keys: `shift + tape` and `t1..t4` on the tape screen (the probe's key log).
2. **Page shell:** screen, empty/invalid states, tape-key takeover, `shift + tape` switch, claims,
   simulator screenshot bench.
3. **Set loader:** FAT reader, validation chain, `set.txt`, memory budget.
4. **Audio:** `A1` streaming + preloaded loops, sample-locked mix, levels, mute/solo, soft clip,
   bar counter, underrun handling.
5. **Patterns:** pattern player, three engine instances + drum on one clock, CPU check.
6. **Tests:** simulator benches for every valid/invalid set; device test; then add `play` to
   `recipes/op1plus.json`.

## 9. Non-goals (v1)

- No loops without `A1`, no more than 8 audio tracks, no two files per slot.
- No pattern counts other than 0 or 4; no second drum pattern; no dsynth as a pattern engine.
- No 24-bit / 48 kHz / compressed files, no resampling, no time-stretching.
- No per-track offsets, no recording, no exporting the mix to the album.
- No writing, trimming or converting the user's files; no changes to TE's tape code or data.

## 10. Acceptance criteria

1. Image without `play`: stock tape screen, byte-identical behaviour.
2. Image with `play`, empty `play/`: tape key shows `NO SET`, silent; `shift + tape` shows TE's
   tape, which records and plays as stock.
3. `A1` alone, `A1` + 7 loops, and `A1` + gaps (A3, A7) play in sync, with working mute, solo,
   level, master and bar counter; loops stay locked to bar 1 for the whole arrangement.
4. Rejected with `SET INVALID` and silent: loops without `A1`, two files for one slot, a 48 kHz or
   24-bit file, a corrupt header, a loop that is not whole bars, loops over the memory budget.
5. Four pattern tracks (`P4` drums) start and stop on the same bar 1 as the audio and follow
   tempo changes; full set peak CPU < 85 % on the device.
6. Playback continues across mode changes; `stop` stops everything; orange press re-reads disk.
7. No file on disk and no row in TE's databases is written or modified by the feature.
