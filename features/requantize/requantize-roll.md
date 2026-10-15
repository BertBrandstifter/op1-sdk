# Requantize piano roll — spec

Status: **v3.1** (2026-10-13: panel and roll are one screen; v3.0 same day), part of `features/requantize`; host-tested (`test_rq_roll.c`) and
sim-verified (`sim/run_rqtest.py`, roll scenario); awaiting device test.

## Problem

Requantize (see `requantize.md`) treats a take as a whole: one grid, one mode, all notes. A take often
has a few notes that need something else: one note quantized while the rest keeps its feel, a swung
note that should stay as played, a wrong note, a note held too long, a slip that STRICT/QUEUE moved
to the wrong step. There is no way to see what the quantizer did, or to fix single notes.

## Idea

The take's log becomes a **note list** (start, length, pitch per note, plus how it was played). A
**piano roll** on the tape screen shows it and edits single notes or a selection. Every note has a
timing state:

- **AUTO** (default): the panel's grid + mode quantize it, exactly as before (SNAP / QUEUE / STRICT).
- **FIXED**: the user owns its start and length; the panel's quantize leaves it alone. Notes become
  FIXED when moved, resized or snapped in the roll, or set back to "as played".

The panel's quantize runs over the AUTO notes only (FIXED and deleted notes are left out of the pass,
so QUEUE/STRICT order the remaining notes around them). Pitch edits and deletion work in both states.
Preview and commit play what the roll shows. A take that is never opened in the roll behaves exactly
as in v2.5.

## Controls

Open: ending the take (shift + blue press) opens the **requantize screen** directly: the roll, with
the take settings (grid, mode, target track) in its header (tape screen, full screen overlay; v3.0
had a separate panel). **Green press flips the knobs** between two focuses on the same screen:

- **NOTES** (default): edit notes, table below. Edits act on the **targets**: the selected notes,
  or the cursor note when nothing is selected.
- **TAKE** (header lit): quantize the whole take and commit.

So both ways stay: **whole take** = TAKE focus, grid + mode (every AUTO note follows; shift + orange
press brings every note back to AUTO); **one or many notes** = NOTES focus, orange press quantizes
the cursor note or the selection to the grid.

TAKE focus:

| input | plain (shift: the same) | with shift |
|---|---|---|
| blue turn | grid | |
| green turn | SNAP / QUEUE / STRICT | |
| white turn | target track (own track = REPLACE) | |
| orange turn | zoom | |
| blue press | — | exit requantize |
| green press | knobs back to NOTES | — |
| white press | preview (again = stop) | — |
| orange press | **commit** | **every note back to AUTO** (whole take quantized by grid + mode; pitch and delete stay) |

NOTES focus:

| input | plain | with shift |
|---|---|---|
| blue turn | cursor to previous / next note (time order) | zoom: 1 / 2 / 4 / 8 / 16 bars per screen |
| blue press | select / unselect the cursor note | exit requantize (as in the panel) |
| green turn | move targets ±5 ms | move targets ±1 grid step |
| green press | knobs to TAKE (grid, mode, track, commit) | select all / clear the selection |
| white turn | pitch ±1 semitone | pitch ±12 |
| white press | preview (again = stop), as in the panel | delete / restore targets |
| orange turn | length ±5 ms | length ±1 grid step |
| orange press | **quantize** targets: start snapped to the nearest grid point (FIXED) | toggle targets **as played** (FIXED at the played start/length) ↔ **AUTO** |

- "Grid" = the take's grid (blue knob in TAKE focus); the roll redraws as grid / mode change.
- Grid-step moves keep a note on the grid if it was on a grid point (also for triplet grids, where a
  step is fractional); off-grid notes keep their offset.
- Commit only in TAKE focus (orange press); orange press in NOTES focus only quantizes the targets.
- Knobs do nothing while a preview or commit runs (white press still stops the preview,
  shift + blue press aborts).
- Edits live as long as the requantize screen: focus changes, preview and commit keep them;
  exiting with shift + blue press drops them (the next shift + blue press starts a new take).

## Screen (320 x 156 UI)

```
 1/16 QUEUE T1 REPLACE 12/34 SEL 3               line 1 (scale 2): lit in TAKE focus
 C#4  BAR 3.2  +12MS  LEN 240MS  AUTO            line 2: cursor note (scale 2)
 |    :    :    :    |    :    :    :    |        roll: bar lines bright, beats mid, grid dim
 |  ====    [====]       ==                       notes: AUTO green, FIXED blue, selected orange,
 |       ##        ----                           cursor white frame, deleted outline only
 |^ take start (red)                    ^ end     tick under the cursor note = where it was played
 BLUE SEL GREEN TIME WHITE PITCH ORANGE LEN ...   hint line (scale 1)
```

- Opens at the smallest zoom that shows the whole take. Pitch rows fit the take's pitch range
  (2..10 px per row); taller ranges scroll to keep the cursor note visible. Time scrolls to keep
  the cursor note on screen.
- Line 2: note name (60 = C4), bar.beat of its start (4/4, from tape position 0 = TE's bar grid),
  offset to the nearest grid point (`ON GRID` / `+12MS` / `-3MS`), length, state (AUTO / FIX / DEL).
- During preview / commit a white line shows the tape position; line 1 shows the status.

## Behaviour details

- Note list: built when the take ends, from the merged keyboard + MIDI logs; note-on paired with the
  next note-off of the same key (first open first). A note-off without note-on (pressed before the
  take) is dropped; a note-on without note-off (held past the take) ends at the take's record end.
  Up to 1024 notes. Each note keeps the replay data logged with it (velocity, MIDI out flag, synth/
  drum state).
- Render (on every edit and on preview/commit): AUTO notes go through `rq_quantize` with their
  played positions and played pitches (so an unedited take quantizes identically to v2.5); results
  are written back as the notes' shown start/length. FIXED notes keep theirs. All live notes become
  the replay events (note-off before note-on at the same position).
- Shortest note `QZ_MIN_LEN` (one block); starts clamp at tape position 0; pitch 0..127.
- Replay skips events before the pass start (preview / record start = 0.25 s before the take);
  notes past the take's end are not played or recorded (as before). The roll shows the take's range.
- Drum notes: pitch changes the drum key (= another drum sound).

## Out of scope (v3.0)

Velocity editing (the OP-1 keyboard plays fixed velocity), adding new notes, undo (shift + orange
press brings timing back to AUTO / as played; pitch and delete are toggles you can turn back),
copy/paste, knob moves and sound changes during the take.

## Code

- `rq_roll.h` / `rq_roll.c` (pure C, host tests `test_rq_roll.c`): note list, build from the log,
  render, edits, time order, grid helpers.
- `rq_core.c`: `rq_sort` exported for the render.
- `requant.c`: roll view (`roll` flag on top of the PANEL state, so preview/commit/finish are
  unchanged), knob/key mapping, view (zoom, scroll), drawing; `start()` renders the note list
  instead of re-quantizing the raw log.
- `sim/run_rqtest.py`: roll scenario (open, navigate, delete, pitch, fine move, preview plays the edits).

## Plan

1. Spec (this file).
2. `rq_roll` core + host tests: build pairing, render equals v2.5 output on an unedited take, FIXED
   notes excluded from the quantize pass, every edit op, grid-step moves on triplet grids, order.
3. `requant.c`: note list at take end, render in `start()`, roll state + controls, drawing.
4. Manifest (version 3.0, sources, claims for shift + turns), `requantize.md` link.
5. Sim bench roll scenario; full suite; build `op1plus`.
6. Device test: open roll, readable at scale 1 hint line? frame cost of the full-screen overlay,
   preview + commit of an edited take.
