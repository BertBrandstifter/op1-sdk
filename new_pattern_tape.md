# NEW PATTERN TAPE — the PLAY page

**Status (2026-11-15):** PLAY v0.3 built and device-verified: one track list of up to 8 AUDIO /
SYNTH / DRUM tracks, master-bus mix, CPU options (`features/play/README.md` is the current
manual). Sections 0-2.2 and 4-7 below describe the v0.1 design (fixed `A1..A8` / `P1..P4` slots,
pages) and are kept as history where the README says otherwise. **Section 2.3 (song / chain
views) is the open design.**
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

### 2.3 Song mode: a tracker (proposal 2026-11-15, rewritten for the track list)

Concepts from the M8 tracker's public manual (structure and workflow only; own design, own look).
This section replaces the first version (11 fixed tracks `P1..P4` / `A2..A8`, page keys T1-T4):
since v0.2 PLAY has **one list of up to 8 typed tracks** (AUDIO / SYNTH / DRUM, `features/play`),
and the song grid has one column per track. **Built:** the model and the playheads
(`py_song.c`, host-tested, `SONG.BIN` "PSN2"). **Open:** the views below, AUDIO cells, live mode.
Points marked **[decide]** are proposals waiting for a yes.

#### Levels

| Level | What | Edited in |
|---|---|---|
| **SONG** | 128 rows x 8 columns, column k = track k of the list (same order). SYNTH / DRUM cell: a chain `00..7F` or empty; AUDIO cell: `ON` / `OF` or empty | PLAY, song view |
| **CHAIN** | 16 rows of *pattern* (`1..14` or empty) + *transpose* (-48..+48 semitones) | PLAY, chain view |
| **PATTERN** | NOTE ROLL (SYNTH) / DRUM ROLL (DRUM) pattern 1..14, 1..64 steps | NOTE ROLL / DRUM ROLL |

The 128 chains are shared: any column can use any chain. A chain in a SYNTH column plays NOTE
ROLL's patterns, the same chain in a DRUM column DRUM ROLL's (the numbers mean the editor of the
column's type). The chain view names the track it was opened from.

#### Playback (built)

- Every track has its own playhead going down its column. A chain row plays its pattern once (the
  pattern's own length), then the next chain row; after row 15 or an empty row, the next song row.
  An empty song cell ends the column: back to the start row. Columns with different lengths drift
  apart on purpose (polymeter); equal lengths stay aligned. Song rows are therefore **not** a
  shared time grid: the views show each column's own playhead.
- **Start row:** play on the track list starts every column at row 00 (as now); play on the song
  view at the cursor row, which is also where every column loops back to.
- **Chain view play:** that chain alone on its track, looping; the other tracks are silent until
  stop (an audition). **[decide]**
- Transport, mute / solo / level, CPU and the master stay the track list's (section 4 / README).

#### AUDIO tracks in the song **[decide]**

- An AUDIO column with **no cells** plays as today: free, from bar 1, LOOP or ONCE on the list.
  Nothing changes for sets without a song.
- As soon as the column has a cell, it follows the song: `ON` = the file from its start, once,
  its own length (a 4-bar loop = 4 bars); `OF` = one bar of silence; empty = the column's end
  (back to the start row). An arrangement is then `ON ON OF ON ...` down the column. The list
  shows `SONG` instead of LOOP / ONCE for such a track.
- Implementation: the audio ring keeps streaming by global frame; the mix maps a song-driven
  track's frame through its head (file frame = g - head start while `ON`) and re-seeks the ring
  when a head starts a new `ON` (the ring is read ahead from the next head's start).

#### Getting there and back **[decide]**

- **join** (unused on the list today) switches **track list <-> song view**. In the chain view,
  join goes to the song view too.
- **orange press** goes one level up: chain view -> song view -> track list. (On the list it stays
  "re-read the set folder".)
- **white press on a chain cell** opens the chain view. **shift + blue press** opens the pattern in
  NOTE ROLL / DRUM ROLL, as on the list (song view: the pattern the column plays now, else the
  cell's chain's first row; chain view: the cursor row's pattern); shift + blue press closes it.
- The list stays the home screen; PLAY remembers the view and cursors for the session.

#### Song view

```
SONG                    BPM 120   CPU 31   BAR 05.1      PLAY
     1SYN  2DRM  3AUD  4SYN  5DRM
00   00    01    ON    03    --
01   00    01    ON    03
02   02    04    OF    --
03   --    04    ON
04         --
CHAIN 02 ON TRACK 1     WHITE PRESS: OPEN  SHIFT: NEW / CLONE
```

- Columns: the list's tracks (number + type in the type's colour), 36 px each; rows 11 px, about
  9 visible, scrolling with the cursor. Empty cells blank, the column's end `--`.
- The cell each column plays now is filled in the track's colour (no glyph: the 3x5 font has no
  `>`); the cursor is the list's selection bar on one cell.
- Footer: what the cursor cell is, and the keys that apply to it.

| Control | Song view |
|---|---|
| blue turn | cursor row (shift: 16 rows) |
| green turn | cursor column (track) |
| white turn | the cell's value: chain `00..7F` / empty (shift: 16 at a time); AUDIO `ON` / `OF` / empty |
| white press | empty cell: the last chain used in this column (M8's "last value"); chain cell: open the chain view |
| shift + white press | empty cell: a new unused chain; chain cell: clone it (a copy under a new number, the cell points to the copy) |
| blue press / green press | mute / solo the cursor column's track (as on the list) |
| cut | cut the cell into the clipboard (one press: the clipboard keeps it) |
| insert | paste the clipboard into the cell |
| shift + insert | a new empty row at the cursor (rows below move down; refused if row 127 is used) |
| shift + cut, twice | delete the cursor row (rows below move up) |
| T1..T4 / shift + T1..T4 | cursor to column 1..4 / mute it |
| play / stop / back / forward | as on the list; play starts at the cursor row |
| shift + blue press | the pattern in NOTE ROLL / DRUM ROLL |
| join / orange press | back to the track list |

#### Chain view

```
CHAIN 02   TRACK 1 SYN  SND 2           BAR 05.1      PLAY
ROW  PAT  TSP
0    01   +0
1    01   +5
2    03   -12
3    --
CHAIN 02 IS ALSO USED BY TRACK 4    SHIFT+BLUE: EDIT PATTERN
```

| Control | Chain view |
|---|---|
| blue turn | cursor row 0..15 |
| green turn | column PAT / TSP |
| white turn | PAT: `--`, 1..14; TSP: -48..+48 (shift: octaves) |
| white press | an empty PAT: the last pattern used here |
| cut / insert | cut the row into the clipboard / paste it |
| shift + insert / shift + cut twice | insert an empty row / delete the row (the rest moves) |
| shift + blue press | the row's pattern in NOTE ROLL / DRUM ROLL |
| play / stop | audition this chain on its track / stop |
| orange press | back to the song view |

#### The track list with a song

- The list's PAT column shows the pattern the track plays now (while stopped: song row 00's).
- White turn on a SYNTH / DRUM row keeps editing song row 00's chain, first row (the quick way
  for a one-pattern track) **only while** the column is just row 00 and its chain one row long.
  Otherwise the row shows `SONG` and white turn does nothing (today it would cut the chain to one
  row: a bug to fix with this).

#### Storage

Unchanged: `SONG.BIN` ("PSN2", CRC) holds the grid, the chains and the track settings; the
clipboard, the cursors, the "last value" per column and the start row are session-only.

#### Live mode (later)

Shift + play on a song row cues it: every column jumps there when its current chain ends; a
single cell cues only its column. Not part of the first song-view version.

#### Build order

1. Song view, read only: columns, cells, playheads, cursor; join / orange press in and out;
   play from the cursor row (sim bench: the drawn playheads follow the heads).
2. Song view editing: values, last value, new / clone, clipboard, row insert / delete; the list's
   white turn rule above.
3. Chain view: values, rows, audition play.
4. AUDIO cells (`ON` / `OF`, song-driven streaming).
5. Live mode.

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
