# PARTY synth engine (Yamaha PSS-11, feature `party`)

Status: **v0.4** (2026-10-27): the knob, the lamps and the SELECT buttons drawn antialiased
(smooth rings, pointer, round corners; engine host `eh_aa.c`), the LCD unchanged (sim-verified).
v0.3 (2026-10-26): the screen is the LCD 16×2 (after "PARTY · display overview"):
the sound, the knob turned last, lamps for B / MIX / the sounding notes, the knob and the SELECT
buttons; the knob rows below unchanged (sim-verified). v0.2 (2026-10-22): the PSS-11's AWM tone generator (GEW7) is recreated: its 8- and
12-bit samples played as stored, its envelope (attack, decay 1, decay level, decay 2, key scaling,
96 dB exponential level) and LFO (vibrato, tremolo, reverse start), tested against MAME's
per-chip-sample code. **No Yamaha sound ships with it**: without a ROM dump the engine plays 8
own built-in waves (that is what the default image sounds like, not a PSS-11). The real sounds
come from the user's own ROM dump with `gen_pss11.py --pss-rom`.

A Yamaha PortaSound PSS-11 AWM sample player: two sample layers (A/B), each with the chip's own
envelope and LFO, a low-pass filter with its own envelope, resonance, key tracking, A decay and
B fade-in, plus the usual bank on **shift + blue turn**. The screen draws the PARTY keyboard live
(after the "PARTY · overview" design) above the same parameter rows as STATION 1. The PSS-11 itself
is a 1992 32-key keyboard (GEW7 / YMW270-F: a 65C02 with a 12-slot AWM tone generator, one 256 KB
ROM for program and samples, mono) with 100 voices; the same ROM runs the PSS-21, PSS-31 and PSR-75.

## Open it on the OP-1

Part of the default `op1plus` image. Pick **PARTY** in the synth engine list, or load the preset
`party` (browser folder `party`). Sounds: **shift + blue turn** (8 built-in sounds without data;
with a ROM-derived library: every sound of the ROM's sample table, S000 ..). Knob push cycles pages.

Without `build/pss_data.inc` the engine plays 8 built-in single-cycle waves (SAW, SQUARE,
PULSE 25%, TRIANGLE, SINE, ORGAN, E.PIANO, VOICE; E.PIANO with an AWM envelope) and the bank
has 8 entries.

## Screen

After "PARTY · display overview": the screen is built around the keyboard's green-grey **LCD 16×2**,
not the whole instrument. Bench screens: `build/demo/pss_screen.png` (P02, A4 held),
`build/demo/pss_screen_fx.png` (page 4), `build/demo/pss_screen_own.png` (a changed P--).

- No header line: the LCD sits at the top.
- **LCD** (teal frame, 16 × 2 cells of 5 × 7 dots, dark on green-grey): line 1 = the sound, `02
  E.PIANO` (`*` at the end = edited) or `-- <wave A>`; line 2 = the knob turned last on this page
  (knob push starts at the blue one): its name and value (`CUTOFF      75`, `WAVE B    SINE`,
  `B PITCH     +7`). For 1 s after a sound is picked (shift + blue) or the screen opens, and on the
  FX page, line 2 shows the waves (`A SAW B SINE +7`); for P-- `SHIFT+BLUE:SOUND` until the sound is
  changed (then `B <wave> +st`). The sample loader's state on line 2 while it has one.
- **Lamps** under the LCD: green = B sounds, the red **MIX** bar, a teal lamp per sounding note (up
  to 3): a key held on the OP-1 counts until it is released (its note from its voice, rounded, so
  glide / LFO / bend do not move it); notes without a key (sequencer, MIDI) for ~145 ms.
- **Knob** (antialiased: white and grey rings, the teal pointer with a round tip in 0.1° steps, the
  green arc): the value on line 2. **SELECT**: the 12 parameters as 3 × 4 buttons (column = page 1..3,
  row = blue / green / white / orange), the current page's column white, the knob turned last teal;
  buttons and lamps with rounded, antialiased corners.
- Below, as on STATION 1 (unchanged): the page's four knobs (name and bar), `PUSH: <next page>`;
  page 4 = the sound's FX knobs.

## What it is

| page | blue | green | white | orange |
|---|---|---|---|---|
| 1 | **WAVE A** | **WAVE B** (off + waves) | **MIX** A .. B | **CUTOFF** 30 Hz .. 16 kHz, top = off |
| 2 | **B PITCH** −24..+24 st | **DETUNE** A down, B up, 0..25 ct | **F.ENV** cutoff up to +6 octaves at note on | **F.DECAY** 5 ms .. 2 s |
| 3 | **RES** | **KEY TRK** | **A DECAY** 0 = hold | **B FADE** instant / 10 ms .. 4 s |
| 4 | the sound's FX knobs (engine host) | | | |

- Page 1 lives in knob slots 1-4, so TE's LFO can move wave, mix and cutoff.
- WAVE A / B step one sound per detent; the waves are taken at note on.
- A DECAY + B FADE give the classic two-layer workstation envelopes; TE's ADSR shapes the note
  afterwards.

## Data: the PSS-11 ROM

The samples are Yamaha's, not ours. Nothing of the ROM is committed, and none is found on the
web legally (MAME, and plug-ins built on it such as Vintage Emulator Studio, ship the emulation
but not the ROM). To build the library from your own dump of a GEW7 keyboard with this sample table
(MAME `ympsr150.cpp`: `xl437a00.ic4`, 256 KB, CRC `75fba08f`: PSS-11 / PSS-21 / PSS-31 / PSR-75;
`xm041a00.ic2`, 512 KB, CRC `b2db9672`: PSR-110; `xl561a00.ic4`: PSR-150):

```sh
python3 features/party/gen_pss11.py --pss-rom /path/to/xm041a00.ic2
```

The PSR-110 dump (2026-10-22): 512 entries, 354 repeats, **158 sounds** (157 8-bit, one 12-bit, 116
tuned from a periodic loop), 449,451 bytes of sample data. `build/demo/party_rom_tour.wav` (made
during that work) plays each at C4.

This writes `build/pss_data.inc` (tables), `build/pss_rom.bin` (the sample bytes as stored in the
ROM: a content file of the update, never packed) and `build/pss_rom.inc` (host tests / benches).

What the extractor reads (the GEW7 sample table, as MAME's `gew7.cpp` `init_sample`): 512 entries of
12 bytes at `0x4000`: 21-bit start + format (bit 22 = 12-bit, two samples in three bytes, else
signed 8-bit), loop (a sample index from the start), end (`0x4000 -` the sample count; the chip
always loops end -> loop), byte 7 vibrato / tremolo / reverse, byte 8 LFO speed, bytes 9-11
attack | decay 1, decay 2 | release, key scaling | decay level. Each valid, distinct entry becomes
a sound `Snnn` (table index) with its own envelope and LFO; repeated entries share the sample bytes.
A periodic loop (whole cycles: the period L / k with the best match) tunes the sound exactly; an
unpitched one (drums, noise) plays C4 at `--rate` (default 32 kHz). A ROM sound picked from the
bank plays alone with its envelope and LFO through the keyboard's output low-pass, which MAME notes but
does not emulate (`ympsr150.cpp`: an RLC, R 150 / 120 ohm, L 3.3 mH, C 0.33 uF): CUTOFF 81 % (4.8 kHz),
RES 29 % (Q 0.67); CUTOFF fully up removes it.

Not in the table, so not yet extracted: which entries, keys, octaves and levels each of the 100
voices uses (that is the 65C02 firmware's voice table), so the bank is the ROM's sounds one by one
rather than the 100 named voices. With a dump, those tables can be found and mapped.

v0.1's extractor read loop / end as byte offsets in a 16 KB bank and converted to ADPCM; both
are gone (it would have cut every 12-bit sample at the wrong place).

The data is loaded at runtime into TE's sample memory (VisualDSP heap 1) exactly as STATION 1
does; it costs none of the 2 MB SDK region (the whole ROM is 256 KB).

## AWM: the GEW7 model

Behaviour, rate steps and LFO constants follow MAME's `src/devices/sound/gew.cpp` and `gew7.cpp`
(BSD-3-Clause, see `NOTICE`), current master: the exponential attack from about -63 dB towards a
target above the top, linear decays in a 96 dB / 1024-step level, `steps = {0x99, 0xaa, 0xcc, 0x100}
<< (i / 4)` per chip sample at 8 MHz / 72 = 111.1 kHz, decay = attack >> 5, key scaling
`(octave + KRS) x 2 + pitch bit 9`, LFO speeds 0.168 .. 7.066 Hz (as MAME steps them: speed 7 runs at
6.78 Hz), vibrato up to 79.3 cents, tremolo up to 24 dB, triangle shapes.

On the OP-1 the envelope moves once per 32 output samples (88.2 kHz) in closed form, with the decay
level crossing done per chip sample; the octave and pitch bit for the key scaling come from the note's
step. Samples are read as stored (8-bit, 12-bit), interpolated linearly, looping as the chip does;
above a step of 3 the voice reads only the samples it needs, so a block costs the same at any pitch.
TE's envelope still shapes the note and ends it: the engine host passes no key-up, so the chip's
release stage is TE's release.


## Tests

1. `test_pss_core.c` (41 checks without data; with `build/pss_data.inc` more): ADPCM decoder test
   vectors, pitch, loops, filter, every page parameter, bank, built-in waves; **GEW7 envelope**
   against MAME's per-chip-sample code (`gen_pss11.py gew_eg`, 92 settings: every attack, decay 1,
   decay 2, decay level, key scaling at 5 octaves; 34 points up to 47 s, within one 32-sample step);
   **GEW 8/12-bit decoding** through loop wraps and a reversed start; a rendered note following its
   envelope.
2. `sim/run_awmtest.py`: a synthetic GEW7 ROM (no Yamaha data) through `gen_pss11.py --pss-rom` and
   the C voice: table parsing (invalid / repeated entries), bytes as stored, loop periods (50, 41.667,
   32), pitch at A4 / C3 / key 113 (direct reads), vibrato +-79 cents at 6.78 Hz, tremolo 24 dB, and
   cycles per voice block on the Blackfin model (one sound <= 12.2k, two + every stage 22.3k of 30k).
3. `sim/run_psstest.py`: engine host tag 1 routing, pitch/restart, 4 pages, WAVE stepping, 6-bit
   fields, bank, FX page, engine-list rename (`party` → `drwave`), screens
   (`build/demo/pss_screen*.png`), the sounding note (held A4, not the voice's G4 start, none after
   release), P-- title hint, the 1 s title, the sound's number on the LCD, cycles per voice block.
4. `sim/run_pssload.py` (with `build/pss_rom.bin`): the runtime loader and its error paths.

`python3 tools/test_all.py` runs them with everything else.


## Limits

- Mono voices; TE gives engines no velocity.
- Waves are taken at note on.
- 6 OP-1 voices (the PSS-11 allows 12 notes including accompaniment).
- HARMONY / ECHO / SPLIT voice behaviour and the 100-voice table are not modelled; use two layers
  and TE's FX.
- Not verified against a real PSS-11 or a real dump yet: MAME's envelope steps are themselves
  "fudged" (its own TODO), and bytes 7-8 are marked unverified there.
- The chip's release stage and per-voice level (TL) come from the firmware; here TE's release and
  full level.
