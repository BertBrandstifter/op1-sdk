# PLAY screens 0.17: spec and plan

Source: the design handover `op1-multitrack-handover` (screens 01 lanes, 01b stacked rows, 02 song),
drawn at 4x for a 320 x 160 display. This file maps it onto what PLAY is and can show.

## 1. Analysis: design vs. PLAY

| Design assumes | PLAY has | Decision |
|---|---|---|
| 320 x 160 | 320 x 156 (`gfx_w`, `gfx_h`) | header 18, footer 15, content 123 (4 px less than the design) |
| 4 tracks + ADD | up to 8 tracks, AUDIO / SYNTH / DRUM | 4 lanes (rows) visible, the window follows the cursor; the others listed small |
| Jost thin, big numerals | 3x5 and 5x7 bitmap fonts | new thin stroke numerals (0-9, A-F, N, O, -, ., :, /) drawn with the antialiased lines and arcs of enginehost's `eh_aa.c`; labels 3x5, values 5x7 |
| track colour = encoder order | type colours (AUD amber, SYN blue, DRM red) | design rule: list position 1..4 = blue, green, white, red (5..8 again); the type label keeps a colour (SYN white, DRM red, AUD amber) |
| simulated meter | per-track peak `py_mx.peak[k]` (post-fader, GUI clears it each frame) | dB meter -48..+6 dB, instant attack, ~24 dB/s release, white peak hold falling ~6 dB/s, fader marker, 0 dB tick |
| step strip of the current bar | NOTE ROLL / DRUM ROLL patterns, published copies in py_pattern.c | new accessor: the 16 sixteenths of the bar the head plays (stopped: bar 1 of the list's pattern), playhead step |
| `ENGINE` name | drwave tag -> SDK engine name, else TE's engine type | SYNTH: the engine name; DRUM: `DRUM`; AUDIO: the file name (`FILE`); red message when the engine cannot play |
| `PAT 1` / `SNG 5` chip | the same words today (`py_song_quick`) | chip in the track colour; a pending pattern (chosen, starts when the playing one ends) in orange |
| voices `V2 ●●○○`, ECO | V1 / V2 / V4, ECO | as designed (4 dots = the cap); `BUSY` (red) replaces it while notes find no instance |
| (nothing) | CPU % per track and in total, `!` underruns, `GONE` files, `LOADING` | kept: total in the header's context label, per track small grey `12%` |
| song: `Pnn` per row, hold lines, repeat per row | chains `00..7F` per cell, `OF` rests, AUDIO `ON` / `OF`, an empty cell ends the column, one loop section `a..b xN`, a playhead per column | cells show the chain hex in the track colour, `OF` grey, empty `· · ·`; no hold lines (rows are not a time grid); `RPT` column shows the loop's `xN` on its first row; the loop is the red gutter bracket; every column marks its own playing cell |
| song detail: pattern, pass, LEN / STEPS / RPT, note preview | chain under the cursor | big chain number, `PASS n/N` while the column plays inside the loop, facts `ROWS` / `BARS` / `ALSO` (another track using it), preview = the chain's patterns side by side with their notes / hits and a red playhead |
| tape twice | tape = the list from anywhere | tape on the list toggles LANES <-> ROWS; kept across power-off (marker file) |

The chain, songs, files and help pages keep their layout but get the new header.

## 2. Spec

### 2.1 Shared
- Colours (RGB565 of `tokens.json`): bg black; text `#F2F2F2`; muted `#8C8C8C`; lines `#1C1C1C`,
  lane borders `#141414`; meter off `#1F1F1F`; step off `#2E2E2E`; control off `#444`; disabled
  `#555`; empty cell `#3A3A3A`; selected lane / row `#0F0F16` (the design's `#0C0C11`, one step up so
  the panel shows it); playhead row `#14141B`; orange `#FF7A3D`; track colours as above. A muted
  track's name, level and meter at 35 %.
- Header (0..17, line at 17): title 5x7 tracked + context label 3x5 grey (`4 TRK · CPU 31`,
  `LOADING`); centre `BPM` + thin 10 px numerals, `BAR` + `03.2`; right: transport (outlined green
  triangle `PLAY`, white square `STOP`, red dot `REC` while rendering). Song view: `ROW 05` and the
  loop range in red (`04-07 x2`) as well.
- Footer (line at 141, text at 146, 3x5): hints `○ SOUND` with the encoder's colour ring, key names
  white, actions grey. A note, an armed cut / insert / delete or the render question replaces the
  hints (amber / red). During a render: name, percent, progress bar.

### 2.2 LANES (01)
- 4 lanes of 64 px from x 10; a lane: identity (dial 11 px + `SYN` / `SND 1`), engine block
  (`ENGINE` + 5x7 name, rule), level (thin 22 px numeral + `dB`), meter (26 segments of 1 px),
  step strip (16 cells 2 x 3), status (chip, voices, ECO), M / S rings at the bottom.
- Selected lane: background + 1 px top bar in the track colour; cut armed: red background.
- Right column (44 px): the ADD lane (dashed ring `+`, `ADD` / type, dashed box) when the window
  shows the last track and there is room; else the tracks outside the window, one small line each
  (number, type, a short meter, M).

### 2.3 ROWS (01b)
- Column header (3x5): `TRK SOURCE SEQ VOICES` dB scale `LEVEL M/S`.
- Rows of 23 px: dial, type / source, chip, voices + ECO (or `—` / CPU), meter (2 px segments,
  gap 1) + fader marker, level (thin 13 px), M / S rings. Selected: background + 1 px bar left.
- 4 rows visible + the ADD row (12 px); more tracks scroll, small arrows show more above / below.

### 2.4 SONG (02)
- Tracker (x 0..224): gutter (loop bracket, red), row number (hex, 5x7, cursor row white with
  `▶`), 4 track columns of 46 px (more tracks: the 4 around the cursor column; arrows), `RPT`.
- Column header: mini dial + `SND n` / `KIT n` / file (cursor column white, DRUM red, muted dim).
- 9 rows of 9 px. Cursor cell: white frame; cursor row: `#14141B`. A column's playing cell: its
  colour at 25 % behind it; a cue (live): blinking frame.
- Overview: `OVERVIEW 00-1F` and `0:42 / 2:08` (elapsed / the longest column once), a lane per
  track (one cell per song row in use, at least 16), the visible rows boxed, each column's playing
  row a white tick.
- Detail panel (x 226..319): mini dial + `SYN · SND 2 · ROW 05`; `CHAIN` + thin 30 px number (or
  `OF`, `ON`, `--`); `PASS 2/3` (orange) while the column plays in the loop; facts `ROWS`, `BARS`,
  `ALSO`; preview box 28 px: the chain's patterns in order, widths by length, notes (NOTE ROLL:
  pitch as height) / hits (DRUM ROLL: lane as height), bar lines, red playhead.
- LIVE: title red `LIVE`. Render question / progress in the footer line.

### 2.5 Controls (unchanged but one)
- **tape on the list: LANES <-> ROWS** (footer note `LAYOUT: LANES` / `ROWS`), written to
  `/yaffs2/user/play_s/ROWS.LAY` (present = rows) by the main loop, read at boot.
- Everything else as in README.md (list, song view).

## 3. Plan

1. `py_draw.c` / `py_draw.h` (no TE addresses): tokens, tracked text, thin numerals, dial, rings,
   chip, meter (dB from peaks, release, hold), step strip, header, footer, `py_d_list` (both
   layouts) and `py_d_song` from plain view models (`py_lm`, `py_sm`, `py_hm`).
2. `py_pattern.c`: `py_pat_bar_steps` (the bar's 16 cells + playhead) and `py_pat_chain_view`
   (patterns, lengths, notes of a chain, playhead) for the song detail.
3. `play.c`: build the list model each frame; header model; layout toggle on tape + marker file.
4. `py_view.c`: build the song model; footer texts; help page line.
5. `sim/preview.c` + `sim/preview.py`: the draw code on the host with sample models ->
   `build/demo/play_new_*.png` (seconds instead of the 4-minute bench), compared with the
   handover previews.
6. Sim bench: screenshots of both layouts and the song view, every check still passing; host tests.
7. README, feature.json (0.17.0, controls), help page.
8. Device test (user): both layouts, toggle survives a power cycle, meters move with the music,
   frame rate stays smooth while playing 4+ tracks.

## 4. After the device test (2026-11-21, the owner's feedback)

- The panel's edge hides y > ~140: no hint row at the bottom of LANES, ROWS and SONG any more;
  notes (armed keys, master, layout, the render question) appear at y 134 for a moment.
- Thin numerals below 18 px: no rounding of every stroke to whole pixels (that bent the curves),
  a slightly wider stroke and gamma-lifted edge coverage (`eh_aa_gamma`, enginehost 1.10).
- LANES, top to bottom: number + type, engine name (no ENGINE label), level + meter,
  `SND n PAT n` (5x7), then M S V ECO in one line. No step strip. No circle round the number.
- One colour per lane / row: every element in the track's colour (muted: dimmer); red only for
  warnings. M / S off: the same colour, dimmer; on: filled.
- ROWS: no column header line (the dB scale went with it), 1 px meter segments.
- Meters follow time, not the frame rate: release per audio block (~30 dB/s), the top segment
  partly lit (smooth between segments).
- SONG: the tracker alone, all columns (up to 8: 68 px each up to 4 tracks, else 34), 10 rows;
  no detail panel, no overview.
- ROWS: the type alone, `SND n` over `SNG n` (5x7, no box), voices as `V2` (no dots).
- Headers: no BAR on the play pages; the song view shows only SONG / LIVE, the loop, the transport.
- ROWS: the type in 5x7, the engine name (DRUM: KIT n, AUDIO: the file) over SNG / PAT, the
  column further left; switching the layout shows no note.
- A solo on: every track not soloed is drawn as muted (LANES, ROWS, SONG).
- SONG: the columns share the whole width (fewer tracks, wider cells); the header shows only
  each track's number (5x7, no circle, no SND); orange press there does nothing (chain view: back).
- LANES: voices as `V2` (no dots), ECO next to it.
- ECO lanes / rows: a slight green background (selected: a bit brighter).
- ECO tint removed again. LANES: M / S rings 11 px, V2 and ECO in 5x7; a 5th track shows as a lane cut off at the edge (the summary column went). ROWS: V2 over ECO in 5x7.
- ECO: black text on a green box (both play pages).
- SONG cells: thin numerals 9 px (rows 12 px, 8 on screen). CPU guard for the 4th-6th instance: ~75 % of a block (435k).
