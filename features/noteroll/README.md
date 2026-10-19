# NOTE ROLL — the synth finger sequencer's step editor — spec

Status: **v0.9** (2026-10-17): punch-ins on both held patterns, repeats loop the last step with a note. **v0.8** (2026-10-17): punch-in banks A / B / C (green press), 16 new arcade / time effects, shift + black always TE's step record. **v0.7** (2026-10-17): undo / redo (shift + BACK / FORWARD), also of TE's own finger screen edits; STEP REC octave on shift + green turn. **v0.6** (2026-10-17): the roll's rows at least 4 px (a 1 px gap always), the cursor frame one size on empty and full cells. **v0.5** (2026-10-17): 14 punch-in effects, reordered (FILL FAST, GLITCH FAST left out).
**v0.4** (2026-10-17): the pitch cursor (shift + blue turn: any row, blue press: a note on /
off) and STEP REC (shift + white press: play the keyboard into the cursor step, the arrows move). **v0.3** (2026-10-17): punch-in effects on the black keys of TE's synth finger screen (user:
"also add the punch in effect to the finger in synth mode"; then the PO-20 arcade's set, shift +
black for 6 more, a window while held). **v0.2** (2026-10-17): zoom 3 and 3.5 bars, the shift key drawn as its arrow. **v0.1**
(2026-10-17, user: "in a similar fashion to our finger + drum roll, add a similar
editor to the finger sequencer for synth tracks"; scope picked: full, like DRUM ROLL). DRUM ROLL's
twin for the synth: opens over TE's **synth finger sequencer** and shows / edits the selected finger
pattern as a piano roll; a pattern edited there plays with chords and every NOTE ROLL parameter.
`features/noteroll`, in `op1plus`; host-tested (`test_nr_core.c`, `test_nr_fs.c`, `test_nr_store.c`),
sim-verified (`sim/run_nrtest.py`); awaiting device test. Screens from the simulator:
`build/demo/noteroll_note.png`, `noteroll_chord.png`, `noteroll_finger.png`.

The name: "SYNTH ROLL" is requantize's screen, so this one is NOTE ROLL.

## Open it on the OP-1

Synth playmode, press the **sequencer key** with the **finger** sequencer picked (the screen shows
**⇧ BLUE: EDITOR** (the shift arrow) top left), then **shift + blue press**: NOTE ROLL shows the finger pattern
that is selected. Shift + blue press again closes it (shift + green press too). On the sequencer
screen shift + blue press is never requantize's (requantize 4.0 leaves it there).

## TE's synth finger sequencer (RE 2026-10-17, R246)

As DRUM ROLL's README describes for the drum, with the synth's own data:

- **Data**: OkData synth instance `0x01dd98bc` (same layout): `+0x08` artist pattern[2], `+0x10`
  notes[14][32][2] (int32, -1 empty), `+0xe10` length[14]. A note is stored as **53 + key + 12 x
  the finger screen's octave** (`0x01ddc59c`); the player adds **12 x the synth sound's octave**
  (`0x01dfe4e0[TE_SYNTH_SLOT]`, now `TE_SYNTH_OCTAVE`) when it plays it.
- **Player**: one for both playmodes (`0x01ddc5a8`; selected pattern `+0x1c`, steps `+0x0c` shared;
  artists per playmode in OkData `+0x08`). The synth branch of `0x019edbb8` runs only when
  `TE_PLAYMODE == 0`; its `get_note` call is at **`0x019edc4e`**; a note goes out through VoiceCtl
  note-on (`0x01a85014` builds it: velocity 127, MIDI out 1, extra 0). **Gate: one step**: at every
  step event TE first releases all its notes (`0x019ed564`), then plays the next step; a note still
  sounding is released before it is played again.
- Sequencer type: `0x01ab3144` (`TE_SYNTH_SEQ_TYPE`), finger = 3; the sequencer screen is
  `TE_EDIT_MODE` 7 for both playmodes.

## How it works

### Patterns

14 patterns = the 14 finger patterns. A NOTE ROLL pattern: length 1..64 steps of 1/16; each step up
to **6 notes** (a chord; sorted by pitch) with per note **VEL** (1..100) and **LEN** (1/8 step ..
16 steps, or **TIE** = until the same note plays again, at most the pattern), per step **NUDGE**
(-50..+50 % of a step), **PROB** (0..100), **RATCH** (1..4). Swing (global).

- **TE's** pattern (not yet edited here): TE keeps and plays it; NOTE ROLL shows an **import**
  (every note as stored, one step long = TE's gate, VEL 100, TE's length), redone when the pattern
  is shown and when TE changes it.
- **NOTE ROLL's** (from its first edit here): NOTE ROLL plays it; TE's copy becomes a **summary**
  for TE's compressed view and stock firmware: per step 0..31 the lowest and the highest note (one
  note: hand 0), length = NOTE ROLL's (TE keeps at most 32).
- TE's edits on a NOTE ROLL pattern still work (main thread compares TE's copy with what it last
  saw there, every main-loop pass): a step-recorded note joins its step; TE's delete / clear of a
  step (both hands) empties the step with all its notes; TE's length becomes the pattern's.
- Page 5 CLEAR empties the pattern on both sides and gives it back to TE.

### Playback

- TE's patterns: TE plays them. NOTE ROLL's: the `get_note` call site returns -1 for them (TE
  plays nothing) and records the **step events**; NOTE ROLL plays the pattern from the seq tick on
  its own clock, started by the first step event, re-synced every second one, stopped when no
  artist holds it or after 3 steps without events (the clock follower of DRUM ROLL).
- Notes as TE's player sends them: VoiceCtl note-on / note-off, the stored note + 12 x the synth
  sound's octave, velocity VEL x 127 / 100, MIDI out on; a sounding copy of the same note is released
  first; only in synth playmode and not while switching; note-offs wait for synth playmode (never
  dropped). Two artists = two patterns at once.

## Controls

DRUM ROLL's handling: knob turn = the value under that knob, **green press = next page**, **blue
press = the cursor note on / off** (an empty slot: a note at the last pitch), **white press = hear
the step**.

| page | blue | green | white | orange |
|---|---|---|---|---|
| P1 NOTE | STEP | PITCH (empty slot: adds a note) | VEL | PROB |
| P2 SHAPE | STEP | LEN | NUDGE | RATCH |
| P3 CHORD | STEP | VOICE (the step's notes, + a free slot) | CHORD shape on the lowest note: NOTE MAJ MIN SUS2 SUS4 7 MAJ7 MIN7 DIM AUG 5 OCT | INVERT (right: lowest up an octave, left: highest down) |
| P4 PAT | LENGTH 1..64 (TE: max 32) | SWING | ZOOM 1 / 2 / 3 / 3.5 / 4 bars | TRANSPOSE (all notes) |
| P5 CLEAR | press: clear? | (press: next page) | press: clear? | press: clear? |

| with shift | |
|---|---|
| blue turn | VOICE (the step's notes) |
| green turn | finger pattern 1..14 |
| blue / green press | close |
| orange press | clear the step (again: undo) |

- The view follows TE's selected pattern (a held pattern key: at once); while a pattern plays the
  view shows the playhead's bars (2 s after a knob event: the cursor's).
- Screen: DRUM ROLL's / SYNTH ROLL's look: header (pattern, NR / TE, PLAY / STOP, BPM, swing, page),
  piano roll (pitch column, black keys darker, step / beat / bar lines, the loop line in orange,
  notes as long as they are held, velocity = brightness, repeats past the length dim, ratchets split,
  PROB < 100 dotted, the cursor step's column lit, the cursor note white, the playhead), four knob
  columns, the step's notes + chord name, the hint line.

## Punch-in effects (v0.8: three banks)

As DRUM ROLL's (its README: keys, banks by green press on the finger screen with the editor closed,
shift + black = TE's step record, the window): on TE's synth finger screen, on NOTE ROLL's playing
patterns. All note effects (`nr_punch.c`: one note in, up to 16 timed notes out).

| key | A (the PO-20 for the synth) | B (arcade) | C (time + solos) |
|---|---|---|---|
| F# | FILL | CHIP ARP UP: the step's notes in turn every 1/32 while they last (one note: with its octave) | HALF TIME |
| G# | RETRIG | CHIP ARP DN | DOUBLE TIME |
| A# | RETRIG FAST | POWER UP: a 1/32 roll, a semitone up each | TRIPLET |
| C# | GLITCH | POWER DOWN | LOOP BEAT |
| D# | RISING ARP | COIN: an octave-up grace note a quarter step after each | BUILD |
| F# | OCT UP | TRILL: the note and a whole tone up in turn every 1/32 | SOLO CHORDS |
| G# | OCT DOWN | ECHO: the note again 3 and 6 steps later, softer | SOLO LEAD |
| A# | STACCATO | REVERSE: the bar's steps backwards | SOLO BASS |
| C# | BLINDS | OCT ARP: the octave half a step after each note | SOLO MELODY |
| D# | RANDOM | LASER: a 1/64 roll, 2 semitones down each | FADE: down to nothing over a bar |

- v0.9: as DRUM ROLL 2.1: both held patterns (a held TE pattern is taken over while any effect is
  held), the repeat effects loop the last step with a note, not an empty one.

## Step record and the pitch cursor (v0.4)

User (2026-10-17): step record as in DRUM ROLL, and "I cannot add new notes easily: shift + blue turn
only moves among the current notes, I cannot pick another note on the same step".

- **The cursor is a step and a pitch row** (a piano roll's): **shift + blue turn** moves the row up
  / down by a semitone over the whole range; **blue press** puts a note there / takes it away; so a
  second, third ... note on a step = shift + blue turn to its row, blue press. PITCH (P1) moves the
  note under the cursor (the cursor goes along); VOICE (P3) jumps to the step's next note up / down;
  CHORD / INVERT put the cursor on the root.
- **STEP REC** (shift + white press while open; header: a red REC): the **24 keyboard keys** play
  notes into the cursor step (the first key after arriving replaces its notes, more keys add to it;
  stored as TE's finger screen stores them: 53 + key + 12 x the octave), heard as played;
  **FORWARD / BACK** go a step on / back (a step passed by stays), **shift + FORWARD / BACK** the
  octave (-4 .. 4, starting at TE's finger screen octave). The keys and the arrows are NOTE ROLL's
  meanwhile; closing the editor ends it.

## Undo / redo (v0.7)

As DRUM ROLL's (its README): **shift + BACK = undo, shift + FORWARD = redo**, in NOTE ROLL and on
TE's synth finger screen with it closed, 16 steps of one pattern's state (NOTE ROLL's pattern, TE's
copy, whose it is). TE's own finger screen edits are found by the main thread's look at TE's copy
and pushed as steps there; undoing them on a TE pattern writes TE's notes back from the main thread
(the one thread that writes TE's copy). STEP REC: shift + BACK / FORWARD are undo / redo now, the
octave moved to **shift + green turn**.

## Saving

Table `sdk_noteroll (id INTEGER PRIMARY KEY, data TEXT)` in TE's `user/op1.db`: rows 0-13 patterns,
row 14 settings (shown pattern, swing, zoom, which patterns are NOTE ROLL's); base64 of
`'N' 'R' version id` + raw data, version 1; written ~3 s after the last edit or at the next pass after
closing; loaded values clamped (as DRUM ROLL). TE's summary notes go into `okseq` through TE's own
mark-dirty / saver: values TE stores itself (MIDI notes, -1) and a length 1..32.

## Out of scope (v0.1)

Live recording into NOTE ROLL patterns, TE's play modes / step order for NOTE ROLL patterns, per-note
pitch bend / glide, the synth octave shown in the note names (the roll shows stored notes, as TE's
finger screen keys them), MIDI out of anything but what VoiceCtl echoes.

## Code

- `nr_core.h/.c` (`test_nr_core.c`): data model, chords, inversions, the scheduler.
- `nr_fs.h/.c` (`test_nr_fs.c`): import, summary, TE's edits, the clock follower.
- `nr_store.h/.c` (`test_nr_store.c`): base64 rows, load with range checks.
- `noteroll.c`: get_note hook, seq tick, keys / knobs, screen, TE's copy (main thread), op1.db.
- `sim/run_nrtest.py`: the composed op1plus image against an emulated VoiceCtl, synth finger
  sequencer and op1.db.

## Device test

Open over the synth finger screen with a TE pattern (import right?); edit: plays in time with TE's
step grid, chords, lengths, ties, the synth sound's octave; two patterns at once; TE step record and
delete into a NOTE ROLL pattern; TE's length; CLEAR; drum playmode switch while playing; power cycle
(patterns + ownership back, TE's mini view shows the summary), boot clean.
