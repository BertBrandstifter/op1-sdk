# PLAY

A second tape screen with one list of up to **8 tracks** in any order. Each track is one of:

- **AUDIO**: a WAV / AIF file from the set folder, looped or played once;
- **SYNTH**: NOTE ROLL patterns, played with one of the 8 synth sounds on any synth engine: TE's
  own (FM, digital, string, cluster, pulse, sampler, phase, dna, dsynth, voltage, iter; v0.13) or
  an SDK engine (ACID 3, MONO 3, STATION 1, PARTY, PALETTE, MALLET, WHEEL, WARP, TRIO), each track
  on its own instance of the engine; up to 8 SYNTH tracks share 6 instances, so at most 6 sound at
  once (the 4th to 6th while the CPU allows; they take turns in the song);
- **DRUM**: DRUM ROLL patterns, played from one of the 8 drum kits.

Everything runs in sync from bar 1, with level, mute, solo, master and a bar counter. PLAY is the
tape screen of an image that contains it: TE's own 4-track tape screen is not reachable there
(until v0.5 a second press of tape switched to it; TE's tape data is never touched). The tape
key always shows the track list; join opens the song view from any screen (v0.8.2). A tracker song arranges it all: the
**song view** (join, from any screen) has one column per track, the **chain view** strings patterns together,
**live mode** (shift + rec in the song view) cues rows while it plays, and **rec** renders
the song into an AIFF file that plays back as one cheap AUDIO track (below). **speech** shows a help
page for the view you are in. The track list and the song view follow a design handover (v0.17,
`SCREENS.md`): thin numerals, a colour per track, live meters; the other pages use the SDK's 5x7 font. Spec: `new_pattern_tape.md` in the SDK root (section 2.3).

## The screen

Two layouts of the same track list; **tape on the list switches between them** (v0.17), and the
choice stays across power-off (`/yaffs2/user/play_s/ROWS.LAY` there = ROWS):

- **LANES**: four tracks side by side, each a lane, top to bottom: its number and type, the
  engine (AUDIO: the file), the level as a big number with a live meter, `SND n PAT n` (or
  `SNG n` when the song plays it; dimmer: chosen, starts when the playing one ends; AUDIO: LOOP /
  ONCE / SONG), then M, S, voices (`V2`) and ECO in one line. A fifth column: the `+ ADD` lane, or the
  tracks outside the window (more than 4).
- **ROWS**: one row per track (four on screen, the list scrolls): number, type, `SND n` over
  `PAT n` / `SNG n`, voices (`V2`) and ECO, a wide meter, the level, M / S.

Everything in a lane / row is drawn in its track's colour, which follows the encoders: track 1
blue, 2 green, 3 white, 4 red (5-8 again); a muted track dims (with a solo on: every track
not soloed, here and in the song view), red stays for warnings (BUSY,
an engine PLAY cannot play, a file gone, CPU above 30 %). The meters run -48 to +6 dB (red from
0 dB) with a falling white peak; the small triangle under a meter is the level set. No hint line
at the bottom (the panel's edge hides it): notes (an armed cut / insert, the master, the
layout) appear there for a moment. The header shows the tempo (BPM) and, while playing, PLAY's
CPU share; each SYNTH / DRUM track shows its own. The song view's header: only SONG, the loop and
the transport.

The song view (join) is the tracker alone: every column (up to 8 on screen, sharing the width;
the header shows each track's number), the rows with the cursor row marked, each column's playing cell tinted, the loop as a red bracket with its times.

```
PLAY 4 TRK . CPU 31     BPM 120   BAR 03.2           > PLAY
 1 SYN         | 2 SYN     12% | 3 SYN     16% | 4 DRM      4% | (+) ADD
 ACID 3        | MALLET        | STATION 1     | DRUM          |  SYNTH
 -9 dB         | -9 dB         | 0 dB          | -9 dB         |
 [meter]       | [meter]       | [meter]       | [meter]       |
 SND1 PAT1     | SND2 PAT1     | SND2 SNG2     | KIT3 SNG5     |
 (M)(S) V2 ECO | (M)(S) V4 ECO | (M)(S) V4 ECO | (M)(S)        |
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
| shift + join | the songs page (save / load, see Songs) | the songs page |
| speech | the help page (blue turn: the other pages; any key: back) | |
| shift + white press | SYNTH: ECO on / off | |
| orange turn | SYNTH / DRUM: the pattern 1..14 (held at the ends); while playing it shows at once in amber and starts when the playing pattern ends (white then); a track with a longer song shows `SNG` and is edited in the song view | |
| shift + orange turn | master | master |
| orange press | re-read the set folder | |
| cut, cut again | remove the track | |
| insert, insert again | a new track before this one (white turn while armed: its type) | a new track at the end |
| T1..T4, shift + T1..T4 | cursor to track 1..4 / mute it | |
| play / stop / back / forward | play-pause / pause, again bar 1 / one bar | |
| tape | the layout: LANES <-> ROWS (from another view: the list first) | the same |

**Tempo = TE's tempo page** (v0.14; metronome key, blue knob BPM, shift + blue fine). PLAY
follows it live, playing or not: every track keeps its place in the bar and its pattern, the
notes sound on, the next steps come at the new tempo. AUDIO files are not stretched: they keep
their speed and pick up at the scaled position (while they play, PLAY waits ~0.1 s for the knob
to rest, then re-reads them once). The header's `BPM` shows it. Before v0.14 PLAY took the
tempo once, when the set was loaded, and a `set.txt` with `bpm` overrode it.

**play / stop on every screen** (v0.13.1): on the synth, drum, mixer, tempo and sequencer screens
play and stop drive PLAY too (play: start / pause, stop: pause, again bar 1; during a render stop
cancels it). TE keeps them on the album screen (album playback), with shift held, with rec held
(rec + play still records TE's hidden tape), while TE's tape runs, and while REQUANTIZE drives
TE's tape.

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

**Loop section** (v0.16): rows `a..b` play `n` times before the song goes on below them, for
example rows 02-05 twice: 00 01 02 03 04 05 02 03 04 05 06 ... Set it in the song view: cursor on
the first row, **shift + back**; on the last row, **shift + forward**; **orange turn** sets the
times (2 to 16, `X1` turns it off). An amber bracket marks the rows, with `X2` on its first row;
each change shows `LOOP 02-05 X2` in the footer. Every column counts its own passes (a column
leaving row `b` jumps back to row `a`), AUDIO columns too; when the song starts again from its
start row, the loop does again. A song started inside the loop also goes back to its first row.
Inserted or deleted song rows move the loop with its rows. It is kept with the song (`SONG.BIN`
and song slots: spare bytes of the format, so older files and slots still load, without a loop).

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
| shift + back / shift + forward | the loop section: its first / last row = the cursor row (none yet: a new one, twice) |
| orange turn | the loop's times `X2` .. `X16`; `X1` = off (the rows stay for later) |
| shift + blue press | the pattern in NOTE ROLL / DRUM ROLL |
| keys 1..8 | sound / kit 1..8 of the cursor column's track, at once |
| shift + orange turn | master |
| tape | back to the track list (join stays: join always opens the song view; orange press: nothing, v0.17) |
| shift + join | the songs page: save / load the song with its patterns (see Songs) |

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
the next block: notes still sounding go on with the new sound's parameters (a sound on another engine:
they start again on it). Any engine works (TE's own since v0.13; a sampler sound needs its
sample loaded: `SAMPLER: NO SAMPLE` otherwise); only the 8 slots in memory can be used, no
presets are loaded from flash.

The first empty row ends a chain. Chains are shared: the same chain in a SYNTH column plays NOTE
ROLL's patterns, in a DRUM column DRUM ROLL's. Clipboards, cursors and the start row are kept for
the session; the song itself is in `SONG.BIN`.

## Songs: save and load

**shift + join** (song view or track list) opens the **songs page**: 16 slots, each holding a whole
song: the track list with its settings, the song grid, the chains, **all 14 NOTE ROLL and 14 DRUM
ROLL patterns** (as PLAY plays them, TE's own finger patterns included), both editors' swing and
**the synth sounds the song plays** (engine, its parameters, envelope and octave of every sound
slot a SYNTH track or one of its chain rows uses).

```
SONGS           BPM 120                  BAR 01.1      STOP
SLOT  TRACKS                             ROWS   PAT
01 *  SYN DRM AUD SYN                     12     9
02    DRM SYN                              4     3
03    --
SAVED IN SLOT 01
REC X2: SAVE  PLAY X2: LOAD  CUT X2: DELETE
```

| Control | Songs page |
|---|---|
| blue / white turn | the slot |
| rec, rec again | save the song in the slot (the footer asks first; an existing slot is replaced) |
| play, play again | load the slot: playback stops, the song is replaced, the song view opens at row 00. Patterns and sounds that differ from the ones now are asked about first (below) |
| cut, cut again | delete the slot |
| stop | stop / bar 1 as usual |
| join / orange press | back to the song view (tape: the track list) |
| speech | the help page |

**Loading asks about patterns and sounds.** The slot keeps the patterns and sounds as they were
when it was saved, so a song sounds as it did even if a pattern or a sound was edited since. Before
a load, PLAY compares the slot's 28 patterns and its sounds with the ones now; if any differ, it
lists them:

```
SONGS           BPM 120                  BAR 01.1      STOP
LOAD SLOT 01: 3 PATTERNS, 2 SOUNDS DIFFER
NOTE ROLL 03    IN THE SONG     FROM THE SLOT
NOTE ROLL 07    NOT IN THE SONG KEEP NOW
DRUM ROLL 04    IN THE SONG     FROM THE SLOT
SOUND 2         ACID 3          FROM THE SLOT
SOUND 6         MONO 3          KEPT: NOT HERE
1 KEPT AS NOW, THE REST AS SAVED
GREEN: CHANGE  WHITE PR: ALL  PLAY: LOAD  JOIN: NO
```

| Control | Load question |
|---|---|
| blue / white turn | the pattern or sound |
| green turn right / left, green press | from the slot / keep the one now / the other one |
| white press | all kept (if one is from the slot), else all from the slot |
| play | load: the song, the patterns as chosen, the swings |
| join / orange press | do not load |

Patterns the song plays (its SYNTH / DRUM chains) start as FROM THE SLOT, the others as KEEP NOW.
Sounds start as FROM THE SLOT. Nothing differs: loaded at once. A load with a kept pattern shows
the slot as changed (amber). Slots saved by PLAY 0.10.0 do not know which patterns the song plays:
every stored one counts.

**A sound comes back** into its sound slot (TE's synth sound 1-8, the one the track names) with its
engine's parameters, envelope and octave, saved as TE saves a sound edit. It stays as it is now
(shown, not selectable) when:

| Shown | Why |
|---|---|
| `KEPT: NOT HERE` | its SDK engine is not in this firmware (or cannot run in PLAY) |
| `KEPT: OTHER ENGINE` | the sound slot holds one of TE's engines now (FM, string, ...): PLAY does not switch TE's engine type. Pick any SDK engine on that slot and load again |
| `KEPT` | the stored sound is one of TE's engines: it plays in PLAY (v0.13), but a slot does not bring its parameters back yet (TE's per-engine ranges are not mapped) |

The track then plays whatever that slot holds now, as before. Every value written is clamped into
TE's ranges first (TE checks stored sounds at boot). A track on `CUR` stores the sound that was
current when the slot was saved. Slots of PLAY 0.10 / 0.11 have no sounds (the slots' own stay).

`*` marks the slot the song came from (or was last saved to): green while nothing changed since,
amber once the song or a pattern was edited. Loading asks with a warning when the song now is in no
slot or has changed since.

- Loaded patterns (the ones not kept) become NOTE ROLL's / DRUM ROLL's own (they write TE's copies and op1.db), so the
  finger sequencer shows them too; the editors' undo history starts anew.
- **Not in a slot:** the drum kits (the song only names the kit slots), TE's synth FX and LFO
  (PLAY does not apply them), the audio files (only their names: a slot loaded with another set
  shows `GONE`), the tempo (TE's tempo page), the master.
- Files: `/yaffs2/user/play_s/SONG01.BIN` .. `SONG16.BIN` (kept when a new set is imported). Empty
  patterns are not stored: a small song is ~8 KB, all 28 patterns used ~84 KB (a sound: 36 bytes). A file is read back
  once after it is written; a damaged one shows `DAMAGED`, a file from an editor version with other
  pattern sizes `OTHER VERSION` (neither can be loaded).
- The song you work on is still kept in `SONG.BIN` as before (a load writes the loaded song there).

### Sharing songs

Entering **disk mode** copies every slot to the disk's `play_s/` folder as `SONG01.PSL` ..
`SONG16.PSL`: one file holds the whole slot (song, patterns, sounds), so it is what you share.
To bring songs in, put `.PSL` files into `play_s/` (it is there even with no slots) while in disk
mode; leaving disk mode takes every whole one that no slot holds already into a **free**
slot: `SONG05.PSL` into slot 05 if that is free, any other name (or a taken number) into the first
free slot. A slot is never overwritten: to replace one, delete it on the songs page first. A file
that is damaged, cut short or from a PLAY version with other pattern sizes stays out, as do files
beyond 16 slots; TE's console line after disk mode says how many came in and how many were left.
Deleting a `.PSL` on the computer deletes nothing on the OP-1 (TE empties the folder anyway when
disk mode is left).

### Demo songs

Four songs come with PLAY (v0.15, `demos/`): own compositions on the genres' common ground, and
one public-domain piece.

| Slot | Song | Genre | Tempo | Tracks |
|------|------|-------|-------|--------|
| 13 | WAREHOUSE | techno, A minor | 128 | drums, a 303 line (ACID 3: accents, slides), a pad (STATION 1 UNIVERSE), a bell motif (DIGI BELL) |
| 14 | PIANO HOUSE | house, F major | 124 | drums with claps, shaker and congas, M1 PIANO stabs, HOUSE ORGAN bass, STRINGS |
| 15 | RADIO | pop, G major (I V vi IV) | 112 | drums, E.PIANO, SYNTH BASS, SAW LEAD verse and chorus melodies, BELL PAD |
| 16 | PRELUDE | classic: J. S. Bach, Prelude in C major BWV 846 | 76 | bars 1-19 and a closing chord on HARPSICHORD, STRINGS under it |

- **In the image:** the files ride along as `/yaffs2/content/play_demo_13.psl` .. `_16.psl`; at the first boot with
  them PLAY copies each into a free slot (its own number 13-16 if free, else the first free one,
  never over a song of yours) and writes `play_s/DEMOS.OK`, so this happens once: a demo you
  delete stays deleted, a new image does not bring it back.
- **Without the image:** copy `demos/SONG13.PSL` .. `SONG16.PSL` into the disk's `play_s/` folder in
  disk mode (Sharing songs, below).
- **Before playing:** load the **m1 drums** kit (drum preset browser, folder station 1) into
  **drum slot 8**: the drum tracks name that slot and its keys (kick 1, side stick 4, snare 5,
  clap 9, closed hat 10, open hat 12, shaker 14, crash 20, ...). The synth sounds go into **synth
  slots 5-8**: the load asks, as for any slot; a sound only comes back onto a slot that holds a
  drwave sound now (any SDK engine's preset; else it shows `KEPT` and the slot plays what it
  has). Set the tempo on TE's tempo page (a slot does not store it).
- `demos/gen_demos.c` writes the files with PLAY's own `py_slot_save` and reads them back with
  `py_slot_check` / `py_slot_load` (every pattern, sound and the song compared; the knobs inside
  drwave's ranges): `python3 features/play/demos/gen_demos.py`; `--preview <dir>` adds a rough WAV
  of each (plain waveforms, not PLAY's engines) to hear the notes on a computer.

The `.PSL` format (`py_slot.h`): a 40-byte header (`PSL2`, pattern sizes, masks of the stored
patterns and sounds, a summary for the slot list), the song (`PSN3`), the stored patterns, the
stored sounds, a CRC-32 over everything. Little-endian, no compression; the patterns are NOTE
ROLL's and DRUM ROLL's own structures, so a file only loads into a PLAY whose editors have the same
pattern sizes.

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
| black keys (held) | trigger effects on the cursor column's track; shift at the press: on every track (v0.17, below) |

**Trigger effects** (v0.17): in live mode the 10 black keys, held, change what the pattern tracks
play, low to high: **FILL** (now and then an extra note / hit of the pattern on a 1/16), **RANDOM**
(SYNTH: some notes an octave up / down or a fifth up; DRUM: every hit -12..+12 semitones), **ECHO**
(again 3 and 6 steps later, softer), **RETRIG** (the last 1/16 with a note, from the press on, over
and over), **RETRIG FAST** (half of it; the last of the two pressed plays), **OCT UP**, **OCT
DOWN**, **COIN** (an octave-up grace note a quarter step after each), **SOLO LEAD** (SYNTH: the
highest note of each step; DRUM: every lane but lane 1), **SOLO BASS** (SYNTH: the lowest note;
DRUM: lane 1 only). Plain: the cursor column's track; shift held at the press: every SYNTH / DRUM
track. They stack (RETRIG plays with the others); released, the track plays on as written (the
pattern's clock never stops). The footer shows what is held (`FX ECHO OCT UP > ALL`). The SYNTH
note effects are NOTE ROLL's own (`nr_px_notes`); a render does not hear them (cues neither).

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
  mode keeps them. The file pool holds 16 files in all, the set's first.
- **In disk mode** the renders show up in a folder **`play_r`** at the top of the disk, to copy
  to the computer. PLAY copies them in each time disk mode is entered (after TE has written
  synth/ and drum/, before the computer sees the disk), so entering takes longer the more you
  have rendered. The folder is a copy: deleting or changing files there changes nothing on the
  OP-1 (TE removes the folder again when disk mode is left; delete renders on the files page).
  A render that does not fit is left out: the disk keeps TE's own 2 MB, the flash 16 MB. Not
  while a render runs.

### The files page: renders listed and deleted

On the songs page, **shift + join** again opens the **files page**: every file of the render
folder (all of them, not only the 16 in the pool) with its length, stereo / mono, size in MB and
the AUDIO tracks of the song that play it.

```
FILES           BPM 120                  BAR 01.1      STOP
RENDER          LENGTH     MB     PLAYED BY
REND01.AIF      0:32 ST    5.6    3
REND02.AIF      1:04 MO    5.6    -
2 FILES, 11.2 MB
CUT X2: DELETE  SHIFT+JOIN: SONGS  JOIN: BACK
```

| Control | Files page |
|---|---|
| blue / white turn | the file |
| play | listen to it: the file alone from its start, PLAY paused (the row shows the time in green); play again or stop: stop (v0.17) |
| cut, cut again | delete it (the footer asks first, `PLAYED!` if a track uses it). Playback stops; a track that played it shows `GONE` |
| shift + join | back to the songs page |
| join / orange press | the song view |
| speech | the help page |

A deleted name is free again: the next render takes the first free `RENDnn`.
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
  take turns (instances 1, 3 and 5 on one block, 2 and 4 on the next), so five ECO tracks cost at
  most three renders in any block. ACID 3 knows ECO and keeps its cutoffs, envelopes and slide (its own
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

Limits: up to 8 SYNTH tracks, at most 6 of them sounding at once (v0.16; v0.9: 5, v0.8: 3): the 6 engine
instances are a pool. The 4th to 6th are taken only while PLAY's CPU allows it: its peak block
(the last of each block parity, at least 41k cycles per sounding instance before anything is
measured) plus one more instance's share must stay under 435k cycles (~75 % of a block since v0.17, before 250k; TE's own
25-30 % on top). Heavy engines without ECO then stop at 3 (`BUSY`), light ones and ECO tracks
reach 6. A track takes one for its first note and gives it back once its voices
are silent, so SYNTH tracks that take turns in the song (rest cells, later columns, live cues)
can use 8 different sounds. A note that finds no free instance takes one whose track only has
a release tail left (the tail is cut), else it is dropped and the track shows `BUSY` for ~1 s
(red `SYN` in the song view). Muted tracks take none. 2 DRUM tracks (CPU), 8 tracks in all. TE's own engines run
as PLAY's instances too (v0.13, "TE's engines" below). The SDK engines keep their state
per voice slot (36: TE's 6 + 5 instances x 6, ~7 KB of RAM per instance over all engines) and per instance (MONO 3's voice order, ACID 3's mono
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
   (A `set.txt` with `bpm ...` is no longer read since v0.14: the tempo is TE's tempo page.)
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
  stored knobs (an SDK engine through enginehost, TE's own engines through their voice objects,
  below); TE's own envelope code (Adsr.cpp, on PLAY's objects) shapes it with the slot's
  envelope; the SYNTH tracks are summed with their gains at 88.2 kHz and PLAY's copy of TE's
  63-tap halfband takes the sum to 44.1 kHz once (ECO tracks join after it). TE's synth FX / LFO are not
  applied (TE has one instance of each). Notes start on the block after their step (1.45 ms).
- **TE's engines** (v0.13, `py_tev.c`, static RE 2026-11-20): TE builds the 6 voices of the current
  sound's engine with its factory (`TE_ENGINE_NEW`, producer_factory.cpp) as small C++ objects at
  `0xffb00000 + vi x size` in the L1 scratchpad, engine-wide state right after them (fm's operator
  setup, pulse's tables); all of it below `0x300` (cluster, the largest, 0x287 bytes). Their code
  reads nothing per voice outside the object but buffers TE fills before each voice (pitch
  increments `0xff801950`; dsynth: the envelope `0xff801d50`), read-only tables, shared scratch
  and the dither generator; the sampler's voice holds its slot's sample at +4. So each PLAY
  instance keeps its own copy of those 0x300 bytes: for a track's block PLAY saves TE's, copies
  the instance's in, calls the voices through their vtables (note_on `+0x1c`, render `+0x14`,
  release `+0x2c`) as TE's voice loop does, copies them back and puts TE's back (audio interrupt,
  after TE rendered its own voices; ~5k cycles of copying). Fresh voices come from one prototype
  per engine a slot holds, built in the main loop with interrupts masked (TE's factory into the
  scratchpad, copied out, TE's voices restored). As with the SDK engines, TE's synth FX and LFO
  and the sound's play mode (unison, glide) are not applied.
- **Drum voices:** TE's kits stay in sample memory; PLAY reads them in place (pitch, start, end,
  playmode, reverse, volume of each key), with DRUM ROLL's VEL x lane level, TONE and LEN gate.
- **Mix:** an "after" hook on TE's metronome mix adds PLAY's sum into TE's master bus, where the
  tape and the instruments are summed too, so TE's master EQ, master FX, the mixer's drive and
  the volume act on PLAY like on the tape. PLAY scales its sum by 0.38 there (TE's drive takes
  the bus up ~x2.64), then per track gain, mute / solo, master, a soft knee, saturation.
- **Import:** a hook before TE's disk cleanup copies `play/` from the disk image
  (`/yaffs2/user/tape_c.raw`, read with the SDK's `fat` library) into the folder PLAY is not
  playing from; the main loop then switches over. The same hook takes new `.PSL` files from
  `play_s/` into free slots.
- **Sounds:** a slot stores TE's engine type, octave, knobs (`TE_SYNTH_KNOBS`) and envelope
  (`0x01b60ef0`) of each sound the song uses; a load writes them back and calls TE's own patch
  setters (octave `0x019ff36c`, envelope `0x01a00c98`, knobs `0x01a00d14`: the JSON and the save,
  as TE's screens do), only onto a drwave sound and only for an SDK engine PLAY runs.

Writes: the disk image's `play_r/` and `play_s/` folders (through TE's FatFs, on the way into disk mode),
`/yaffs2/user/play_r` (rendered files), `/yaffs2/user/play_s` (song slots), `/yaffs2/user/play_a` and `/yaffs2/user/play_b` (the copied set, which take turns;
`ACTIVE` marks the current one) and `SONG.BIN` in the current one; a slot load writes the synth
sounds it takes back (TE's patch of that sound slot). PLAY never writes TE's tape or the disk image
otherwise.

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
- `test_py_slot.c` (host): a song slot through a memory file: the round trip (song, swings, stored
  and empty patterns), size and masks, the header summary, every damaged byte refused with nothing
  changed, short / long files, other pattern sizes, no file, a pattern read inconsistently (retried,
  BUSY), a failing write; the patterns the song plays, which patterns differ from the ones now, a
  load keeping some, a slot from 0.10.0; the sounds the song plays (the track's, a chain row's,
  CUR's) stored, compared, put back, kept or refused; a PSL1 file (0.10 / 0.11) loaded; where an
  imported file goes (its own number if free, else the first free slot, never a taken one).
- `test_py_export.c` (host): renders into disk mode's volume: copied byte for byte (several
  chunks, an empty file), a file without room on the volume or the flash skipped (the next still
  tried), a read error / failing close: that file removed, a short write: removed and stopped.
- `test_py_drum.c` (host): pitch maths, TE's position encoding, one-shot vs gate, reverse, loop
  while held, an octave up, the start delay, the level law, voice stealing, clamping.
- `test_py_synth.c` (host): pitch increments, TE's halfband (DC, 1 kHz passes, 30 kHz gone), the
  voices against stand-ins for an engine and TE's envelope (flags, instance, release after the
  hold, -60 dB release end, release tails (`py_syn_tail`), voice cap and stealing, new notes first, gain and peak, ECO (parity,
  HALF flag, BOOST only with ECO), no engine; the 32-bit decimator bit for bit against the 64-bit sum;
  the envelope rendered before the engine and handed to it, the engine's note-off once (after
  the note_on of a note shorter than a block), a restart on a new engine).
- `sim/load_play.py` (simulator, a measurement, not in test_all: about 10 minutes): three SYNTH tracks on
  ACID 3, a legato note every 1/16, ECO off and on: cycles per block (peak, average), renders per
  track. 2026-11-15: ECO off peak 201k (314k before new-notes-first), ECO on peak 129k (206k
  before the parity split), averages 181k / 92k.
- `sim/run_playtest.py` (simulator, composed with WARP): import, folder switch, tracks added on
  the ADD row, exact mixed samples of once / looped files, a gone file on its own track, keys,
  a DRUM track sample by sample, a SYNTH track on a test engine as instance 1 with TE's envelope
  stand-ins, the same track on a stand-in for one of TE's engines (prototype built once in the
  main loop, note_on / render / release in order on the voices in the scratchpad, increments and
  envelope in TE's buffers, the same level, TE's own voices in the scratchpad untouched), SONG.BIN saved and read back, removing a track, no yaffs buffer at an odd address,
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
  two show BUSY); song slots (the songs page, save, the song, patterns and sounds changed, load:
  the question, cancelled, then one pattern kept and the rest back, a sound back through TE's
  setter stand-ins, a sound on an FM slot kept, delete); the files page (a render deleted); renders
  into disk mode (TE's FatFs stand-ins: every render copied byte for byte, every file closed, TE's
  unmount still called; no room: nothing left behind); a slot into disk mode as SONG01.PSL and
  shared files back (a FAT image with play_s/: the slot's own file skipped, taken back into its
  free slot, into the next free slot when its own is taken, a damaged one left).

License: MIT.
