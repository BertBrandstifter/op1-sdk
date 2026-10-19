# DRUM ROLL — the finger sequencer's step editor — spec

Status: **v2.1** (2026-10-17): punch-ins act on both held patterns (TE's taken over while held), repeats loop the last step with a hit. **v2.0** (2026-10-17): punch-in banks A / B / C (green press on the finger screen), 18 new arcade / time effects, shift + black always TE's step record. **v1.9** (2026-10-17): undo / redo (shift + BACK / FORWARD), also of TE's own finger screen edits. **v1.8** (2026-10-17): 12 punch-in effects, reordered (FILL FAST, GLITCH FAST, LP FAST, HP
FILTER left out). **v1.7** (2026-10-17): STEP REC (shift + white press: play the keyboard into the cursor step,
the arrows move). **v1.6** (2026-10-17): the punch-in effects are the PO-20 arcade's 16 (10 on the black keys,
6 on shift + black), shown in a window while held. **v1.5** (2026-10-17): zoom 3.5 bars (user: "this way 1/32 patterns fit"; op1.db index 4), the
shift key drawn as its arrow (`gfx_shift`) on the finger screen note and the hint line; punch-in effects on the black keys of TE's drum finger screen (user:
"put some fun punch in effects on the black keys in the drum mode when using the finger sequencer";
picked: the mix without a buffer, held = on, stacking). **v1.4** (2026-10-17): zoom 3 bars added (1 / 2 / 3 / 4; op1.db keeps 0 1 2 = 1 2 4 bars,
3 = 3 bars). **v1.3** (2026-10-17): shift + blue press opens and closes it (user: "to enter and leave
the drum roll lets use the shift + blue knob"); no "DRUM ROLL n" badge any more; shift + blue press in drum playmode
no longer starts a requantize take (requantize 4.0). **v1.2** (2026-10-17, user: "really love it!"; the playhead showed only sometimes): the view
follows a held pattern key at once, TE's own patterns get a playhead, the view follows the playhead
while playing. **v1.1** (2026-10-17, user after the v1.0 device test: "nice!"; when the finger sequencer's
pattern length changes, all tracks should follow, and DRUM ROLL should set the length of all tracks
together; then: open it with a plain blue press, a small "editor" note on the finger screen): TE's
length and page 6 PAT's LENGTH set every lane's length; blue press opens (shift + blue closes), note
"BLUE: EDITOR" on TE's finger screen. **v1.0** (2026-10-17, user: "integrate the drum roll into the finger sequencer … remove the drum
roll from the normal drums page"): DRUM ROLL opens over TE's **finger sequencer** (sequencer key, drum
playmode) and shows / edits the selected finger pattern as an 8-lane step grid; a pattern edited there
plays with 8 tracks and every DRUM ROLL step parameter. The drum screen gesture, the "drum roll" row in
the drum engine list and DRUM ROLL's own play / stop + tape clock are gone. `features/drumroll`, in
`op1plus`; host-tested (`test_dr_core.c`, `test_dr_dsp.c`, `test_dr_store.c`, `test_dr_fs.c`),
sim-verified (`sim/run_drtest.py`); v1.0 / v1.1 device-tested ("nice!", "really love it!"), v1.2 awaiting device test. Screens from the simulator:
`build/demo/drumroll_step.png`, `drumroll_sound.png`, `drumroll_clear.png`.

History: v0.9 (2026-10-15) patterns kept in op1.db, page 6 CLEAR; v0.8 "drum roll" in the drum engine
list; v0.7 tape-screen play / stop; v0.6 back-to-back + ratchet fix; v0.3 device-tested ("nice"); v0.1
device-tested ("love it") — all as a stand-alone sequencer on the drum screen.

## Open it on the OP-1

Part of the default `op1plus` image. Drum playmode, press the **sequencer key** with the **finger**
sequencer picked (the screen shows **⇧ BLUE: EDITOR** (the shift arrow) top left, at the size of the engines'
"SHIFT+BLUE: PRESETS"), then **shift + blue press**: DRUM ROLL shows the finger pattern that is
selected. **Shift + blue press** again closes it (shift + green press too; TE's finger screen with its compressed view
is back). Shift + blue press on the finger screen with DRUM ROLL closed stays requantize's.

## Idea

TE's finger sequencer keeps 14 patterns on the keyboard's pattern keys, but shows a pattern only as a
tiny strip and edits it one step at a time with two sounds per step. DRUM ROLL becomes its editor:
a channel-rack grid (FL Studio's step sequencer in a TE look: black screen, thin lines, the four knob
colours) where a finger pattern is **8 lanes**, each lane one drum sample with its own length and rate,
every step with **VEL, TONE, LEN, NUDGE, PROB, RATCH**, every sample with a filter. The finger keys
still start and stop the patterns, so live play stays TE's; DRUM ROLL makes building and tweaking a
pattern easy.

The screen draws time to scale (bars and beats are lines, a step is as wide as its duration), short
lanes show a loop line and dim repeats, every lane has its own playhead while the pattern plays.

## TE's finger sequencer (RE 2026-10-17, R246)

Internally "ok" (`seq/ok_data.cpp`, `ok_player.cpp`, `gui/screens/ok_screen.cpp`, table `okseq`).

- **Data** (`OkData`, one per playmode; getter `0x019ed238` picks drum when `TE_PLAYMODE != 0`; loaded
  from `okseq` on first use, flag `B[0x01ddb564]`). Drum instance `0x01dda710` (synth `0x01dd98bc`):
  `+0` dirty bytes, `+0x08` artist pattern[2] (-1 none, 0..13: the patterns the two "artists" = held
  pattern keys play), `+0x10` **notes[14][32][2]** (int32: MIDI note, drum = 53 + key (+24 per octave
  offset), -1 = empty), `+0xe10` length[14] (1..32), `+0xe48` swing (0..100, 50 straight), `+0xe4c` play
  mode (0..3).
- `set_note(this, pat, step, hand, note)` `0x019ecc5c` writes a note and calls the mark-dirty
  `0x0193ef3c(this)` (TE's saver then writes `okseq`). Its only callers (the finger screen's
  controller): `0x019ec534` delete the cursor step (both hands -1), `0x019ec802` / `0x019ec840` step
  record into hand 0 / hand 1, `0x019ecade` clear the pattern. `get_note(this, pat, step, hand)`
  `0x019eccf0`.
- **Player** `0x01ddc5a8` (built by `0x019ed460`, flag `[0x01ddc5a4]`): `+0x1c` **selected pattern**
  (the one last started; TE's finger screen edits it), `+0x0c` step[2] per artist. Tick `0x019ee0a0`
  at 96 PPQN: a step is 24 ticks (1/16), swing moves the first step of each pair (pairs are 48).
  `0x019edbb8` plays one step: for every artist playing a pattern, hands 0 and 1 via `get_note`; the
  **drum** branch's call is at **`0x019ede2a`** (synth: `0x019edc4e`), then VoiceCtl note-on with
  velocity 127.
- **Sequencer screen** = `TE_EDIT_MODE` 7 (sequencer key 21, `0x019ac724`). Active drum sequencer type
  `*(int32 *)0x01ab3148` (synth `0x01ab3144`), finger = 3 (`TE_SEQ_FINGER`).

## How it works

### Patterns

DRUM ROLL has **14 patterns = the 14 finger patterns** (shown 1..14). Each is a DRUM ROLL pattern
(8 lanes x up to 64 steps, as before). A finger pattern is in one of two states:

- **TE's** (not yet edited in DRUM ROLL): TE keeps and plays it. DRUM ROLL shows an **import**: one
  lane per drum key used (in order of first use; more than 8 keys: the first 8), the remaining lanes
  on the lowest unused keys; length = TE's length, rate 1/16; a note = step on with VEL 100, TONE =
  12 x the note's octave offset. The import is redone whenever the pattern is shown, and TE's own
  edits (step record, delete, clear) update it as they happen.
- **DRUM ROLL's** (from its first edit in DRUM ROLL): DRUM ROLL holds the truth and plays it with
  8 tracks and all step parameters. TE's copy of the pattern becomes a **summary** for TE's compressed
  view (and for stock firmware): for each 1/16 step 0..31 the first two hits of that grid position, in
  lane order, as 53 + key. TE's length is left alone. TE's own edits still work on such a pattern: a
  recorded note at step s turns on step s of the first 1/16 lane on that key (or of the first empty
  lane, which gets that key; none free: ignored), a deleted / cleared note turns that step off.

DRUM ROLL's page 6 PAT clears the pattern on both sides and gives it back to TE (empty).

### Pattern length

A pattern has one length in 1/16 steps (1..64): every lane plays that time span, in its own rate's
steps (1 bar = 4 x 1/4, 8 x 1/8, 12 x 1/8T, 16 x 1/16, 24 x 1/16T, 32 x 1/32; rounded, 1..64). Shown
and set on page 6 PAT (blue turn): all lanes, then TE's length (TE keeps at most 32; a longer DRUM
ROLL pattern stays as it is). The pattern's length is its longest 1/16 or 1/32 lane (those hold any
length exactly; without such a lane: its longest lane). Page 4 LANE's LENGTH still sets one lane
(polymeter) until the next pattern length change.

- TE's length changes (finger screen) on a DRUM ROLL pattern: all its lanes follow at the next
  main-loop pass (polled; an edit in DRUM ROLL sets the lanes before TE's length, a length that
  already matches is left alone, so the two never fight). New steps are empty; steps past a shorter
  length are kept (lengthen again: they are back), as TE keeps its notes.
- On a TE pattern on screen: imported again with the new length.

### Playback

- TE's patterns: untouched, TE plays them.
- DRUM ROLL's patterns: the `get_note` call site of TE's drum player (`0x019ede2a`) returns -1 for
  them (TE plays nothing), and records **step events** (hand 0 = one event per artist step). DRUM
  ROLL plays the pattern from the seq tick (after TE's player in the same block):
  - starts on its **first step event** (clock 0; its step-0 hits go out in that very block);
  - runs on its own sample clock at TE's tempo, **re-synced on every second step event** (the swing
    pair grid): behind by up to 2 blocks it catches up, ahead it holds, further off it jumps;
  - stops when no artist holds it any more, or after 3 steps without a step event;
  - two artists = two patterns at the same time; every lane loops on its own length from the
    pattern's start (polymeter), swing = DRUM ROLL's SWING;
  - TE's play mode, step order and swing do not apply to the notes of a DRUM ROLL pattern (TE's play
    mode still decides when a pattern starts and stops).
- Notes as before: VoiceCtl note-on/off with 53 + key, TONE as the note-on's extra, only in drum
  playmode and not while switching (TE asserts otherwise), every note-on gets its note-off, a held
  copy of the same drum is released first (TE ignores a held note).
- **Voice post-process** unchanged: drumroll owns the drum sampler voice's render and note_on slots
  (`0x0190fad8`, `0x0190fae0`): per-sample filter, the step's velocity x lane level, the length gate;
  hand-played keys and TE's own finger notes get the sample's filter and level.
- Recording: the finger sequencer runs while the tape records; DRUM ROLL's notes go to tape like TE's.

## Punch-in effects (v2.0: three banks)

v2.0 (user, 2026-10-17: "to avoid accidental edits don't use shift + black keys for more punch-in
effects any more; cycle through banks A / B / C with the green knob press in the finger sequencer
… and add some more cool, funky arcade inspired effects"). On TE's drum finger screen the **10
black keys** (ids 13 14 15 32 22 23 24 25 26 27, F# G# A# C# D# F# G# A# C# D# low to high) are
punch-in effects while held (a plain black press does nothing in TE there; shift + black is TE's
step record again, always). **Green press** on the finger screen with DRUM ROLL closed picks the
bank, A -> B -> C (a box shows FX BANK x; the finger screen note shows FX x); with the editor open
green press stays "next page", the bank picked plays on.

| key | A (the PO-20) | B (arcade) | C (time + mix) |
|---|---|---|---|
| F# | FILL | 8-BIT: 5 bits, 11 kHz (every drum sound) | HALF TIME |
| G# | RETRIG: the step over and over | LASER: a 1/64 roll, 2 semitones down each ("pew") | DOUBLE TIME |
| A# | RETRIG FAST: half a step | FALL: a 1/32 roll, a semitone down each | TRIPLET: 1/16T retrigger |
| C# | GLITCH: 1/16 pieces from random places of the bar | COIN: an octave-up grace hit a quarter step after each hit | LOOP BEAT: the beat over and over |
| D# | RISE: a 1/32 roll, a semitone up each | RND PITCH: every hit -12..+12 semitones | BUILD: a roll 1/8 -> 1/16 -> 1/32 -> 1/64, beat by beat |
| F# | LP SWEEP (every drum sound) | OCT UP: every hit an octave up | SOLO 1-4 |
| G# | HP SWEEP (every drum sound) | OCT DOWN | SOLO 5-8 |
| A# | BLINDS: 1/16 gate (every drum sound) | ECHO: two softer repeats 3 and 6 steps later | NO KICK: all but lane 1 |
| C# | SOLO 1 | REVERSE: the bar's steps backwards | FADE: down to nothing over a bar |
| D# | SOLO 1+2 | SHUFFLE: heavy swing (66) | CUT: silence |

- Time effects (RETRIG, RETRIG FAST, GLITCH, RISE, LASER, FALL, TRIPLET, LOOP BEAT, BUILD, HALF /
  DOUBLE TIME, REVERSE): the last one pressed plays, its place captured at the nearest 1/16 of the
  first; the others stack. The pattern's clock never stops: released, it goes on.
- They act on DRUM ROLL's patterns (TE plays its own); the sound effects (LP / HP SWEEP, BLINDS,
  8-BIT) on every drum voice (`dr_punch.c`, after TE's render and DRUM ROLL's own post-process).
- v2.1 (user: "the finger sequencer can play two patterns at the same time, the punch-in effects
  apply only to one … retrigger needs perfect timing; hit on an empty note, use the last note"):
  **both held patterns**: a held TE pattern (not yet DRUM ROLL's) is **taken over** while an effect
  on the notes is held: TE's notes read at the press (as the import), TE's get_note gives -1 for it
  meanwhile and DRUM ROLL plays it on its own clock; released, TE plays it again (it stays TE's,
  nothing saved). The repeat effects (RETRIG, RETRIG FAST, RISE, FALL, LASER, TRIPLET, BUILD) loop
  the **last step with a hit** from the nearest 1/16 back (up to a bar), not an empty one.
- Window while held (as CHORD+'s panel): bank + key, the last pressed effect's pictogram and name,
  the other held ones, "GREEN = NEXT BANK".

## Step record (v1.7)

User (2026-10-17): "add a step record mode … select the start point, play the keys on the keyboard
one / multiple per step, the left and right arrows advance or go back and re-record a step".
**Shift + white press** (open) turns STEP REC on / off (header: a red REC; hint line: how). While on:

- the **24 keyboard keys** play drums into the **cursor step** (the step index of every lane; pick
  the start with the STEP knob first): the first key after arriving at a step clears it in every
  lane, more keys add to it; a key's drum goes to the first lane on that drum, else to the first
  empty lane (which gets the drum), else nowhere; every key is heard as it is played;
- **FORWARD / BACK** (the arrows) go a step on / back (wrapping at the selected lane's length); a
  step passed by stays as it is, so go back and play to re-record one;
- the keys and the arrows are DRUM ROLL's meanwhile (pattern keys, TE's step record and the
  black-key punch-ins wait); closing the editor ends STEP REC.

## Undo / redo (v1.9)

User (2026-10-17): "an undo / redo in all the editors, shift + left (undo) / shift + right (redo),
several steps … also undo additions to a pattern I accidentally do in the finger sequencer".
**Shift + BACK = undo, shift + FORWARD = redo**, in DRUM ROLL and on TE's drum finger screen with
it closed; a box shows UNDO / REDO and how many steps are left (or NOTHING TO UNDO).

- A step = the state of one pattern before a change: DRUM ROLL's pattern, **TE's copy of it** (notes
  + length) and whose it is. So TE's own finger screen edits (step record, delete, clear: they pass
  DRUM ROLL's set_note hooks) go back too, also on TE's own (not yet edited) patterns; undoing a
  pattern's first DRUM ROLL edit gives it back to TE with its notes.
- Every press is a step; knob turns of one kind, the STEP REC keys of one step and TE's edits in a
  row within a second are one. 16 steps (the oldest go); a new edit drops what could be redone.
- The view goes to the pattern that changed. Not kept over a power cycle.

## Data

- Lane: sample key 1..24, length 1..64 steps, rate 1/4 1/8 1/8T **1/16** 1/16T 1/32, level 0..100,
  mute. Step: on/off + VEL 1..100 (100), TONE -24..+24 (0), LEN 1/8 .. 16 steps or **FULL**, NUDGE
  -50..+50 % (0), PROB 0..100 (100), RATCH 1..4 (1). Per sample filter OFF / LP / BP / HP, cutoff,
  resonance. Swing 0..100 (global).

## Controls

Same handling as the SDK synth engines: a knob turn edits the value under that knob, **green press =
next page**, **blue press = cursor step on / off** (any page), **white press = hear the cursor step**.

| page | blue | green | white | orange |
|---|---|---|---|---|
| P1 STEP | step | ON (right = hit, left = rest) | VEL | PROB |
| P2 NOTE | step | TONE | LEN | RATCH |
| P3 TIME | step | NUDGE | SWING (all) | ZOOM 1 / 2 / 3 / 3.5 / 4 bars |
| P4 LANE | SAMPLE (drum key) | LENGTH (steps) | RATE | MUTE |
| P5 SOUND | FILTER OFF LP BP HP | CUTOFF | RESO | LEVEL |
| P6 PAT | LENGTH (all lanes + TE) / press: clear? | (press: next page) | press: clear? | press: clear? |

| with shift | |
|---|---|
| blue turn | lane (browse) |
| green turn | finger pattern 1..14 |
| blue press (or green press) | close |
| orange press | clear the lane (again: undo) |
| white press | nothing |

- Opening shows the selected finger pattern; while open, when the finger sequencer selects another
  pattern (its key is pressed and it starts) the view follows at once (next main-loop pass, while
  the key is still held).
- Playhead: a white line under the playing step of every lane, for DRUM ROLL's patterns (its clock)
  and TE's own (TE player's step, `0x01ddc5a8 + 0x0c` per artist). While a pattern plays the view
  shows the playhead's bar(s); a knob turn or press brings the cursor's back for 2 s.
- The keyboard stays TE's (pattern keys, step record, BACK / FWD).
- P6 PAT: blue turn = the pattern length (all lanes, TE's too); a blue, white or orange press asks **CLEAR PATTERN n?** (NO picked); any knob turn picks
  (left NO, right YES), any press confirms. Leaving the screen drops the question.
- Header: pattern number, TE / DR (whose pattern), PLAY / STOP (that pattern), bar.beat, BPM, swing,
  page. Closed: nothing of DRUM ROLL over TE's screens but the "(shift arrow) BLUE: EDITOR" note on the finger
  screen (v1.3, user: the small "DRUM ROLL n" badge removed).

## Saving

Table `sdk_drumroll (id INTEGER PRIMARY KEY, data TEXT)` in TE's `user/op1.db`: rows 0-13 = patterns
1-14, row 14 = settings (shown pattern, swing, zoom, **which patterns are DRUM ROLL's** (bit mask),
the 24 filters). Base64 of `'D' 'R' version id` + raw data; **version 2** (v0.9's version-1 rows,
patterns A-D, are ignored). Written ~3 s after the last edit, or at the next main-loop pass after
closing. Safety as before: table created first, fixed SQL text plus base64 only, reads through TE's
lock and sqlite calls, loaded values clamped. TE's summary notes go into `okseq` through TE's own
mark-dirty / saver; they are values TE writes itself (53..76, -1).

## Out of scope

Following TE's reverse / random step order or TE's swing for DRUM ROLL patterns, the synth finger
sequencer, live recording into DRUM ROLL patterns (TE's finger sequencer only step-records), pattern
copy / chaining, MIDI out of the sequence.

## Code

- `dr_core.h/.c` (pure C, `test_dr_core.c`): data model, ranges, the scheduler `dr_schedule`.
- `dr_dsp.h/.c` (pure C, `test_dr_dsp.c`): filter + per-voice post-process.
- `dr_store.h/.c` (pure C, `test_dr_store.c`): base64 rows, load with range checks.
- `dr_fs.h/.c` (pure C, `test_dr_fs.c`): finger glue without TE calls — import of a TE pattern,
  the summary, a TE edit on a DRUM ROLL pattern, the clock follower (start / resync / hold / jump).
- `drumroll.c`: hooks (get_note + 4 set_note call sites), Seq tick, voice slots, keys / knobs,
  screen, op1.db.
- `sim/run_drtest.py`: the composed op1plus image against stub TE functions and an emulated
  finger sequencer (data, player state, step events).

## Plan (v1.0)

1. RE of the finger sequencer (above): data layout, writers, player step path, screen id, type.
2. Spec (this file).
3. `dr_fs` + host tests: import (keys in order, >8 keys, octave -> TONE, length), summary (first two
   hits per 1/16, other rates on the grid), TE edits (add to lane / free lane / full; remove), clock
   follower (start, steady, behind, ahead, jump).
4. `dr_store`: 14 pattern rows + settings with owned mask, version 2, two-digit ids; tests.
5. `drumroll.c`: remove drum screen / engine list / tape play; finger screen gate; 14 patterns,
   follow the selection; ownership + summary write-back; get_note / set_note hooks; per-pattern play
   state in the seq tick.
6. Manifest (hooks, claims on the seq screen, no content_sql / drum pick hook), docs (`features.md`,
   repo README); sim bench rewritten for the finger sequencer; full suite; build `op1plus`.
7. Device test: open over the finger screen with a TE pattern (import looks right), edit -> plays 8
   lanes in time with TE's step grid, two patterns at once, TE step record into a DRUM ROLL pattern,
   CLEAR, power cycle (patterns + ownership back, TE's mini view shows the summary), boot is clean.
