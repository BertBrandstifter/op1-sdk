# Requantize a tape take — spec

Status: **v2.4** (2026-10-12; v2.0–v2.3 same day): replace via lift + record pass with a 0.25 s pre-roll
(hides TE's record fade-in); STRICT mode; 1/32T; sim-verified against an emulated clip tape (overdub, lift, key latency);
awaiting device test.
Feature: `features/requantize` (in recipe `op1plus`).

**v3.0 (2026-10-13): piano roll** — green press in the panel opens a piano roll of the take: select
notes, move / resize / transpose / delete / quantize single notes or a selection; untouched notes
still follow the panel's grid + mode. Spec: [`requantize-roll.md`](requantize-roll.md).
**v3.1:** panel and roll are one screen (the roll opens directly, settings in its header, green press
flips the knobs between notes and take settings; commit = orange press in TAKE focus).

## Problem

The tape records audio, so a take played freely cannot be quantized afterwards: re-timing overlapping
synth notes inside audio would need heavy time-stretching with artifacts. But the notes that produced
the take are known to the SDK while they are played.

## v2.0 commit (current)

TE's tape records **overdub** (device finding: recording over audio mixes into it) and is
**clip-based** (every record pass becomes a clip with exactly its range). So:

- **target = the take's own track (default, panel shows REPLACE):** select the track (injected T key
  if needed), seek to one block inside the take (stopped), inject **lift** (cut key 10): TE removes
  the take's clip into its clipboard and the region is silent; ~0.2 s later the tape seeks to
  **0.25 s before the take** and a **normal record pass from standstill** (rec held + play)
  starts there, the quantized notes replay by tape position, and stop is injected at the take's
  **original record end** (lead = the measured key latency). Same end as the original; the clip
  starts 0.25 s early with silence (see pre-roll below). **Drop** (paste) brings the original back
  from TE's clipboard.
- **target = another track:** the same record pass (with pre-roll) on that track, no lift; the
  original gets muted.
- **Pre-roll (v2.2, device finding):** TE fades in the first moments of every recording (also a
  normal rec + play from standstill). v2.0/v2.1 started the pass exactly at the take, so the fade
  muted the first notes. v1.1's early start (0.25 s of empty audio, counted on the device as
  3 ▶▶ steps; taken for a bug and removed in v1.2) was what kept the notes out of the fade; v2.2
  brings it back (`REC_PREROLL`). On the take's own track the pre-roll records silence in overdub over whatever
  lies just before the take (existing audio stays, per TE's overdub).
- Every injected key is followed by `TE_KEY_WAKE` (else TE handles it only on the next real key
  press: that is why v1.1 did not stop by itself).
- Preview: tape from 0.25 s before the take to its end with the take's track muted; waits until TE
  actually plays before checking.
- v2.1 (device finding): TE plays the live synth through the **selected** track's gain, so with the
  take's track muted and selected the preview notes were silent (stopping the preview unmuted it:
  only the release tail was heard). Preview now selects another unmuted track meanwhile (none: the
  next track, unmuted temporarily) and restores mute + selection when it ends. The bench emulates
  this and fails without the fix.
- Consequences: TE's clipboard is overwritten with the original take; if the take was overdubbed
  onto an existing clip, lift removes that whole clip; the new clip is 0.25 s longer at the front
  (silent).

## Idea: log the take, re-perform it quantized

1. **shift + blue knob press** (1st press): a take starts. From now on every note played (keyboard + MIDI:
   pitch, velocity, note-on/off) is logged with its **tape position**. Record normally with TE's own
   controls (rec + play). Overlay: `TAKE` + a red dot while logging.
2. **shift + blue knob press** (2nd press): the take ends; the **requantize panel** opens on the tape screen:
   - **blue knob**: grid 1/2, 1/4, 1/4T, 1/8, 1/8T, 1/16 *(default)*, 1/16T, 1/32, 1/32T (v3.2: longest first, 1/4T added)
   - **green knob**: SNAP / QUEUE *(default)* / STRICT (v2.4)
     - SNAP, QUEUE: as live quantize
     - **STRICT**: notes and chords are put one grid step after the other in the order played,
       pauses removed: the first chord on its grid point (late-window rule), every next note or
       chord on the next step. Chord = note-ons within 40 ms (at most half a step) of its first
       note. Each note keeps its held length but ends at the latest where the next chord starts
       (no overlaps); the last chord keeps its full length. A take played faster than the grid
       gets longer: notes past the take's original end are not recorded (as in QUEUE).
   - **white knob**: target track — default **the take's own track = replace the take** (panel shows
     `REPLACE`); another track keeps the original (it gets muted)
   - **click white — preview**: the tape plays from 0.25 s before the take to its end with the
     take's track muted, the quantized notes play live through the current sound; click white
     again to stop
   - **click orange — commit**: the tape plays from 0.25 s before the take (not recording); exactly
     at the take's **original record start** recording is punched in (rec held while playing, TE's
     momentary punch-in), the quantized notes replay by tape position, and exactly at the original
     **record end** it punches out; 0.1 s later the tape stops by itself. Same start, end and length
     as the original take; nothing outside it is touched.
   - **click green — piano roll** (v3.0, see `requantize-roll.md`; v3.1: the panel is the roll's header, green press flips the knobs between notes and these settings)
   - **shift + blue knob press**: exit the panel (or abort a running preview/commit)
3. TE R246 has **no tape undo** (recording overwrites in place): committing onto the take's own
   track replaces it for good. To keep the original, pick another track; compare with the mutes
   (shift + T-key). The log stays until the next take, so another grid can be committed again.
4. Trigger: **shift + blue knob press** (v1.1; v1.0 used shift + stop, which is TE's tape-grid
   size cycling — now left to TE). Knob presses with shift are unused by TE's controllers; in the
   op1plus recipe requantize runs before engine_plus, whose plain knob clicks stay unaffected.

## Behaviour details

- Quantization of the log uses the same core as live quantize (`quant_core.c`, offline through
  `rq_core.c`): tape grid aligned to tape position 0 (the bar markers), SNAP/QUEUE rules (STRICT: `rq_core.c` itself), chords
  (40 ms), held lengths kept; phrase anchoring off (the tape grid is the reference).
- Only notes played while the tape **records** are logged (keyboard + MIDI, synth + drums with
  their press-time octave/shift state). Replay goes out up to one block early so it sounds on the
  grid after TE's output latency.
- The re-record uses the **current** synth sound/preset (log v1 stores notes only; knob moves and
  sound changes during the take are not reproduced).
- Log in RAM, up to 2048 events (~1000 notes); lost at power-off.
- A take with several separate record passes is treated as one range (first start to last end).
- Transport (from the tape RE): seek `0x01a55024` (only while stopped, asynchronous: the Seq tick
  waits for the position), mute flags `0xff8010a0[4]` + gains rebuild `0x01967ed0`, selected track
  `0xff802964`, state `0xff802960` (0 stopped, 1 recording, 2 playing); rec/play/stop/T keys are
  injected through TE's key queue (`TE_KEY_PUSH`) so TE's own guards apply (e.g. out of clips).

## What v1.2/v1.3 tried and why it was rolled back (device findings)

- Punch-in over the take's own track (rec held while playing = TE's momentary punch-in) **mixed**
  the quantized take into the existing audio instead of replacing it, on whatever track was
  selected — TE's punch-in appears to overdub. Replacing a take needs a different mechanism.
- Preview stopped working after the key-thread wake / start-wait changes (cause not isolated).
- v1.1 behaviour (back now): commit onto another track (original muted), tape from 0.25 s before
  the take, recording until 1.5 s after the last event; known limit: the tape may not stop by
  itself after commit (the injected stop waits in TE's key queue, see `TE_KEY_WAKE`).

## v1.2 changes (rolled back, kept for reference)

1. Commit onto the take's own track was refused (v1 safety rule) → allowed and now the default.
2. Recording started 0.25 s early and ran 1.5 s past the last note → punch-in/out over exactly the
   take's original record range (tracked while TE recorded during the take).
3. The tape did not stop by itself: injected keys only reach TE when its key thread is woken
   (`TE_KEY_WAKE` 0x019c2f04 posts the KeyHandler semaphore; `TE_KEY_PUSH` only queues). The stop
   waited for the next real key press. Every injected key is now followed by a wake; the bench
   emulates this strictly and fails without it (mutation-tested).
4. Panel text scale 2.

v1.3: preview ended immediately (it checked "is the tape playing?" on the very next block, but TE
needs a few blocks to start) → waits for the tape like commit does. Punch timing is now
self-calibrating: the blocks TE takes to react to the injected play key become the lead for the
punch-in/out keys (a fixed one-block lead was ~3 blocks late with a realistic key latency). The
bench emulates a 3-block key latency and fails without either fix (mutation-tested).

## Risks (device test)

- Key injection shares TE's key queue with the scanner (rare race); seek/record timing on the
  real tape engine; the gains rebuild is also called from the Seq thread when a preview ends.
- Only tape speed 1× is handled (positions are tape samples).

## Plan

1. ~~RE tape control~~ 2. ~~take logger + offline requantize (test_rq_core.c)~~
3. ~~panel + preview~~ 4. ~~commit + sim bench with an emulated transport (sim)~~
5. Device test.
