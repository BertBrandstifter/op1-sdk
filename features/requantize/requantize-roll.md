# Requantize piano roll — spec

Status: **v4.3** (2026-10-17): undo / redo (shift + BACK / FORWARD). **v4.2** (2026-10-17): drawing fixed (user: frames left after add + delete, rows of different heights): an added note deleted again goes for good, a deleted take note is a thin line, the cursor frame is one size, rows at least 4 px (a 1 px gap always). **v4.1** (2026-10-17): NOTE ROLL's navigation (a grid-step + pitch-row cursor, blue press = a note on / off: new notes too). **v4.0** SYNTH ROLL (2026-10-17: DRUM ROLL's design and pages, default mode CLEAN; sim-verified); v3.1 (2026-10-13: panel and roll are one screen; v3.0 same day; device-tested: "very nice"), part of `features/requantize`; host-tested (`test_rq_roll.c`) and
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

## Controls (v4.1: NOTE ROLL's navigation)

v4.1 (user, 2026-10-17: "replace the quantize editor with the piano roll editor; the navigation
shall be the one of the piano roll editor, change the quantize functions so they fit into the
pages"). The cursor is NOTE ROLL's: a **grid step** (of the take's grid) and a **pitch row**; the
note under it is the one of that pitch starting inside that step (none: an empty place). A knob turn
edits the value under that knob, **green press = next page**. Targets of an edit: the selected
notes, else the note under the cursor.

| page | blue | green | white | orange | orange press |
|---|---|---|---|---|---|
| P1 NOTE | STEP (the cursor, grid steps) | PITCH (the note; empty: the row) | VEL ±5 | LENGTH ±1 grid step | quantize targets |
| P2 TIME | NOTE: to the next / previous note | MOVE ±1 grid step | NUDGE ±5 ms | LEN MS ±5 ms | quantize targets |
| P3 QUANT | GRID | MODE CLEAN / SNAP / QUEUE / STRICT | ZOOM 1..16 bars | (FIXED count) | every note back to AUTO |
| P4 TAKE | TRACK (own = REPLACE) | | | | **commit** |

| press | plain | with shift |
|---|---|---|
| blue | **a note at the cursor on / off**: an empty place gets a new note one grid step long (FIXED, velocity 100, the take's replay data); a note added here goes for good (v4.2), a note of the take is deleted / back (shown as a thin line) | exit SYNTH ROLL (abort a preview / commit) |
| green | next page | select all / clear the selection |
| white | preview (again = stop) | select / unselect the note under the cursor |
| orange | the page's action (above) | targets as played (FIXED) <-> AUTO |

**Shift + BACK = undo, shift + FORWARD = redo** (v4.3): the take's note list after every edit is kept
(24 states, ~1500 notes over all; the oldest go), every press a step, knob turns of one kind within
a second one; selecting is no step, nor are GRID / MODE (settings: AUTO notes follow them). The
info line says UNDO / REDO / NOTHING TO UNDO.

Shift + blue turn = the cursor's pitch row (any of 0..127); the cursor follows its note when it is
moved or transposed; a new grid keeps the cursor's time.

- **CLEAN** (default): every note starts on its nearest grid point and lasts a whole number of grid
  steps (held length rounded, at least one); never before the take's record start; a key still
  sounding is cut where it starts again; the same key twice on one grid point = one note. Orange
  press "quantize" on P1 / P2 does the same for the targets (FIXED).
- SNAP / QUEUE / STRICT: the live quantizer's rules / one step per note / packed.
- Knobs do nothing while a preview or commit runs (white press still stops the preview, shift +
  blue press aborts). Edits live as long as SYNTH ROLL; exiting drops them.

## Screen (320 x 156 UI, DRUM ROLL's layout)

- Header: the take's track (orange box), SR, STOP / PLAY / REC, bar.beat (tape position while
  running, else the cursor note), BPM, page dots + "P1 NOTE".
- Roll: pitch column on the left (names; the cursor note's row lit), rows of black keys darker, a line
  under every C; bars bright, beats and grid steps dim, the take's range in red; notes AUTO green,
  FIXED blue, selected orange (velocity = brightness), deleted outline only; cursor white frame, a
  white tick where it was played; while running a white tape line and the view follows the tape.
- Panel: four knob columns (name in the knob colour, value, bar); P4 TAKE: TRACK + what commit does.
  Info line: the status, else the cursor note (name, bar.beat, ON / OFF GRID, length in grid steps,
  AUTO / FIXED / DELETED, selection count) and what the page does. Hint line as DRUM ROLL's.

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

 (shift + orange
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
